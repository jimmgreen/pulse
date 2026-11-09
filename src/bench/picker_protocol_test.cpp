#include "../ipc/picker_protocol.h"

#include <windows.h>
#include <cstdio>
#include <iostream>
#include <string_view>

using namespace pulse::ipc::picker;
namespace {
LONG WINAPI TraceException(EXCEPTION_POINTERS* exception) {
    MEMORY_BASIC_INFORMATION memory{};
    const auto* record = exception->ExceptionRecord;
    VirtualQuery(record->ExceptionAddress, &memory, sizeof(memory));
    wchar_t module[MAX_PATH]{};
    GetModuleFileNameW(static_cast<HMODULE>(memory.AllocationBase), module, MAX_PATH);
    std::fprintf(stderr, "[EXCEPTION] code=%08lx address=%p base=%p rva=%zx module=%ls\n",
        record->ExceptionCode, record->ExceptionAddress, memory.AllocationBase,
        reinterpret_cast<uintptr_t>(record->ExceptionAddress) - reinterpret_cast<uintptr_t>(memory.AllocationBase), module);
    for (DWORD i = 0; i < record->NumberParameters; ++i)
        std::fprintf(stderr, "[EXCEPTION] parameter%lu=%zx\n", i, static_cast<size_t>(record->ExceptionInformation[i]));
#if defined(_M_IX86)
    const auto* context = exception->ContextRecord;
    std::fprintf(stderr, "[EXCEPTION] eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx esp=%08lx ebp=%08lx\n",
        context->Eax, context->Ebx, context->Ecx, context->Edx, context->Esi, context->Edi, context->Esp, context->Ebp);
    DWORD stack[16]{};
    SIZE_T copied = 0;
    if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(static_cast<uintptr_t>(context->Esp)), stack, sizeof(stack), &copied))
        for (size_t i = 0; i < 16; ++i) std::fprintf(stderr, "[EXCEPTION] stack%zu=%08lx\n", i, stack[i]);
#endif
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
int checks = 0;
int failures = 0;
void Check(bool ok, const char* name) {
    ++checks;
    if (!ok) ++failures;
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n';
}
Message Make(decltype(Message::body) body = Request{}) {
    Message m;
    m.request_id[0] = 1;
    m.body = std::move(body);
    return m;
}
void Put(std::vector<uint8_t>& b, size_t at, uint32_t n) {
    for (size_t i = 0; i < 4; ++i) b[at + i] = static_cast<uint8_t>(n >> (i * 8));
}
void RoundTrip(const Message& m, bool trace = false) {
    std::vector<uint8_t> wire;
    Message decoded;
    if (trace) std::cout << "[TRACE] encode begin\n" << std::flush;
    const auto encoded = Encode(m, wire);
    if (trace) std::cout << "[TRACE] encode end bytes=" << wire.size() << '\n' << std::flush;
    Check(encoded == CodecError::None && Decode(wire, decoded) == CodecError::None && decoded == m, "round trip");
    // Keep the failure above, but never patch a header that was not encoded.
    if (encoded != CodecError::None) return;
    for (size_t n = 0; n < wire.size(); ++n) {
        if (trace) std::cout << "[TRACE] n=" << n << " copy begin\n" << std::flush;
        auto original = decoded;
        if (trace) std::cout << "[TRACE] n=" << n << " decode begin\n" << std::flush;
        const auto error = Decode(std::span(wire).first(n), decoded);
        if (trace) std::cout << "[TRACE] n=" << n << " compare begin\n" << std::flush;
        Check(error != CodecError::None && decoded == original, "every truncation preserves output");
        if (trace) std::cout << "[TRACE] n=" << n << " complete\n" << std::flush;
    }
    if (trace) std::cout << "[TRACE] trailing-byte checks begin\n" << std::flush;
    wire.push_back(0);
    Check(Decode(wire, decoded) != CodecError::None, "trailing byte rejected");
    Put(wire, 8, static_cast<uint32_t>(wire.size() - 28));
    Check(Decode(wire, decoded) != CodecError::None, "trailing byte with adjusted length rejected");
}
void Reject(const Message& m) {
    std::vector<uint8_t> wire{42};
    Check(Encode(m, wire) != CodecError::None && wire == std::vector<uint8_t>{42}, "invalid model preserves encoded output");
}
}
void LocalOpenPolicyChecks() {
    Request request;
    request.required_capabilities |= LocalOpenAccessV1;
    request.options |= ValidateLocalOpenAccessV1;
    Message message = Make(request);
    Message decoded;
    std::vector<uint8_t> encoded;
    const auto encode_error = Encode(message, encoded);
    const auto decode_error = Decode(encoded, decoded);
    Check(encode_error == CodecError::None && decode_error == CodecError::None && decoded == message,
        "local Open V1 pair roundtrip");
    RoundTrip(message);
    request.options = 0;
    message = Make(request);
    Check(Encode(message, encoded) == CodecError::InvalidSemantics, "local capability requires matching option");
    request.options = ValidateLocalOpenAccessV1; request.required_capabilities = Open;
    message = Make(request);
    Check(Encode(message, encoded) == CodecError::InvalidSemantics, "local option requires matching capability");
    request.required_capabilities |= LocalOpenAccessV1 | ExplicitAccessPolicy;
    request.options |= SkipAccessValidation;
    message = Make(request);
    Check(Encode(message, encoded) == CodecError::InvalidSemantics, "local policy cannot waive access validation");
    request.required_capabilities = Open | LocalOpenAccessV1 | MultiSelect;
    request.options = ValidateLocalOpenAccessV1 | AllowMultiSelect;
    message = Make(request);
    Check(Encode(message, encoded) == CodecError::None, "local V1 multi-select requires both declared bits");
    RoundTrip(message);
    request.required_capabilities &= ~MultiSelect;
    Check(Encode(Make(request), encoded) == CodecError::InvalidSemantics, "local multi option requires capability");
    request.required_capabilities |= MultiSelect; request.options &= ~AllowMultiSelect;
    Check(Encode(Make(request), encoded) == CodecError::InvalidSemantics, "local multi capability requires option");
    request.options |= AllowMultiSelect | SkipAccessValidation;
    request.required_capabilities |= ExplicitAccessPolicy;
    Check(Encode(Make(request), encoded) == CodecError::InvalidSemantics, "local multi cannot waive per-file access");
    request = Request{}; request.operation = Operation::SaveFile;
    request.required_capabilities = Save | LocalOpenAccessV1; request.options = ValidateLocalOpenAccessV1;
    message = Make(request);
    Check(Encode(message, encoded) == CodecError::InvalidSemantics, "local V1 cannot change Save validation");
}
void SessionChecks() {
    const auto hello = Make(SessionHello{});
    const auto welcome = MakeSessionWelcome(hello);
    const auto request = Make();
    RoundTrip(hello);
    RoundTrip(welcome);
    RoundTrip(Make(SessionWelcome{0, 0, 0}));
    Check(ValidateSessionWelcome(hello, welcome), "session compatible welcome");
    Check(ValidateSessionRequest(hello, welcome, request), "session binds request");
    Check(!ValidateSessionWelcome(hello, Make(SessionWelcome{0, 0, 0})), "session explicit rejection");
    for (const auto body : {SessionHello{3, 4, kSessionBasic, Open}, SessionHello{1, 1, kSessionBasic, Open},
             SessionHello{2, 2, 0x80000001, Open}, SessionHello{2, 2, kSessionBasic, 0x80000001}}) {
        const auto unsupported = Make(body);
        RoundTrip(unsupported);
        const auto rejected = MakeSessionWelcome(unsupported);
        Check(std::get<SessionWelcome>(rejected.body) == SessionWelcome{0, 0, 0} &&
            !ValidateSessionWelcome(unsupported, rejected), "session incompatible requirement rejected");
        Check(!ValidateSessionWelcome(unsupported, welcome), "session unsupported hello cannot accept ordinary welcome");
    }
    auto changed = welcome;
    changed.request_id[0] = 2;
    Check(!ValidateSessionWelcome(hello, changed), "session changed welcome id rejected");
    changed = request; changed.request_id[0] = 2;
    Check(!ValidateSessionRequest(hello, welcome, changed), "session changed request id rejected");
    changed = request; std::get<Request>(changed.body).required_capabilities |= MultiSelect;
    Check(!ValidateSessionRequest(hello, welcome, changed), "session request capability drift rejected");
    for (const auto body : {SessionWelcome{3, kSessionBasic, kRequestCapabilities},
             SessionWelcome{2, 0, kRequestCapabilities}, SessionWelcome{2, kSessionBasic, 0}}) {
        const auto invalid = Make(body);
        RoundTrip(invalid);
        Check(!ValidateSessionWelcome(hello, invalid), "session missing or unsupported selection rejected");
    }
    auto future = Make(SessionWelcome{2, 0x80000001, kRequestCapabilities | 0x80000000});
    RoundTrip(future);
    Check(ValidateSessionWelcome(hello, future), "session future optional welcome bits tolerated");
    Check(!ValidateSessionWelcome(request, welcome) && !ValidateSessionWelcome(hello, request) &&
        !ValidateSessionRequest(hello, welcome, Make(Cancel{})), "session wrong kinds rejected");
    Check(std::get<SessionWelcome>(MakeSessionWelcome(request).body).version == 0,
        "session wrong hello kind produces rejection");
    Check(ValidateSessionWelcome(Make(SessionHello{1, 3, 0, Open}), welcome), "session compatible version range selects V2 basic");
    for (const auto body : {SessionHello{0, 2, 1, Open}, SessionHello{3, 2, 1, Open}, SessionHello{2, 0, 1, Open}})
        Reject(Make(body));
    Reject(Make(SessionWelcome{0, 1, 0}));
    Reject(Make(SessionWelcome{0, 0, Open}));
    changed = hello; changed.request_id = {}; Reject(changed);
    Check(!ValidateSessionWelcome(changed, welcome), "session zero hello id rejected");
    changed = welcome; changed.request_id = {}; Reject(changed);
    std::vector<uint8_t> wire;
    Encode(hello, wire);
    Put(wire, 28, 0);
    auto output = request;
    Check(Decode(wire, output) == CodecError::InvalidSemantics && output == request,
        "session malformed hello wire preserves output");
    Encode(welcome, wire); Put(wire, 28, 0);
    Check(Decode(wire, output) == CodecError::InvalidSemantics && output == request,
        "session malformed rejection wire preserves output");
    for (const auto& legacy : {request, Make(Response{}), Make(Cancel{})}) {
        RoundTrip(legacy);
        Encode(legacy, wire);
        std::vector<uint8_t> expected(28, 0);
        expected[0] = 'P'; expected[1] = 'P'; expected[2] = 'C'; expected[3] = 'K';
        expected[4] = 1; expected[6] = static_cast<uint8_t>(legacy.body.index() + 1); expected[12] = 1;
        if (std::holds_alternative<Request>(legacy.body)) {
            expected.resize(60, 0); Put(expected, 28, 1); Put(expected, 32, Open);
        } else if (std::holds_alternative<Response>(legacy.body)) {
            expected.resize(52, 0); Put(expected, 28, 1); Put(expected, 32, 2);
        }
        Put(expected, 8, static_cast<uint32_t>(expected.size() - 28));
        Check(wire == expected, "session legacy V1 wire unchanged");
    }
}
int main(int argc, char** argv) {
    SetUnhandledExceptionFilter(TraceException);
    std::cout << std::unitbuf;
    if (argc == 2 && std::string_view(argv[1]) == "--session") {
        SessionChecks();
        std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--local-open") {
        LocalOpenPolicyChecks();
        std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--max-string") {
        Request q;
        std::cout << "[TRACE] oversize assign\n";
        q.filename.assign(kMaxStringUnits + 1, u'a');
        Reject(Make(q));
        std::cout << "[TRACE] maximum assign\n";
        q.filename.assign(kMaxStringUnits, u'a');
        std::cout << "[TRACE] maximum Make\n";
        auto message = Make(q);
        std::vector<uint8_t> encoded;
        std::cout << "[TRACE] maximum Encode\n";
        Check(Encode(message, encoded) == CodecError::None, "maximum string accepted");
        std::cout << "[TRACE] maximum Decode bytes=" << encoded.size() << '\n';
        Message decoded;
        Check(Decode(encoded, decoded) == CodecError::None && decoded == message, "maximum string decoded unchanged");
        std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--unsupported-truncation") {
        Response response;
        response.status = Status::Unsupported;
        response.error_source = ErrorSource::Win32;
        response.error_code = 5;
        std::vector<uint8_t> encoded;
        Check(Encode(Make(response), encoded) == CodecError::None && encoded.size() == 52,
            "focused Unsupported response is the original 52-byte frame");
        RoundTrip(Make(response), true);
        std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
        return failures ? 1 : 0;
    }
    LocalOpenPolicyChecks();
    RoundTrip(Make());
    Request explicit_access;
    explicit_access.options = SkipAccessValidation;
    explicit_access.required_capabilities |= ExplicitAccessPolicy;
    RoundTrip(Make(explicit_access));
    auto invalid_access = explicit_access;
    invalid_access.required_capabilities &= ~ExplicitAccessPolicy;
    Reject(Make(invalid_access));
    invalid_access = explicit_access; invalid_access.options = 0;
    Reject(Make(invalid_access));
    for (auto operation : {Operation::PickFolder, Operation::SaveFile}) {
        invalid_access = explicit_access; invalid_access.operation = operation;
        invalid_access.required_capabilities |= Folder | Save;
        Reject(Make(invalid_access));
    }
    std::vector<uint8_t> access_wire;
    Encode(Make(explicit_access), access_wire);
    Put(access_wire, 32, Open);
    Message access_decoded;
    Check(Decode(access_wire, access_decoded) == CodecError::InvalidSemantics,
        "wire skip-access option cannot omit mandatory policy capability");
    Request preserve_links;
    preserve_links.required_capabilities |= PreserveLinks;
    preserve_links.options = NoDereferenceLinks;
    RoundTrip(Make(preserve_links));
    preserve_links.required_capabilities &= ~PreserveLinks;
    Reject(Make(preserve_links));
    preserve_links.required_capabilities |= PreserveLinks;
    preserve_links.options |= 0x80000000;
    Reject(Make(preserve_links));
    preserve_links.options = NoDereferenceLinks;
    std::vector<uint8_t> link_wire;
    Encode(Make(preserve_links), link_wire);
    Put(link_wire, 32, Open);
    Message ignored_link;
    Check(Decode(link_wire, ignored_link) == CodecError::InvalidSemantics, "wire no-dereference flag cannot omit capability negotiation");
    RoundTrip(Make(Cancel{}));
    Request q;
    q.required_capabilities |= MultiSelect | Filters;
    q.options = AllowMultiSelect | ShowHidden;
    q.initial_directory = u"\\\\server\\share";
    q.filename = u"\U0001f600.txt";
    q.filters = {{u"Text", u"*.txt"}, {u"All", u"*.*"}};
    q.filter_index = 1;
    RoundTrip(Make(q));
    Request advisory = q;
    advisory.options |= AdvisoryFileFilters | NoDereferenceLinks;
    advisory.required_capabilities |= AdvisoryFilters | PreserveLinks;
    RoundTrip(Make(advisory));
    std::vector<uint8_t> advisory_wire;
    Encode(Make(advisory), advisory_wire);
    Put(advisory_wire, 32, advisory.required_capabilities & ~AdvisoryFilters);
    Message decoded_advisory;
    Check(Decode(advisory_wire, decoded_advisory) == CodecError::InvalidSemantics,
        "wire advisory option without capability is rejected");
    Put(advisory_wire, 32, advisory.required_capabilities | 0x80000000u);
    Check(Decode(advisory_wire, decoded_advisory) == CodecError::UnsupportedCapability,
        "unknown mandatory capability rejects rather than silently applying strict filtering");
    auto invalid_advisory = advisory;
    invalid_advisory.required_capabilities &= ~AdvisoryFilters;
    Reject(Make(invalid_advisory));
    invalid_advisory = advisory; invalid_advisory.options &= ~AdvisoryFileFilters;
    Reject(Make(invalid_advisory));
    invalid_advisory = advisory; invalid_advisory.filters.clear(); invalid_advisory.filter_index = 0;
    Reject(Make(invalid_advisory));
    for (auto operation : {Operation::SaveFile, Operation::PickFolder}) {
        invalid_advisory = advisory; invalid_advisory.operation = operation;
        invalid_advisory.options = AdvisoryFileFilters;
        invalid_advisory.required_capabilities |= Save | Folder;
        Reject(Make(invalid_advisory));
    }
    Response advisory_reply;
    advisory_reply.status = Status::Accepted; advisory_reply.paths = {u"C:\\fixture\\manual.bin"};
    advisory_reply.filter_index = advisory.filter_index;
    Check(ValidateResponse(Make(advisory), Make(advisory_reply)), "advisory acceptance preserves actual filter index");
    advisory_reply.filter_index = 2;
    Check(!ValidateResponse(Make(advisory), Make(advisory_reply)), "advisory acceptance still rejects invalid filter index");
    Request folder;
    folder.operation = Operation::PickFolder; folder.required_capabilities = Folder;
    RoundTrip(Make(folder));
    Request save;
    save.operation = Operation::SaveFile; save.required_capabilities = Save;
    save.default_extension = u"txt"; save.filename = u"new.txt"; save.options = OverwritePrompt;
    RoundTrip(Make(save));
    Response p;
    RoundTrip(Make(p));
    p.status = Status::Accepted; p.paths = {u"C:\\a", u"C:\\b"}; p.filter_index = 1;
    RoundTrip(Make(p));
    Check(ValidateResponse(Make(q), Make(p)), "multi result matches request");
    Check(!ValidateResponse(Make(), Make(p)), "single request rejects multi result");
    p.filter_index = 2;
    Check(!ValidateResponse(Make(q), Make(p)), "response filter out of request range");
    p.filter_index = 0; p.paths.resize(1);
    auto wrong_id = Make(p); wrong_id.request_id[1] = 1;
    Check(!ValidateResponse(Make(), wrong_id), "response ID mismatch");
    p.operation = Operation::SaveFile;
    RoundTrip(Make(p));
    Check(!ValidateResponse(Make(), Make(p)), "operation mismatch");
    p.paths.push_back(u"C:\\b"); Reject(Make(p));
    p.operation = Operation::PickFolder; Reject(Make(p));
    p.paths.clear(); Reject(Make(p));
    p.status = Status::Cancelled; p.paths = {u"C:\\a"}; Reject(Make(p));
    p.paths.clear(); p.filter_index = 1; Reject(Make(p));
    p.filter_index = 0; p.status = Status::Failed; Reject(Make(p));
    p.error_source = ErrorSource::Win32; p.error_code = 5; RoundTrip(Make(p));
    p.status = Status::Unsupported; RoundTrip(Make(p));
    p.status = Status::Cancelled; Reject(Make(p));
    q = Request{}; q.options = AllowMultiSelect; Reject(Make(q));
    folder.options = AllowMultiSelect; Reject(Make(folder));
    save.options |= AllowMultiSelect; save.required_capabilities |= MultiSelect; Reject(Make(save));
    q.options = 0x80000000; Reject(Make(q));
    q = Request{}; q.required_capabilities = 0; Reject(Make(q));
    q = Request{}; q.required_capabilities |= 0x80000000; Reject(Make(q));
    q = Request{}; q.filter_index = 1; Reject(Make(q));
    q = Request{}; q.default_extension = u"txt"; Reject(Make(q));
    q = Request{}; q.options = OverwritePrompt; Reject(Make(q));
    q = Request{}; q.filename = std::u16string(1, 0xd800); Reject(Make(q));
    q.filename = std::u16string(1, 0xdc00); Reject(Make(q));
    q.filename = std::u16string{0xd800, u'A'}; Reject(Make(q));
    q.filename = std::u16string{u'A', 0, u'B'}; Reject(Make(q));
    q.filename.assign(kMaxStringUnits + 1, u'a'); Reject(Make(q));
    std::cout << "[TRACE] full maximum assign\n";
    q.filename.assign(kMaxStringUnits, u'a');
    std::cout << "[TRACE] full maximum Make\n";
    auto maximum_message = Make(q);
    std::vector<uint8_t> wire;
    std::cout << "[TRACE] full maximum Encode\n";
    // MSVC 14.51 /O2 x86 misorders arguments for the nested
    // Check(Encode(Make(q), wire) == ..., label): Encode receives label as output.
    // Keep these full expressions separate; all original assertions remain.
    const auto maximum_encoded = Encode(maximum_message, wire);
    Check(maximum_encoded == CodecError::None, "maximum string accepted");
    Message decoded;
    Check(Decode(wire, decoded) == CodecError::None, "maximum string decoded");
    q = Request{}; q.required_capabilities |= Filters; q.filters.resize(kMaxFilters + 1, {u"a", u"*"}); Reject(Make(q));
    q.filters = {{u"", u"*"}}; Reject(Make(q));
    p = Response{}; p.status = Status::Accepted; p.paths.resize(kMaxPaths + 1, u"C:\\a"); Reject(Make(p));
    p.paths.resize(kMaxPaths); p.paths.assign(kMaxPaths, std::u16string(kMaxStringUnits, u'a')); Reject(Make(p));
    auto zero_id = Make(); zero_id.request_id = {}; Reject(zero_id);
    Encode(Make(Cancel{}), wire);
    std::vector<uint8_t> golden = {0x50, 0x50, 0x43, 0x4b, 1, 0, 3, 0, 0, 0, 0, 0, 1};
    golden.resize(28);
    Check(wire == golden, "cancel golden bytes");
    Encode(Make(), wire);
    const auto base = wire;
    wire[4] = 2; Check(Decode(wire, decoded) == CodecError::UnsupportedVersion, "unknown version");
    wire = base; wire[6] = 4; Check(Decode(wire, decoded) != CodecError::None, "unknown kind");
    wire = base; wire[0] = 0; Check(Decode(wire, decoded) != CodecError::None, "bad magic");
    wire = base; Put(wire, 8, 0xffffffff); Check(Decode(wire, decoded) != CodecError::None, "overflow payload length");
    wire = base; Put(wire, 32, 0x80000001); Check(Decode(wire, decoded) == CodecError::UnsupportedCapability, "unknown required wire capability");
    wire = base; Put(wire, 28, 99); Check(Decode(wire, decoded) != CodecError::None, "unknown operation");
    wire = base; Put(wire, 40, 0xffffffff); Check(Decode(wire, decoded) != CodecError::None, "overflow string length");
    wire = base; Put(wire, 52, 0xffffffff); Check(Decode(wire, decoded) != CodecError::None, "overflow filter count");
    q = Request{}; q.initial_directory = u"A"; Encode(Make(q), wire);
    wire[44] = 0; Check(Decode(wire, decoded) != CodecError::None, "wire NUL rejected");
    wire[45] = 0xd8; Check(Decode(wire, decoded) != CodecError::None, "wire unmatched high surrogate rejected");
    wire[45] = 0xdc; Check(Decode(wire, decoded) != CodecError::None, "wire unmatched low surrogate rejected");
    wire.assign(kMaxMessageBytes + 1, 0); Check(Decode(wire, decoded) != CodecError::None, "overlarge message rejected");

    // Local save access and dialog labels (out-of-process Save).
    const auto local_save = [] {
        Request s; s.operation = Operation::SaveFile; s.required_capabilities = Save | LocalSaveAccessV1;
        s.options = OverwritePrompt | ValidateLocalSaveAccessV1; s.initial_directory = u"C:\\Work"; s.filename = u"Report";
        return s;
    };
    auto s = local_save();
    Check(Encode(Make(s), wire) == CodecError::None && Decode(wire, decoded) == CodecError::None && decoded == Make(s), "local save round trip");
    s = local_save(); s.options |= SaveNativeDefaults | SaveNoTestFileCreate | SaveRequireExistingParent;
    Check(Encode(Make(s), wire) == CodecError::None && Decode(wire, decoded) == CodecError::None && decoded == Make(s), "local save options round trip");
    s = local_save(); s.options &= ~ValidateLocalSaveAccessV1; Check(Encode(Make(s), wire) == CodecError::InvalidSemantics, "local save capability needs its option");
    s = local_save(); s.required_capabilities &= ~LocalSaveAccessV1; Check(Encode(Make(s), wire) == CodecError::InvalidSemantics, "local save option needs its capability");
    s = local_save(); s.options &= ~OverwritePrompt; Check(Encode(Make(s), wire) == CodecError::InvalidSemantics, "local save requires overwrite prompt");
    s = local_save(); s.options |= SaveSkipAccessValidation | SaveRequireUnencrypted; Check(Encode(Make(s), wire) == CodecError::InvalidSemantics, "caller save excludes unencrypted");
    s = local_save(); s.options |= SaveSkipAccessValidation | SaveNativeDefaults; Check(Encode(Make(s), wire) == CodecError::InvalidSemantics, "caller save excludes native defaults");
    s = local_save(); s.options |= SaveRequireExistingParent; Check(Encode(Make(s), wire) == CodecError::InvalidSemantics, "existing parent needs no-test-create");
    s = local_save(); s.options |= SaveNoTestFileCreate | SaveRequireUnencrypted; Check(Encode(Make(s), wire) == CodecError::InvalidSemantics, "no-test-create excludes unencrypted");
    q = Request{}; q.options = SaveNativeDefaults; Check(Encode(Make(q), wire) == CodecError::InvalidSemantics, "save option rejected on open");
    q = Request{}; q.operation = Operation::OpenFiles; q.required_capabilities = Open | LocalSaveAccessV1; q.options = ValidateLocalSaveAccessV1;
    Check(Encode(Make(q), wire) == CodecError::InvalidSemantics, "local save access rejected on open");

    s = local_save(); s.required_capabilities |= DialogLabels; s.label_flags = LabelTitle | LabelOkButton | LabelFileName;
    s.title = u"导出报告"; s.ok_button_label = u"导出"; s.file_name_label = u"名称:";
    Check(Encode(Make(s), wire) == CodecError::None && Decode(wire, decoded) == CodecError::None && decoded == Make(s), "labels round trip");
    s.label_flags = LabelTitle; s.ok_button_label.clear(); s.file_name_label.clear(); s.title.clear();
    Check(Encode(Make(s), wire) == CodecError::None && Decode(wire, decoded) == CodecError::None && decoded == Make(s), "explicit empty title round trip");
    s.label_flags = 0; Check(Encode(Make(s), wire) == CodecError::InvalidSemantics, "label capability needs flags");
    s = local_save(); s.label_flags = LabelTitle; Check(Encode(Make(s), wire) == CodecError::InvalidSemantics, "label flags need capability");
    s = local_save(); s.required_capabilities |= DialogLabels; s.label_flags = LabelTitle; s.ok_button_label = u"OK";
    Check(Encode(Make(s), wire) == CodecError::InvalidSemantics, "unset label must be empty");
    s.label_flags = LabelOkButton; s.ok_button_label = u"Line\nbreak"; Check(Encode(Make(s), wire) == CodecError::InvalidSemantics, "label control characters rejected");
    s.ok_button_label.assign(kMaxLabelUnits + 1, u'x'); Check(Encode(Make(s), wire) == CodecError::InvalidSemantics, "overlong label rejected");
    s.label_flags = 8; s.ok_button_label.clear(); Check(Encode(Make(s), wire) == CodecError::InvalidSemantics, "unknown label flag rejected");
    s = local_save(); s.required_capabilities |= DialogLabels; s.label_flags = LabelTitle; s.title = u"T";
    Encode(Make(s), wire); wire.pop_back(); wire.pop_back();
    Check(Decode(wire, decoded) != CodecError::None, "truncated label section rejected");
    s = local_save(); Encode(Make(s), wire); Put(wire, 32, Save | LocalSaveAccessV1 | DialogLabels);
    Check(Decode(wire, decoded) != CodecError::None, "label capability without label section rejected");

    // Caller window, for a standby host without a command line.
    q = Request{}; q.required_capabilities |= CallerWindow; q.caller_window = 0x0000'7ff6'1234'5678ull;
    Check(Encode(Make(q), wire) == CodecError::None && Decode(wire, decoded) == CodecError::None && decoded == Make(q), "caller window round trip (64-bit)");
    q.caller_window = 0; Check(Encode(Make(q), wire) == CodecError::InvalidSemantics, "caller window capability needs a window");
    q = Request{}; q.caller_window = 42; Check(Encode(Make(q), wire) == CodecError::InvalidSemantics, "caller window needs its capability");
    q = Request{}; q.required_capabilities |= CallerWindow; q.caller_window = 42; Encode(Make(q), wire); wire.resize(wire.size() - 4);
    Check(Decode(wire, decoded) != CodecError::None, "truncated caller window rejected");
    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures ? 1 : 0;
}

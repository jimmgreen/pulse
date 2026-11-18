#include "../ipc/picker_protocol.h"

#include <iostream>

using namespace pulse::ipc::picker;
namespace {
int checks = 0, failures = 0;
void Check(bool ok, const char* label) {
    ++checks;
    if (!ok) ++failures;
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
}
Message Make(decltype(Message::body) body) {
    Message m;
    m.request_id[0] = 1;
    m.body = std::move(body);
    return m;
}
EventSnapshot Snapshot() {
    return {0x123456789abcdef0ull, 0xfedcba9876543210ull, 1, u"C:\\fixture", u"file.txt",
        {u"C:\\fixture\\selected.txt"}, {u"C:\\fixture\\file.txt"}};
}
void RoundTrip(const Message& m) {
    std::vector<uint8_t> wire;
    Message result = Make(Cancel{});
    const auto error = Encode(m, wire);
    Check(error == CodecError::None && Decode(wire, result) == CodecError::None && result == m, "event codec roundtrip including high 64-bit values");
    if (error != CodecError::None) return;
    bool truncations = true;
    for (size_t n = 0; n < wire.size(); ++n) {
        const auto before = result;
        truncations &= Decode(std::span(wire).first(n), result) != CodecError::None && result == before;
    }
    Check(truncations, "every event frame truncation preserves output");
    wire.push_back(0);
    Check(Decode(wire, result) != CodecError::None && result == m, "event trailing byte rejected without changing output");
}
void Reject(const Message& m) {
    std::vector<uint8_t> wire{42};
    Check(Encode(m, wire) != CodecError::None && wire == std::vector<uint8_t>{42}, "invalid event model preserves output");
}
void Put(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
}
void BadWire(const Message& m, size_t offset, uint32_t value) {
    std::vector<uint8_t> bytes;
    Check(Encode(m, bytes) == CodecError::None, "malformed wire starts from valid model");
    if (bytes.size() < offset + 4) return;
    if (offset == 6) { bytes[6] = static_cast<uint8_t>(value); bytes[7] = 0; }
    else Put(bytes, offset, value);
    auto output = Make(Cancel{});
    Check(Decode(bytes, output) != CodecError::None && output == Make(Cancel{}), "malformed event wire preserves output");
}
}
int main() {
    const auto snapshot = Snapshot();
    for (const auto kind : {EventKind::Ready, EventKind::FolderChanging, EventKind::FolderChanged,
             EventKind::SelectionChanged, EventKind::TypeChanged, EventKind::FileOk})
        RoundTrip(Make(SessionEvent{0xaabbccdd11223344ull, kind, snapshot,
            kind == EventKind::FolderChanging ? u"C:\\next" : u""}));
    for (uint32_t hr : {0u, 1u, 0x80004005u, 0xffffffffu})
        RoundTrip(Make(EventReply{0xaabbccdd11223344ull, snapshot.generation, hr}));
    for (auto kind : {CommandKind::Query, CommandKind::GetWindow, CommandKind::Close}) {
        RoundTrip(Make(SessionCommand{0xaabbccdd11223344ull, kind, kind == CommandKind::Close ? 0 : snapshot.generation, 0}));
        RoundTrip(Make(CommandReply{0xaabbccdd11223344ull, kind, 0, kind == CommandKind::Close ? EventSnapshot{} : snapshot}));
        RoundTrip(Make(CommandReply{1, kind, 0x80004005, {}}));
    }
    RoundTrip(Make(SessionCommand{1, CommandKind::Close, 0, 1}));
    RoundTrip(Make(SessionTerminal{TerminalKind::Accepted, 0, 1, snapshot.generation, 1, snapshot.candidates}));
    RoundTrip(Make(SessionTerminal{}));
    RoundTrip(Make(SessionTerminal{TerminalKind::Cancelled, 0x800704c7, 1, snapshot.generation, 0, {}}));
    RoundTrip(Make(SessionTerminal{TerminalKind::Closed, 1, 1, snapshot.generation, 0, {}}));
    RoundTrip(Make(SessionTerminal{TerminalKind::Failed, 0x80004005, 0, 0, 0, {}}));
    auto event = SessionEvent{1, EventKind::Ready, snapshot, {}};
    auto bad = event; bad.event_id = 0; Reject(Make(bad));
    bad = event; bad.kind = static_cast<EventKind>(99); Reject(Make(bad));
    bad = event; bad.snapshot.generation = 0; Reject(Make(bad));
    bad = event; bad.snapshot.window = 0; Reject(Make(bad));
    bad = event; bad.snapshot.filter_index = kMaxFilters; Reject(Make(bad));
    bad = event; bad.snapshot.folder.clear(); Reject(Make(bad));
    bad = event; bad.proposed_folder = u"C:\\next"; Reject(Make(bad));
    bad = event; bad.kind = EventKind::FolderChanging; Reject(Make(bad));
    bad = event; bad.kind = EventKind::FileOk; bad.snapshot.candidates.clear(); Reject(Make(bad));
    bad = event; bad.snapshot.selections = {u""}; Reject(Make(bad));
    bad = event; bad.snapshot.candidates = {u""}; Reject(Make(bad));
    bad = event; bad.snapshot.filename.assign(1, static_cast<char16_t>(0xd800)); Reject(Make(bad));
    bad = event; bad.snapshot.folder.assign(1, static_cast<char16_t>(0xdc00)); Reject(Make(bad));
    bad = event; bad.snapshot.filename = std::u16string(u"a\0b", 3); Reject(Make(bad));
    bad = event; bad.snapshot.filename.assign(kMaxStringUnits + 1, u'x'); Reject(Make(bad));
    bad = event; bad.snapshot.selections.assign(kMaxPaths + 1, u"x"); Reject(Make(bad));
    bad = event; bad.snapshot.candidates.assign(17, std::u16string(kMaxStringUnits, u'x')); Reject(Make(bad));
    auto zero_id = Make(event); zero_id.request_id = {}; Reject(zero_id);
    Reject(Make(EventReply{0, 1, 0})); Reject(Make(EventReply{1, 0, 0}));
    Reject(Make(SessionCommand{0, CommandKind::Query, 0, 0}));
    Reject(Make(SessionCommand{1, static_cast<CommandKind>(99), 0, 0}));
    Reject(Make(SessionCommand{1, CommandKind::Query, 0, 1}));
    Reject(Make(SessionCommand{1, CommandKind::GetWindow, 0, 0x80004005}));
    Reject(Make(SessionCommand{1, CommandKind::Close, 1, 0}));
    Reject(Make(CommandReply{0, CommandKind::Query, 0, snapshot}));
    Reject(Make(CommandReply{1, static_cast<CommandKind>(99), 0, snapshot}));
    Reject(Make(CommandReply{1, CommandKind::Query, 1, snapshot}));
    Reject(Make(CommandReply{1, CommandKind::Query, 0, {}}));
    Reject(Make(CommandReply{1, CommandKind::Query, 0x80004005, snapshot}));
    Reject(Make(CommandReply{1, CommandKind::Close, 0, snapshot}));
    Reject(Make(CommandReply{1, CommandKind::Close, 1, {}}));
    for (const auto terminal : {
             SessionTerminal{TerminalKind::Accepted, 1, 1, 1, 0, {u"x"}},
             SessionTerminal{TerminalKind::Accepted, 0, 0, 0, 0, {u"x"}},
             SessionTerminal{TerminalKind::Accepted, 0, 1, 1, 0, {}},
             SessionTerminal{TerminalKind::Accepted, 0, 1, 1, kMaxFilters, {u"x"}},
             SessionTerminal{TerminalKind::Cancelled, 0, 0, 0, 0, {}},
             SessionTerminal{TerminalKind::Cancelled, 0x800704c7, 1, 0, 0, {}},
             SessionTerminal{TerminalKind::Cancelled, 0x800704c7, 0, 1, 0, {}},
             SessionTerminal{TerminalKind::Cancelled, 0x800704c7, 2, 1, 0, {}},
             SessionTerminal{TerminalKind::Cancelled, 0x800704c7, 0, 0, 1, {}},
             SessionTerminal{TerminalKind::Cancelled, 0x800704c7, 0, 0, 0, {u"x"}},
             SessionTerminal{TerminalKind::Closed, 0, 0, 0, 0, {}},
             SessionTerminal{TerminalKind::Failed, 1, 0, 0, 0, {}},
             SessionTerminal{static_cast<TerminalKind>(99), 0x80004005, 0, 0, 0, {}}}) Reject(Make(terminal));
    BadWire(Make(event), 6, 11);
    BadWire(Make(event), 36, 99);
    BadWire(Make(event), 60, 0xffffffff); // Folder string length.
    BadWire(Make(SessionCommand{1, CommandKind::Query, 0, 0}), 36, 99);
    BadWire(Make(CommandReply{1, CommandKind::Query, 0, snapshot}), 40, 1);
    BadWire(Make(SessionTerminal{}), 28, 99);
    const auto hello = Make(SessionHello{2, 2, kSessionBasic | 2, Open});
    Check(std::get<SessionWelcome>(MakeSessionWelcome(hello).body).version == 0,
        "event feature remains unadvertised and unsupported in negotiation");
    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures ? 1 : 0;
}

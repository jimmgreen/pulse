#include "picker_protocol.h"

#include <algorithm>

namespace pulse::ipc::picker {
namespace {
constexpr uint32_t kMagic = 0x4b435050; // PPCK, little endian.
constexpr uint32_t kOptions = AllowMultiSelect | ShowHidden | OverwritePrompt | NoDereferenceLinks | AdvisoryFileFilters | SkipAccessValidation | ValidateLocalOpenAccessV1 |
    ValidateLocalSaveAccessV1 | SaveNativeDefaults | SaveNoTestFileCreate | SaveRequireExistingParent | SaveRequireUnencrypted | SaveSkipAccessValidation;
constexpr uint32_t kSaveOptions = SaveNativeDefaults | SaveNoTestFileCreate | SaveRequireExistingParent | SaveRequireUnencrypted | SaveSkipAccessValidation;
bool ValidLabel(const std::u16string& s) {
    return s.size() <= kMaxLabelUnits && s.find_first_of(u"\r\n\t") == s.npos;
}
// The local-save options carry the same combination rules the in-process
// provider enforces (picker_ui_bootstrap_v2.cpp).
bool ValidLocalSave(const Request& r) {
    const bool local = r.required_capabilities & LocalSaveAccessV1;
    if (local != bool(r.options & ValidateLocalSaveAccessV1)) return false;
    if (!local) return !(r.options & kSaveOptions);
    if (r.operation != Operation::SaveFile || !(r.options & OverwritePrompt) || (r.options & NoDereferenceLinks)) return false;
    const bool caller = r.options & SaveSkipAccessValidation;
    const bool unencrypted = r.options & SaveRequireUnencrypted;
    const bool no_test = r.options & SaveNoTestFileCreate;
    if (caller && (unencrypted || (r.options & SaveNativeDefaults))) return false;
    if (no_test && (caller || unencrypted)) return false;
    if ((r.options & SaveRequireExistingParent) && !no_test) return false;
    return true;
}
bool ValidText(const std::u16string& s);
bool ValidLabels(const Request& r) {
    if (bool(r.required_capabilities & DialogLabels) != (r.label_flags != 0) || (r.label_flags & ~(LabelTitle | LabelOkButton | LabelFileName))) return false;
    if ((!(r.label_flags & LabelTitle) && !r.title.empty()) || (!(r.label_flags & LabelOkButton) && !r.ok_button_label.empty()) ||
        (!(r.label_flags & LabelFileName) && !r.file_name_label.empty())) return false;
    return ValidText(r.title) && ValidText(r.ok_button_label) && ValidText(r.file_name_label) &&
        ValidLabel(r.ok_button_label) && ValidLabel(r.file_name_label);
}
bool ValidText(const std::u16string& s) {
    if (s.size() > kMaxStringUnits) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        const auto c = s[i];
        if (!c) return false;
        if (c >= 0xd800 && c <= 0xdbff) {
            if (++i == s.size() || s[i] < 0xdc00 || s[i] > 0xdfff) return false;
        } else if (c >= 0xdc00 && c <= 0xdfff) return false;
    }
    return true;
}
bool ValidOperation(Operation op) { return op >= Operation::OpenFiles && op <= Operation::SaveFile; }
bool ValidPaths(const std::vector<std::u16string>& paths) {
    return paths.size() <= kMaxPaths && std::all_of(paths.begin(), paths.end(), [](const auto& path) {
        return !path.empty() && ValidText(path);
    });
}
bool ValidSnapshot(const EventSnapshot& s) {
    return s.generation && s.window && s.filter_index < kMaxFilters && !s.folder.empty() &&
        ValidText(s.folder) && ValidText(s.filename) && ValidPaths(s.selections) && ValidPaths(s.candidates);
}
bool FailedHResult(uint32_t hr) { return (hr & 0x80000000u) != 0; }
bool ValidCommandKind(CommandKind kind) { return kind >= CommandKind::Query && kind <= CommandKind::Close; }
CodecError Validate(const Message& m) {
    if (std::all_of(m.request_id.begin(), m.request_id.end(), [](uint8_t b) { return b == 0; }))
        return CodecError::InvalidSemantics;
    if (const auto* r = std::get_if<Request>(&m.body)) {
        if (r->required_capabilities & ~kRequestCapabilities) return CodecError::UnsupportedCapability;
        if (!ValidOperation(r->operation) || (r->options & ~kOptions) || !ValidLocalSave(*r) || !ValidLabels(*r) ||
            bool(r->required_capabilities & CallerWindow) != (r->caller_window != 0))
            return CodecError::InvalidSemantics;
        const uint32_t base = r->operation == Operation::OpenFiles ? Open : r->operation == Operation::PickFolder ? Folder : Save;
        if (!(r->required_capabilities & base) ||
            (bool(r->required_capabilities & LocalOpenAccessV1) != bool(r->options & ValidateLocalOpenAccessV1)) ||
            ((r->options & ValidateLocalOpenAccessV1) &&
                (r->operation != Operation::OpenFiles || (r->options & SkipAccessValidation) ||
                 (r->required_capabilities & ExplicitAccessPolicy) ||
                 (bool(r->options & AllowMultiSelect) != bool(r->required_capabilities & MultiSelect)))) ||
            ((r->required_capabilities & ExplicitAccessPolicy) &&
                (r->operation != Operation::OpenFiles || !(r->options & SkipAccessValidation))) ||
            ((r->options & SkipAccessValidation) &&
                (!(r->required_capabilities & ExplicitAccessPolicy) || r->operation != Operation::OpenFiles)) ||
            ((r->required_capabilities & AdvisoryFilters) &&
                (r->operation != Operation::OpenFiles || !(r->options & AdvisoryFileFilters))) ||
            ((r->options & AdvisoryFileFilters) &&
                (!(r->required_capabilities & AdvisoryFilters) || r->operation != Operation::OpenFiles || r->filters.empty())) ||
            ((r->options & NoDereferenceLinks) && !(r->required_capabilities & PreserveLinks)) ||
            ((r->options & AllowMultiSelect) && (r->operation == Operation::SaveFile || !(r->required_capabilities & MultiSelect))) ||
            ((r->options & OverwritePrompt) && r->operation != Operation::SaveFile) ||
            (!r->default_extension.empty() && r->operation != Operation::SaveFile) ||
            r->filters.size() > kMaxFilters ||
            (r->filters.empty() ? r->filter_index != 0 : r->filter_index >= r->filters.size()) ||
            (!r->filters.empty() && (!(r->required_capabilities & Filters) || r->operation == Operation::PickFolder)) ||
            !ValidText(r->initial_directory) || !ValidText(r->filename) || !ValidText(r->default_extension))
            return CodecError::InvalidSemantics;
        for (const auto& f : r->filters)
            if (f.label.empty() || f.pattern.empty() || !ValidText(f.label) || !ValidText(f.pattern)) return CodecError::InvalidSemantics;
    } else if (const auto* response = std::get_if<Response>(&m.body)) {
        if (!ValidOperation(response->operation) || response->status < Status::Accepted || response->status > Status::Failed ||
            response->error_source > ErrorSource::HResult || response->paths.size() > kMaxPaths || response->filter_index >= kMaxFilters)
            return CodecError::InvalidSemantics;
        const bool success = response->status == Status::Accepted;
        const bool error = response->status == Status::Failed || response->status == Status::Unsupported;
        if (error != (response->error_source != ErrorSource::None) || error != (response->error_code != 0) ||
            (success ? response->paths.empty() : (!response->paths.empty() || response->filter_index != 0)) ||
            (success && response->operation == Operation::SaveFile && response->paths.size() != 1)) return CodecError::InvalidSemantics;
        for (const auto& p : response->paths) if (p.empty() || !ValidText(p)) return CodecError::InvalidSemantics;
    } else if (const auto* hello = std::get_if<SessionHello>(&m.body)) {
        if (!hello->min_version || hello->min_version > hello->max_version) return CodecError::InvalidSemantics;
    } else if (const auto* welcome = std::get_if<SessionWelcome>(&m.body)) {
        if (!welcome->version && (welcome->features || welcome->capabilities)) return CodecError::InvalidSemantics;
    } else if (const auto* event = std::get_if<SessionEvent>(&m.body)) {
        if (!event->event_id || event->kind < EventKind::Ready || event->kind > EventKind::FileOk ||
            !ValidSnapshot(event->snapshot) || !ValidText(event->proposed_folder) ||
            (event->kind == EventKind::FolderChanging ? event->proposed_folder.empty() : !event->proposed_folder.empty()) ||
            (event->kind == EventKind::FileOk && event->snapshot.candidates.empty())) return CodecError::InvalidSemantics;
    } else if (const auto* event_reply = std::get_if<EventReply>(&m.body)) {
        if (!event_reply->event_id || !event_reply->generation) return CodecError::InvalidSemantics;
    } else if (const auto* command = std::get_if<SessionCommand>(&m.body)) {
        if (!command->command_id || !ValidCommandKind(command->kind) ||
            (command->kind == CommandKind::Close ? command->expected_generation != 0 : command->hresult != 0))
            return CodecError::InvalidSemantics;
    } else if (const auto* reply = std::get_if<CommandReply>(&m.body)) {
        if (!reply->command_id || !ValidCommandKind(reply->kind) || (reply->hresult && !FailedHResult(reply->hresult)) ||
            ((reply->kind == CommandKind::Close || reply->hresult) ? reply->snapshot != EventSnapshot{} : !ValidSnapshot(reply->snapshot)))
            return CodecError::InvalidSemantics;
    } else if (const auto* terminal = std::get_if<SessionTerminal>(&m.body)) {
        if (terminal->status < TerminalKind::Accepted || terminal->status > TerminalKind::Failed ||
            terminal->ui_shown > 1 || bool(terminal->ui_shown) != bool(terminal->generation) ||
            terminal->filter_index >= kMaxFilters || !ValidPaths(terminal->paths)) return CodecError::InvalidSemantics;
        if (terminal->status == TerminalKind::Accepted) {
            if (terminal->hresult || !terminal->ui_shown || terminal->paths.empty()) return CodecError::InvalidSemantics;
        } else {
            if (!terminal->paths.empty() || terminal->filter_index ||
                (terminal->status == TerminalKind::Cancelled && terminal->hresult != 0x800704c7) ||
                (terminal->status == TerminalKind::Closed && !terminal->ui_shown) ||
                (terminal->status == TerminalKind::Failed && !FailedHResult(terminal->hresult))) return CodecError::InvalidSemantics;
        }
    }
    return CodecError::None;
}
struct Writer {
    std::vector<uint8_t> data;
    bool ok = true;
    void Number(uint32_t n, size_t count = 4) {
        if (data.size() > kMaxMessageBytes - count) { ok = false; return; }
        for (size_t i = 0; i < count; ++i) data.push_back(static_cast<uint8_t>(n >> (i * 8)));
    }
    void Text(const std::u16string& s) {
        Number(static_cast<uint32_t>(s.size()));
        for (const auto c : s) Number(c, 2);
    }
    void Wide(uint64_t n) { Number(static_cast<uint32_t>(n)); Number(static_cast<uint32_t>(n >> 32)); }
    void Paths(const std::vector<std::u16string>& paths) {
        Number(static_cast<uint32_t>(paths.size()));
        for (const auto& path : paths) Text(path);
    }
    void Snapshot(const EventSnapshot& s) {
        Wide(s.generation); Wide(s.window); Number(s.filter_index); Text(s.folder); Text(s.filename);
        Paths(s.selections); Paths(s.candidates);
    }
};
struct Reader {
    std::span<const uint8_t> data;
    size_t pos = 0;
    bool ok = true;
    uint32_t Number(size_t count = 4) {
        if (count > data.size() - pos) { ok = false; return 0; }
        uint32_t n = 0;
        for (size_t i = 0; i < count; ++i) n |= static_cast<uint32_t>(data[pos++]) << (i * 8);
        return n;
    }
    std::u16string Text() {
        const auto count = Number();
        if (!ok || count > kMaxStringUnits || count > (data.size() - pos) / 2) { ok = false; return {}; }
        std::u16string s;
        s.reserve(count);
        for (uint32_t i = 0; i < count; ++i) s.push_back(static_cast<char16_t>(Number(2)));
        if (!ValidText(s)) ok = false;
        return s;
    }
    uint64_t Wide() {
        const uint64_t low = Number();
        return low | (static_cast<uint64_t>(Number()) << 32);
    }
    std::vector<std::u16string> Paths() {
        const auto count = Number();
        if (!ok || count > kMaxPaths) { ok = false; return {}; }
        std::vector<std::u16string> paths;
        for (uint32_t i = 0; i < count && ok; ++i) paths.push_back(Text());
        return paths;
    }
    EventSnapshot Snapshot() {
        EventSnapshot s;
        s.generation = Wide(); s.window = Wide(); s.filter_index = Number();
        s.folder = Text(); s.filename = Text(); s.selections = Paths(); s.candidates = Paths();
        return s;
    }
};
}

CodecError Encode(const Message& m, std::vector<uint8_t>& output) {
    const auto error = Validate(m);
    if (error != CodecError::None) return error;
    Writer w;
    w.Number(kMagic); w.Number(kVersion, 2); w.Number(static_cast<uint32_t>(m.body.index() + 1), 2);
    w.Number(0);
    for (auto b : m.request_id) w.Number(b, 1);
    if (const auto* r = std::get_if<Request>(&m.body)) {
        w.Number(static_cast<uint32_t>(r->operation)); w.Number(r->required_capabilities); w.Number(r->options);
        w.Text(r->initial_directory); w.Text(r->filename); w.Text(r->default_extension);
        w.Number(static_cast<uint32_t>(r->filters.size()));
        for (const auto& f : r->filters) { w.Text(f.label); w.Text(f.pattern); }
        w.Number(r->filter_index);
        if (r->required_capabilities & DialogLabels) {
            w.Number(r->label_flags); w.Text(r->title); w.Text(r->ok_button_label); w.Text(r->file_name_label);
        }
        if (r->required_capabilities & CallerWindow) {
            w.Number(static_cast<uint32_t>(r->caller_window)); w.Number(static_cast<uint32_t>(r->caller_window >> 32));
        }
    } else if (const auto* response = std::get_if<Response>(&m.body)) {
        w.Number(static_cast<uint32_t>(response->operation)); w.Number(static_cast<uint32_t>(response->status));
        w.Number(static_cast<uint32_t>(response->error_source)); w.Number(response->error_code); w.Number(response->filter_index);
        w.Number(static_cast<uint32_t>(response->paths.size()));
        for (const auto& p : response->paths) w.Text(p);
    } else if (const auto* hello = std::get_if<SessionHello>(&m.body)) {
        w.Number(hello->min_version); w.Number(hello->max_version);
        w.Number(hello->required_features); w.Number(hello->required_capabilities);
    } else if (const auto* welcome = std::get_if<SessionWelcome>(&m.body)) {
        w.Number(welcome->version); w.Number(welcome->features); w.Number(welcome->capabilities);
    } else if (const auto* event = std::get_if<SessionEvent>(&m.body)) {
        w.Wide(event->event_id); w.Number(static_cast<uint32_t>(event->kind)); w.Snapshot(event->snapshot); w.Text(event->proposed_folder);
    } else if (const auto* event_reply = std::get_if<EventReply>(&m.body)) {
        w.Wide(event_reply->event_id); w.Wide(event_reply->generation); w.Number(event_reply->hresult);
    } else if (const auto* command = std::get_if<SessionCommand>(&m.body)) {
        w.Wide(command->command_id); w.Number(static_cast<uint32_t>(command->kind));
        w.Wide(command->expected_generation); w.Number(command->hresult);
    } else if (const auto* reply = std::get_if<CommandReply>(&m.body)) {
        w.Wide(reply->command_id); w.Number(static_cast<uint32_t>(reply->kind)); w.Number(reply->hresult); w.Snapshot(reply->snapshot);
    } else if (const auto* terminal = std::get_if<SessionTerminal>(&m.body)) {
        w.Number(static_cast<uint32_t>(terminal->status)); w.Number(terminal->hresult); w.Number(terminal->ui_shown);
        w.Wide(terminal->generation); w.Number(terminal->filter_index); w.Paths(terminal->paths);
    }
    if (!w.ok) return CodecError::Malformed;
    const auto length = static_cast<uint32_t>(w.data.size() - 28);
    for (size_t i = 0; i < 4; ++i) w.data[8 + i] = static_cast<uint8_t>(length >> (i * 8));
    output = std::move(w.data);
    return CodecError::None;
}

CodecError Decode(std::span<const uint8_t> bytes, Message& output) {
    if (bytes.size() < 28 || bytes.size() > kMaxMessageBytes) return CodecError::Malformed;
    Reader r{bytes};
    if (r.Number() != kMagic) return CodecError::Malformed;
    if (r.Number(2) != kVersion) return CodecError::UnsupportedVersion;
    const auto kind = r.Number(2);
    if (kind < 1 || kind > 10 || r.Number() != bytes.size() - 28) return CodecError::Malformed;
    Message m;
    for (auto& b : m.request_id) b = static_cast<uint8_t>(r.Number(1));
    if (kind == 1) {
        Request q;
        q.operation = static_cast<Operation>(r.Number()); q.required_capabilities = r.Number(); q.options = r.Number();
        q.initial_directory = r.Text(); q.filename = r.Text(); q.default_extension = r.Text();
        const auto count = r.Number();
        if (!r.ok || count > kMaxFilters) return CodecError::Malformed;
        for (uint32_t i = 0; i < count && r.ok; ++i) q.filters.push_back({r.Text(), r.Text()});
        q.filter_index = r.Number();
        if (q.required_capabilities & DialogLabels) {
            q.label_flags = r.Number(); q.title = r.Text(); q.ok_button_label = r.Text(); q.file_name_label = r.Text();
        }
        if (q.required_capabilities & CallerWindow) {
            const uint64_t low = r.Number();
            q.caller_window = low | (static_cast<uint64_t>(r.Number()) << 32);
        }
        m.body = std::move(q);
    } else if (kind == 2) {
        Response p;
        p.operation = static_cast<Operation>(r.Number()); p.status = static_cast<Status>(r.Number());
        p.error_source = static_cast<ErrorSource>(r.Number()); p.error_code = r.Number(); p.filter_index = r.Number();
        const auto count = r.Number();
        if (!r.ok || count > kMaxPaths) return CodecError::Malformed;
        for (uint32_t i = 0; i < count && r.ok; ++i) p.paths.push_back(r.Text());
        m.body = std::move(p);
    } else if (kind == 3) m.body = Cancel{};
    else if (kind == 4) m.body = SessionHello{r.Number(), r.Number(), r.Number(), r.Number()};
    else if (kind == 5) m.body = SessionWelcome{r.Number(), r.Number(), r.Number()};
    else if (kind == 6) {
        SessionEvent event;
        event.event_id = r.Wide(); event.kind = static_cast<EventKind>(r.Number());
        event.snapshot = r.Snapshot(); event.proposed_folder = r.Text(); m.body = std::move(event);
    } else if (kind == 7) m.body = EventReply{r.Wide(), r.Wide(), r.Number()};
    else if (kind == 8) m.body = SessionCommand{r.Wide(), static_cast<CommandKind>(r.Number()), r.Wide(), r.Number()};
    else if (kind == 9) {
        CommandReply reply;
        reply.command_id = r.Wide(); reply.kind = static_cast<CommandKind>(r.Number());
        reply.hresult = r.Number(); reply.snapshot = r.Snapshot(); m.body = std::move(reply);
    } else {
        SessionTerminal terminal;
        terminal.status = static_cast<TerminalKind>(r.Number()); terminal.hresult = r.Number(); terminal.ui_shown = r.Number();
        terminal.generation = r.Wide(); terminal.filter_index = r.Number(); terminal.paths = r.Paths(); m.body = std::move(terminal);
    }
    if (!r.ok || r.pos != bytes.size()) return CodecError::Malformed;
    const auto error = Validate(m);
    if (error == CodecError::None) output = std::move(m);
    return error;
}

Message MakeSessionWelcome(const Message& hello) {
    Message welcome;
    welcome.request_id = hello.request_id;
    welcome.body = SessionWelcome{0, 0, 0};
    const auto* h = std::get_if<SessionHello>(&hello.body);
    if (h && Validate(hello) == CodecError::None && h->min_version <= kSessionVersion && h->max_version >= kSessionVersion &&
        !(h->required_features & ~kSessionFeatures) && !(h->required_capabilities & ~kRequestCapabilities))
        welcome.body = SessionWelcome{};
    return welcome;
}

bool ValidateSessionWelcome(const Message& hello, const Message& welcome) {
    const auto* h = std::get_if<SessionHello>(&hello.body);
    const auto* w = std::get_if<SessionWelcome>(&welcome.body);
    return h && w && hello.request_id == welcome.request_id && Validate(hello) == CodecError::None &&
        Validate(welcome) == CodecError::None && w->version == kSessionVersion &&
        h->min_version <= w->version && h->max_version >= w->version && (w->features & kSessionBasic) &&
        !(h->required_features & ~kSessionFeatures) && !(h->required_capabilities & ~kRequestCapabilities) &&
        !(h->required_features & ~w->features) && !(h->required_capabilities & ~w->capabilities);
}

bool ValidateSessionRequest(const Message& hello, const Message& welcome, const Message& request) {
    const auto* r = std::get_if<Request>(&request.body);
    return r && ValidateSessionWelcome(hello, welcome) && hello.request_id == request.request_id &&
        Validate(request) == CodecError::None &&
        r->required_capabilities == std::get<SessionHello>(hello.body).required_capabilities;
}

bool ValidateResponse(const Message& request, const Message& response) {
    const auto* q = std::get_if<Request>(&request.body);
    const auto* p = std::get_if<Response>(&response.body);
    if (!q || !p || request.request_id != response.request_id || q->operation != p->operation ||
        Validate(request) != CodecError::None || Validate(response) != CodecError::None) return false;
    return p->status != Status::Accepted ||
        ((p->paths.size() == 1 || (q->options & AllowMultiSelect)) &&
         (q->filters.empty() ? p->filter_index == 0 : p->filter_index < q->filters.size()));
}
} // namespace pulse::ipc::picker

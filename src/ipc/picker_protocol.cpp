#include "picker_protocol.h"

#include <algorithm>

namespace pulse::ipc::picker {
namespace {
constexpr uint32_t kMagic = 0x4b435050; // PPCK, little endian.
constexpr uint32_t kCapabilities = Open | Folder | Save | MultiSelect | Filters | PreserveLinks | AdvisoryFilters | ExplicitAccessPolicy | LocalOpenAccessV1;
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
CodecError Validate(const Message& m) {
    if (std::all_of(m.request_id.begin(), m.request_id.end(), [](uint8_t b) { return b == 0; }))
        return CodecError::InvalidSemantics;
    if (const auto* r = std::get_if<Request>(&m.body)) {
        if (r->required_capabilities & ~kCapabilities) return CodecError::UnsupportedCapability;
        if (!ValidOperation(r->operation) || (r->options & ~(AllowMultiSelect | ShowHidden | OverwritePrompt | NoDereferenceLinks | AdvisoryFileFilters | SkipAccessValidation | ValidateLocalOpenAccessV1)))
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
    } else if (const auto* response = std::get_if<Response>(&m.body)) {
        w.Number(static_cast<uint32_t>(response->operation)); w.Number(static_cast<uint32_t>(response->status));
        w.Number(static_cast<uint32_t>(response->error_source)); w.Number(response->error_code); w.Number(response->filter_index);
        w.Number(static_cast<uint32_t>(response->paths.size()));
        for (const auto& p : response->paths) w.Text(p);
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
    if (kind < 1 || kind > 3 || r.Number() != bytes.size() - 28) return CodecError::Malformed;
    Message m;
    for (auto& b : m.request_id) b = static_cast<uint8_t>(r.Number(1));
    if (kind == 1) {
        Request q;
        q.operation = static_cast<Operation>(r.Number()); q.required_capabilities = r.Number(); q.options = r.Number();
        q.initial_directory = r.Text(); q.filename = r.Text(); q.default_extension = r.Text();
        const auto count = r.Number();
        if (!r.ok || count > kMaxFilters) return CodecError::Malformed;
        for (uint32_t i = 0; i < count && r.ok; ++i) q.filters.push_back({r.Text(), r.Text()});
        q.filter_index = r.Number(); m.body = std::move(q);
    } else if (kind == 2) {
        Response p;
        p.operation = static_cast<Operation>(r.Number()); p.status = static_cast<Status>(r.Number());
        p.error_source = static_cast<ErrorSource>(r.Number()); p.error_code = r.Number(); p.filter_index = r.Number();
        const auto count = r.Number();
        if (!r.ok || count > kMaxPaths) return CodecError::Malformed;
        for (uint32_t i = 0; i < count && r.ok; ++i) p.paths.push_back(r.Text());
        m.body = std::move(p);
    } else m.body = Cancel{};
    if (!r.ok || r.pos != bytes.size()) return CodecError::Malformed;
    const auto error = Validate(m);
    if (error == CodecError::None) output = std::move(m);
    return error;
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

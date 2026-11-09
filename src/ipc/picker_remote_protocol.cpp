#include "picker_remote_protocol.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace pulse::ipc::picker::remote {
namespace {
bool TextValid(const std::wstring &s) {
    if (s.size() > PULSE_PICKER_UI_V2_MAX_UNITS)
        return false;
    for (size_t i = 0; i < s.size(); ++i) {
        auto c = static_cast<uint16_t>(s[i]);
        if (!c)
            return false;
        if (c >= 0xd800 && c <= 0xdbff) {
            if (++i == s.size() || s[i] < 0xdc00 || s[i] > 0xdfff)
                return false;
        } else if (c >= 0xdc00 && c <= 0xdfff)
            return false;
    }
    return true;
}
struct Writer {
    std::vector<uint8_t> bytes;
    bool ok = true;
    void operator()(uint32_t &v) {
        if (bytes.size() + 4 > kMaxFrame) {
            ok = false;
            return;
        }
        for (int i = 0; i < 4; ++i)
            bytes.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
    void operator()(uint64_t &v) {
        uint32_t lo = static_cast<uint32_t>(v), hi = static_cast<uint32_t>(v >> 32);
        (*this)(lo);
        (*this)(hi);
    }
    void operator()(std::wstring &s) {
        if (!TextValid(s)) {
            ok = false;
            return;
        }
        uint32_t n = static_cast<uint32_t>(s.size());
        (*this)(n);
        if (bytes.size() + 2 * n > kMaxFrame) {
            ok = false;
            return;
        }
        for (auto c : s) {
            bytes.push_back(static_cast<uint8_t>(c));
            bytes.push_back(static_cast<uint8_t>(c >> 8));
        }
    }
    template <class T> void operator()(std::vector<T> &v) {
        if (v.size() > 1024) {
            ok = false;
            return;
        }
        uint32_t n = static_cast<uint32_t>(v.size());
        (*this)(n);
        for (auto &x : v)
            (*this)(x);
    }
    template <class T> void operator()(T &v) { Fields(*this, v); }
};
struct Reader {
    std::span<const uint8_t> bytes;
    size_t pos = 0;
    bool ok = true;
    void operator()(uint32_t &v) {
        if (bytes.size() - pos < 4) {
            ok = false;
            return;
        }
        v = 0;
        for (int i = 0; i < 4; ++i)
            v |= uint32_t(bytes[pos++]) << (8 * i);
    }
    void operator()(uint64_t &v) {
        uint32_t lo = 0, hi = 0;
        (*this)(lo);
        (*this)(hi);
        v = lo | (uint64_t(hi) << 32);
    }
    void operator()(std::wstring &s) {
        uint32_t n = 0;
        (*this)(n);
        if (!ok || n > PULSE_PICKER_UI_V2_MAX_UNITS || n > (bytes.size() - pos) / 2) {
            ok = false;
            return;
        }
        s.clear();
        s.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            s.push_back(wchar_t(bytes[pos] | (uint16_t(bytes[pos + 1]) << 8)));
            pos += 2;
        }
        if (!TextValid(s))
            ok = false;
    }
    template <class T> void operator()(std::vector<T> &v) {
        uint32_t n = 0;
        (*this)(n);
        if (!ok || n > 1024) {
            ok = false;
            return;
        }
        v.resize(n);
        for (auto &x : v) {
            (*this)(x);
            if (!ok)
                break;
        }
    }
    template <class T> void operator()(T &v) { Fields(*this, v); }
};
template <class A, class... T> void Visit(A &a, T &...v) { (a(v), ...); }
template <class A> void Fields(A &a, Filter &v) { Visit(a, v.label, v.pattern); }
template <class A> void Fields(A &a, ControlItem &v) { Visit(a, v.id, v.state, v.label); }
template <class A> void Fields(A &a, Control &v) {
    Visit(a, v.kind, v.id, v.parent_id, v.has_parent, v.state, v.label, v.items, v.has_selected, v.selected_item_id,
          v.checked, v.text);
}
template <class A> void Fields(A &a, Place &v) { Visit(a, v.location, v.label, v.path); }
template <class A> void Fields(A &a, Property &v) { Visit(a, v.index, v.kind, v.label, v.values); }
template <class A> void Fields(A &a, Config &v) {
    Visit(a, v.version, v.required_capabilities, v.appearance, v.open_options, v.access_policy, v.owner,
          v.initial_folder, v.filename, v.title, v.filters, v.filter_index, v.controls, v.default_extension,
          v.save_options, v.label_flags, v.ok_button_label, v.file_name_label, v.places, v.properties,
          v.native_options);
}
template <class A> void Fields(A &a, Snapshot &v) {
    Visit(a, v.flags, v.show_id, v.generation, v.window, v.filter_index, v.folder, v.filename, v.selection, v.candidate,
          v.selection_paths, v.candidate_paths);
}
template <class A> void Fields(A &a, Event &v) {
    Visit(a, v.kind, v.show_id, v.generation, v.window, v.proposed_folder, v.control_id, v.item_id, v.control_text,
          v.property_values, v.navigation_result, v.target);
}
template <class A> void Fields(A &a, Result &v) {
    Visit(a, v.status, v.flags, v.win32_error, v.terminal_hr, v.filter_index, v.show_id, v.generation, v.path, v.paths);
}
bool Valid(const Control &c) {
    if (c.kind < 1 || c.kind > 8 || c.has_parent > 1 || c.has_selected > 1 || c.checked > 1 || (c.state & ~7u) ||
        c.items.size() > 32)
        return false;
    for (auto &i : c.items)
        if (i.state & ~3u)
            return false;
    return true;
}
bool Valid(const Config &c) {
    if (c.version != 2 || (c.required_capabilities & ~0x3fu) || (c.appearance & ~1u) || (c.open_options & ~0x7fu) ||
        c.access_policy < 1 || c.access_policy > 3 || (c.save_options & ~0x3fu) || (c.label_flags & ~7u) ||
        (c.native_options & ~PULSE_PICKER_UI_V2_NATIVE_OPTIONS_MASK) || c.filters.size() > 64 ||
        c.controls.size() > 16 || c.places.size() > 64 || c.properties.size() > 16 ||
        (c.filters.empty() ? c.filter_index != 0 : c.filter_index >= c.filters.size()))
        return false;
    size_t units = c.initial_folder.size() + c.filename.size() + c.title.size() + c.default_extension.size() +
                   c.ok_button_label.size() + c.file_name_label.size();
    for (auto &f : c.filters) {
        if (f.label.empty() || f.pattern.empty())
            return false;
        units += f.label.size() + f.pattern.size();
    }
    for (auto &x : c.controls) {
        if (!Valid(x))
            return false;
        units += x.label.size() + x.text.size();
        for (auto &i : x.items)
            units += i.label.size();
    }
    for (auto &p : c.places) {
        if (p.location > 1 || p.path.empty())
            return false;
        units += p.label.size() + p.path.size();
    }
    for (auto &p : c.properties) {
        if (p.kind < 1 || p.kind > 2 || p.values.size() > 16)
            return false;
        units += p.label.size();
        for (auto &v : p.values)
            units += v.size();
    }
    return units <= PULSE_PICKER_UI_V2_MAX_INPUT_UNITS;
}
bool PathsValid(const std::vector<std::wstring> &p) {
    size_t n = 0;
    if (p.size() > 1024)
        return false;
    for (auto &x : p) {
        if (x.empty())
            return false;
        n += x.size() + 1;
    }
    return n <= PULSE_PICKER_UI_V2_MAX_PACKED_UNITS;
}
bool Valid(const Snapshot &s) {
    return !(s.flags & ~7u) && s.filter_index < 64 && PathsValid(s.selection_paths) && PathsValid(s.candidate_paths);
}
bool Valid(const Result &r) {
    return r.status >= 1 && r.status <= 4 && !(r.flags & ~1u) && r.filter_index < 64 && PathsValid(r.paths) &&
           (r.status == 1 ? (!r.paths.empty() && r.path == r.paths[0]) : (r.paths.empty() && r.path.empty()));
}
bool Valid(const Message &m) {
    if (!m.sequence || std::all_of(m.request_id.begin(), m.request_id.end(), [](auto b) { return !b; }) ||
        m.kind < Kind::ValidateConfig || m.kind > Kind::Hello)
        return false;
    switch (m.kind) {
    case Kind::Hello:
        return m.version == 2;
    case Kind::ValidateConfig:
    case Kind::Show:
        return Valid(m.config);
    case Kind::UpdateControl:
        return Valid(m.control);
    case Kind::Event:
        return m.event.kind >= 1 && m.event.kind <= 14 && m.event.property_values.size() <= 16;
    case Kind::Overwrite:
    case Kind::ShareViolation:
        return !m.event.target.empty();
    case Kind::EventReply:
        return m.response <= 2;
    case Kind::Reply:
        return Valid(m.snapshot);
    case Kind::Terminal:
        return Valid(m.result);
    default:
        return true;
    }
}
template <class A> void Payload(A &a, Message &m) {
    switch (m.kind) {
    case Kind::ValidateConfig:
    case Kind::Show:
        a(m.config);
        break;
    case Kind::Event:
    case Kind::Overwrite:
    case Kind::ShareViolation:
        a(m.event);
        break;
    case Kind::EventReply:
        a(m.response);
        break;
    case Kind::Query:
        Visit(a, m.show_id, m.expected_generation);
        break;
    case Kind::GetWindow:
    case Kind::Close:
        a(m.show_id);
        break;
    case Kind::UpdateControl:
        Visit(a, m.show_id, m.control);
        break;
    case Kind::Navigate:
        Visit(a, m.show_id, m.folder);
        break;
    case Kind::Hello:
        a(m.version);
        break;
    case Kind::Reply:
        Visit(a, m.snapshot, m.window, m.response, m.version, m.capabilities, m.features);
        break;
    case Kind::Terminal:
        a(m.result);
        break;
    }
}
PulsePickerUiV2Text Text(const std::wstring &s) { return {s.data(), static_cast<uint32_t>(s.size())}; }
bool Read(PulsePickerUiV2Text t, std::wstring &out) {
    if (t.units > 32767 || (!t.data && t.units))
        return false;
    out = t.units ? std::wstring(t.data, t.units) : std::wstring{};
    return TextValid(out);
}
bool Read(const PulsePickerUiV2Buffer &b, std::wstring &out) {
    return b.units < b.capacity && Read(PulsePickerUiV2Text{b.data, b.units}, out) || (!b.units && (out.clear(), true));
}
bool Packed(const PulsePickerUiV2Buffer &b, uint32_t count, std::vector<std::wstring> &out) {
    if (count > 1024 || b.units > 524288 || (!b.data && b.units) || b.units >= b.capacity && b.units)
        return false;
    out.clear();
    size_t p = 0;
    while (p < b.units) {
        size_t end = p;
        while (end < b.units && b.data[end])
            ++end;
        if (end == b.units || end == p)
            return false;
        std::wstring s(b.data + p, end - p);
        if (!TextValid(s))
            return false;
        out.push_back(std::move(s));
        p = end + 1;
    }
    return out.size() == count;
}
std::wstring Pack(const std::vector<std::wstring> &paths) {
    std::wstring s;
    for (auto &p : paths) {
        s += p;
        s.push_back(0);
    }
    return s;
}
} // namespace
bool Encode(const Message &input, std::vector<uint8_t> &output) {
    try {
        if (!Valid(input))
            return false;
        auto m = input;
        Writer w;
        uint32_t magic = 0x32525050, version = 1, kind = static_cast<uint32_t>(m.kind), length = 0;
        Visit(w, magic, version, length, kind, m.sequence, m.hresult);
        for (auto b : m.request_id)
            w.bytes.push_back(b);
        Payload(w, m);
        if (!w.ok)
            return false;
        length = static_cast<uint32_t>(w.bytes.size());
        for (int i = 0; i < 4; ++i)
            w.bytes[8 + i] = static_cast<uint8_t>(length >> (8 * i));
        output = std::move(w.bytes);
        return true;
    } catch (...) {
        return false;
    }
}
bool Decode(std::span<const uint8_t> bytes, Message &output) {
    try {
        if (bytes.size() < 44 || bytes.size() > kMaxFrame)
            return false;
        Reader r{bytes};
        uint32_t magic = 0, version = 0, length = 0, kind = 0;
        Message m;
        Visit(r, magic, version, length, kind, m.sequence, m.hresult);
        if (magic != 0x32525050 || version != 1 || length != bytes.size() || kind < 1 || kind > 14)
            return false;
        m.kind = static_cast<Kind>(kind);
        for (auto &b : m.request_id)
            b = bytes[r.pos++];
        Payload(r, m);
        if (!r.ok || r.pos != bytes.size() || !Valid(m))
            return false;
        output = std::move(m);
        return true;
    } catch (...) {
        return false;
    }
}
bool FromAbi(const PulsePickerUiV2Control &a, Control &out) {
    try {
        if (a.size != sizeof(a) || a.reserved[0] || a.reserved[1] || a.item_count > 32 || (!a.items && a.item_count))
            return false;
        Control c;
        c.kind = a.kind;
        c.id = a.id;
        c.parent_id = a.parent_id;
        c.has_parent = a.has_parent;
        c.state = a.state;
        c.has_selected = a.has_selected;
        c.selected_item_id = a.selected_item_id;
        c.checked = a.checked;
        if (!Read(a.label, c.label) || !Read(a.text, c.text))
            return false;
        for (uint32_t i = 0; i < a.item_count; ++i) {
            auto &x = a.items[i];
            if (x.size != sizeof(x) || x.reserved)
                return false;
            ControlItem item{x.id, x.state, {}};
            if (!Read(x.label, item.label))
                return false;
            c.items.push_back(std::move(item));
        }
        if (!Valid(c))
            return false;
        out = std::move(c);
        return true;
    } catch (...) {
        return false;
    }
}
bool FromAbi(const PulsePickerUiV2Config &a, Config &out) {
    try {
        if (a.size != sizeof(a) || a.reserved[0] || a.reserved[1] || a.filter_count > 64 || a.control_count > 16 ||
            a.place_count > 64 || a.property_count > 16 || (!a.filters && a.filter_count) ||
            (!a.controls && a.control_count) || (!a.places && a.place_count) || (!a.properties && a.property_count))
            return false;
        Config c;
        c.version = a.version;
        c.required_capabilities = a.required_capabilities;
        c.appearance = a.appearance;
        c.open_options = a.open_options;
        c.access_policy = a.access_policy;
        c.owner = reinterpret_cast<uintptr_t>(a.owner);
        c.filter_index = a.filter_index;
        c.save_options = a.save_options;
        c.label_flags = a.label_flags;
        c.native_options = a.native_options;
        if (!Read(a.initial_folder, c.initial_folder) || !Read(a.filename, c.filename) || !Read(a.title, c.title) ||
            !Read(a.default_extension, c.default_extension) || !Read(a.ok_button_label, c.ok_button_label) ||
            !Read(a.file_name_label, c.file_name_label))
            return false;
        for (uint32_t i = 0; i < a.filter_count; ++i) {
            auto &x = a.filters[i];
            Filter f;
            if (x.size != sizeof(x) || x.reserved || !Read(x.label, f.label) || !Read(x.pattern, f.pattern))
                return false;
            c.filters.push_back(std::move(f));
        }
        for (uint32_t i = 0; i < a.control_count; ++i) {
            Control x;
            if (!FromAbi(a.controls[i], x))
                return false;
            c.controls.push_back(std::move(x));
        }
        for (uint32_t i = 0; i < a.place_count; ++i) {
            auto &x = a.places[i];
            Place p;
            p.location = x.location;
            if (x.size != sizeof(x) || !Read(x.label, p.label) || !Read(x.path, p.path))
                return false;
            c.places.push_back(std::move(p));
        }
        for (uint32_t i = 0; i < a.property_count; ++i) {
            auto &x = a.properties[i];
            Property p;
            p.index = x.index;
            p.kind = x.kind;
            if (x.size != sizeof(x) || x.value_count > 16 || (!x.values && x.value_count) || !Read(x.label, p.label))
                return false;
            for (uint32_t j = 0; j < x.value_count; ++j) {
                std::wstring v;
                if (!Read(x.values[j], v))
                    return false;
                p.values.push_back(std::move(v));
            }
            c.properties.push_back(std::move(p));
        }
        if (!Valid(c))
            return false;
        out = std::move(c);
        return true;
    } catch (...) {
        return false;
    }
}
bool FromAbi(const PulsePickerUiV2Event &a, Event &out) {
    try {
        if (a.size != sizeof(a) || a.reserved[0] || a.reserved[1] || a.kind < 1 || a.kind > 14 ||
            a.property_value_count > 16 || (!a.property_values && a.property_value_count))
            return false;
        Event e;
        e.kind = a.kind;
        e.show_id = a.show_id;
        e.generation = a.generation;
        e.window = reinterpret_cast<uintptr_t>(a.window);
        e.control_id = a.control_id;
        e.item_id = a.item_id;
        e.navigation_result = static_cast<uint32_t>(a.navigation_result);
        if (!Read(a.proposed_folder, e.proposed_folder) || !Read(a.control_text, e.control_text))
            return false;
        for (uint32_t i = 0; i < a.property_value_count; ++i) {
            std::wstring s;
            if (!Read(a.property_values[i], s))
                return false;
            e.property_values.push_back(std::move(s));
        }
        out = std::move(e);
        return true;
    } catch (...) {
        return false;
    }
}
bool FromAbi(const PulsePickerUiV2OverwriteEvent &a, Event &out) {
    try {
        if (a.size != sizeof(a) || a.reserved)
            return false;
        Event e;
        e.show_id = a.show_id;
        e.generation = a.generation;
        e.window = reinterpret_cast<uintptr_t>(a.window);
        if (!Read(a.target, e.target) || e.target.empty())
            return false;
        out = std::move(e);
        return true;
    } catch (...) {
        return false;
    }
}
bool FromAbi(const PulsePickerUiV2Snapshot &a, Snapshot &out) {
    try {
        if (a.size != sizeof(a) || a.reserved)
            return false;
        Snapshot s;
        s.flags = a.flags;
        s.show_id = a.show_id;
        s.generation = a.generation;
        s.window = reinterpret_cast<uintptr_t>(a.window);
        s.filter_index = a.filter_index;
        if (!Read(a.folder, s.folder) || !Read(a.filename, s.filename) || !Read(a.selection, s.selection) ||
            !Read(a.candidate, s.candidate) || !Packed(a.selection_paths, a.selection_count, s.selection_paths) ||
            !Packed(a.candidate_paths, a.candidate_count, s.candidate_paths) || !Valid(s))
            return false;
        out = std::move(s);
        return true;
    } catch (...) {
        return false;
    }
}
bool FromAbi(const PulsePickerUiV2Result &a, Result &out) {
    try {
        if (a.size != sizeof(a))
            return false;
        Result r;
        r.status = a.status;
        r.flags = a.flags;
        r.win32_error = a.win32_error;
        r.terminal_hr = static_cast<uint32_t>(a.terminal_hr);
        r.filter_index = a.filter_index;
        r.show_id = a.show_id;
        r.generation = a.generation;
        if (!Read(a.path, r.path) || !Packed(a.paths, a.path_count, r.paths) || !Valid(r))
            return false;
        out = std::move(r);
        return true;
    } catch (...) {
        return false;
    }
}
OwnedControl::OwnedControl(const Control &value) : value_(value) {
    auto &c = value_;
    for (auto &x : c.items)
        items_.push_back({sizeof(PulsePickerUiV2ControlItem), x.id, x.state, 0, Text(x.label)});
    abi_.size = sizeof(abi_);
    abi_.kind = c.kind;
    abi_.id = c.id;
    abi_.parent_id = c.parent_id;
    abi_.has_parent = c.has_parent;
    abi_.state = c.state;
    abi_.label = Text(c.label);
    abi_.items = items_.data();
    abi_.item_count = static_cast<uint32_t>(items_.size());
    abi_.has_selected = c.has_selected;
    abi_.selected_item_id = c.selected_item_id;
    abi_.checked = c.checked;
    abi_.text = Text(c.text);
}
OwnedConfig::OwnedConfig(const Config &value) : value_(value) {
    auto &c = value_;
    for (auto &f : c.filters)
        filters_.push_back({sizeof(PulsePickerUiV2Filter), 0, Text(f.label), Text(f.pattern)});
    for (auto &x : c.controls) {
        owned_controls_.push_back(std::make_unique<OwnedControl>(x));
        controls_.push_back(owned_controls_.back()->Get());
    }
    for (auto &p : c.places)
        places_.push_back({sizeof(PulsePickerUiV2Place), p.location, Text(p.label), Text(p.path)});
    values_.resize(c.properties.size());
    for (size_t i = 0; i < c.properties.size(); ++i) {
        auto &p = c.properties[i];
        for (auto &v : p.values)
            values_[i].push_back(Text(v));
        properties_.push_back({sizeof(PulsePickerUiV2Property), p.index, p.kind, Text(p.label), values_[i].data(),
                               static_cast<uint32_t>(values_[i].size())});
    }
    auto &a = abi_;
    a.size = sizeof(a);
    a.version = c.version;
    a.required_capabilities = c.required_capabilities;
    a.appearance = c.appearance;
    a.open_options = c.open_options;
    a.access_policy = c.access_policy;
    a.owner = reinterpret_cast<HWND>(static_cast<uintptr_t>(c.owner));
    a.initial_folder = Text(c.initial_folder);
    a.filename = Text(c.filename);
    a.title = Text(c.title);
    a.filters = filters_.data();
    a.filter_count = static_cast<uint32_t>(filters_.size());
    a.filter_index = c.filter_index;
    a.controls = controls_.data();
    a.control_count = static_cast<uint32_t>(controls_.size());
    a.default_extension = Text(c.default_extension);
    a.save_options = c.save_options;
    a.label_flags = c.label_flags;
    a.ok_button_label = Text(c.ok_button_label);
    a.file_name_label = Text(c.file_name_label);
    a.places = places_.data();
    a.place_count = static_cast<uint32_t>(places_.size());
    a.properties = properties_.data();
    a.property_count = static_cast<uint32_t>(properties_.size());
    a.native_options = c.native_options;
}
OwnedEvent::OwnedEvent(const Event &value) : value_(value) {
    auto &e = value_;
    for (auto &v : e.property_values)
        values_.push_back(Text(v));
    auto &a = abi_;
    a.size = sizeof(a);
    a.kind = e.kind;
    a.show_id = e.show_id;
    a.generation = e.generation;
    a.window = reinterpret_cast<HWND>(static_cast<uintptr_t>(e.window));
    a.proposed_folder = Text(e.proposed_folder);
    a.control_id = e.control_id;
    a.item_id = e.item_id;
    a.control_text = Text(e.control_text);
    a.property_values = values_.data();
    a.property_value_count = static_cast<uint32_t>(values_.size());
    a.navigation_result = static_cast<HRESULT>(e.navigation_result);
}
OwnedOverwriteEvent::OwnedOverwriteEvent(const Event &value) : value_(value) {
    abi_.size = sizeof(abi_);
    abi_.show_id = value_.show_id;
    abi_.generation = value_.generation;
    abi_.window = reinterpret_cast<HWND>(static_cast<uintptr_t>(value_.window));
    abi_.target = Text(value_.target);
}
namespace {
bool BuffersValid(const void* structure, size_t size, std::initializer_list<const PulsePickerUiV2Buffer*> buffers) {
    const auto start = reinterpret_cast<uintptr_t>(structure);
    if (size > UINTPTR_MAX - start) return false;
    std::vector<std::pair<uintptr_t, uintptr_t>> ranges{{start, start + size}};
    for (auto* buffer : buffers) {
        if (buffer->capacity && !buffer->data) return false;
        if (!buffer->capacity) continue;
        const auto first = reinterpret_cast<uintptr_t>(buffer->data);
        const uint64_t length = uint64_t(buffer->capacity) * sizeof(wchar_t);
        if (length > UINTPTR_MAX - first) return false;
        const auto last = first + static_cast<uintptr_t>(length);
        for (auto range : ranges) if (first < range.second && range.first < last) return false;
        ranges.emplace_back(first, last);
    }
    return true;
}
bool Fits(PulsePickerUiV2Buffer &b, const std::wstring &s) {
    b.units = static_cast<uint32_t>(s.size());
    return s.empty() ? (!b.capacity || b.data != nullptr) : (b.data && b.capacity > s.size());
}
void Copy(PulsePickerUiV2Buffer &b, const std::wstring &s) {
    if (b.data && b.capacity > s.size()) {
        std::copy(s.begin(), s.end(), b.data);
        b.data[s.size()] = 0;
    }
}
} // namespace
HRESULT CopyToAbi(const Snapshot &s, PulsePickerUiV2Snapshot &a) {
    try {
        if (a.size != sizeof(a) || a.reserved || !Valid(s) || s.window > UINTPTR_MAX ||
            !BuffersValid(&a, sizeof(a), {&a.folder, &a.filename, &a.selection, &a.candidate, &a.selection_paths, &a.candidate_paths}))
            return E_INVALIDARG;
        auto selections = Pack(s.selection_paths), candidates = Pack(s.candidate_paths);
        bool fit = true;
        fit &= Fits(a.folder, s.folder);
        fit &= Fits(a.filename, s.filename);
        fit &= Fits(a.selection, s.selection);
        fit &= Fits(a.candidate, s.candidate);
        bool packed = a.selection_paths.data || a.selection_paths.capacity || a.candidate_paths.data ||
                      a.candidate_paths.capacity || s.selection_paths.size() > 1 || s.candidate_paths.size() > 1;
        if (packed) {
            fit &= Fits(a.selection_paths, selections);
            fit &= Fits(a.candidate_paths, candidates);
        } else {
            a.selection_paths.units = static_cast<uint32_t>(selections.size());
            a.candidate_paths.units = static_cast<uint32_t>(candidates.size());
        }
        a.flags = s.flags;
        a.show_id = s.show_id;
        a.generation = s.generation;
        a.window = reinterpret_cast<HWND>(static_cast<uintptr_t>(s.window));
        a.filter_index = s.filter_index;
        a.selection_count = static_cast<uint32_t>(s.selection_paths.size());
        a.candidate_count = static_cast<uint32_t>(s.candidate_paths.size());
        if (!fit)
            return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        Copy(a.folder, s.folder);
        Copy(a.filename, s.filename);
        Copy(a.selection, s.selection);
        Copy(a.candidate, s.candidate);
        if (packed) {
            Copy(a.selection_paths, selections);
            Copy(a.candidate_paths, candidates);
        }
        return S_OK;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}
HRESULT CopyToAbi(const Result &r, PulsePickerUiV2Result &a) {
    try {
        if (a.size != sizeof(a) || !Valid(r) || !BuffersValid(&a, sizeof(a), {&a.path, &a.paths}))
            return E_INVALIDARG;
        auto paths = Pack(r.paths);
        bool fit = Fits(a.path, r.path);
        bool packed = a.paths.data || a.paths.capacity || r.paths.size() > 1;
        if (packed)
            fit &= Fits(a.paths, paths);
        else
            a.paths.units = static_cast<uint32_t>(paths.size());
        a.status = r.status;
        a.flags = r.flags;
        a.win32_error = r.win32_error;
        a.terminal_hr = static_cast<HRESULT>(r.terminal_hr);
        a.filter_index = r.filter_index;
        a.show_id = r.show_id;
        a.generation = r.generation;
        a.path_count = static_cast<uint32_t>(r.paths.size());
        if (!fit)
            return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        Copy(a.path, r.path);
        if (packed)
            Copy(a.paths, paths);
        return S_OK;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}
} // namespace pulse::ipc::picker::remote

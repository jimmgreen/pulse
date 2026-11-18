// tree_view.cpp — JSON/XML outline for Quick Look (see tree_view.h).
#include "tree_view.h"
#include "typography.h"
#include "../common/localization.h"
#include <algorithm>
#include <cmath>
#include <cwctype>

namespace pulse::ui {
namespace {

template <class T> using WrlPtr = Microsoft::WRL::ComPtr<T>;

enum Color : int { kText, kDim, kKey, kString, kNumber, kKeyword, kTag, kAttr, kColorCount };

constexpr size_t kAutoExpandNodes = 80;  // small documents open one more level

bool Zh() { return pulse::l10n::IsChinese(); }

std::wstring Unescape(std::wstring_view field) {
    std::wstring out;
    out.reserve(field.size());
    for (size_t i = 0; i < field.size(); ++i) {
        if (field[i] == L'\\' && i + 1 < field.size()) {
            const wchar_t n = field[++i];
            out += n == L't' ? L'\t' : n == L'n' ? L'\n' : n == L'r' ? L'\r' : n;
        } else {
            out += field[i];
        }
    }
    return out;
}

std::vector<std::wstring_view> Split(std::wstring_view line, wchar_t separator) {
    std::vector<std::wstring_view> fields;
    size_t start = 0;
    while (true) {
        const size_t at = line.find(separator, start);
        if (at == std::wstring_view::npos) { fields.push_back(line.substr(start)); break; }
        fields.push_back(line.substr(start, at - start));
        start = at + 1;
    }
    return fields;
}

uint32_t ToU32(std::wstring_view v) {
    uint32_t n = 0;
    for (const wchar_t c : v) { if (c < L'0' || c > L'9') break; n = n * 10 + static_cast<uint32_t>(c - L'0'); }
    return n;
}

// Control characters as visible escapes, so every node stays on one line.
std::wstring Visible(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (const wchar_t c : s) {
        if (c == L'\n') out += L"\\n";
        else if (c == L'\t') out += L"\\t";
        else if (c == L'\r') out += L"\\r";
        else if (c < 0x20) out += static_cast<wchar_t>(0x2400 + c);  // control pictures
        else out += c;
    }
    return out;
}

std::wstring JsonQuote(const std::wstring& s) {
    std::wstring out = L"\"";
    for (const wchar_t c : s) {
        switch (c) {
        case L'"': out += L"\\\""; break;
        case L'\\': out += L"\\\\"; break;
        case L'\n': out += L"\\n"; break;
        case L'\r': out += L"\\r"; break;
        case L'\t': out += L"\\t"; break;
        default:
            if (c < 0x20) {
                wchar_t buffer[8];
                swprintf(buffer, 8, L"\\u%04x", static_cast<unsigned>(c));
                out += buffer;
            } else {
                out += c;
            }
        }
    }
    out += L'"';
    return out;
}

std::wstring XmlAttributeDisplay(std::wstring_view value) {
    std::wstring out;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == L'&') {
            const size_t end = value.find(L';', i + 1);
            if (end != value.npos) {
                const auto entity = value.substr(i, end - i + 1);
                wchar_t decoded = 0;
                if (entity == L"&amp;") decoded = L'&';
                else if (entity == L"&quot;") decoded = L'"';
                else if (entity == L"&lt;") decoded = L'<';
                else if (entity == L"&#9;") decoded = L'\t';
                else if (entity == L"&#10;") decoded = L'\n';
                else if (entity == L"&#13;") decoded = L'\r';
                if (decoded) { out += decoded; i = end; continue; }
            }
        }
        out += value[i];
    }
    return out;
}

std::wstring XmlEscape(const std::wstring& s) {
    std::wstring out;
    for (const wchar_t c : s) {
        if (c == L'<') out += L"&lt;";
        else if (c == L'>') out += L"&gt;";
        else if (c == L'&') out += L"&amp;";
        else out += c;
    }
    return out;
}

bool Identifier(const std::wstring& key) {
    if (key.empty()) return false;
    for (size_t i = 0; i < key.size(); ++i) {
        const wchar_t c = key[i];
        const bool alpha = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c == L'_' || c == L'$';
        if (!(alpha || (i > 0 && c >= L'0' && c <= L'9'))) return false;
    }
    return true;
}

}  // namespace

// ---- Payload ----------------------------------------------------------------

bool TreeView::SetPayload(const std::shared_ptr<const std::wstring>& payload) {
    if (payload && payload_identity_.lock() == payload && !Payload().empty()) {
        return true;
    }
    if (!payload) return false;
    const bool accepted = SetPayloadImpl(*payload, payload);
    if (accepted) {
        StorePayload(*payload, payload);
        payload_identity_ = payload;
    }
    return accepted;
}

bool TreeView::SetPayload(const std::wstring& payload) {
    return SetPayloadImpl(payload, {});
}

bool TreeView::SetPayloadImpl(const std::wstring& payload,
                           std::shared_ptr<const std::wstring> owner) {
    payload_identity_.reset();
    if (payload == Payload() && !Payload().empty()) return true;
    if (payload.compare(0, 11, L"PULSETREE\t1") != 0) return false;
    Clear();
    std::wstring_view all(payload);
    size_t pos = 0;
    std::vector<int> stack;          // open ancestors by depth
    std::vector<uint32_t> siblings;  // children seen so far, by depth
    bool header = false;
    while (pos < all.size()) {
        size_t end = all.find(L'\n', pos);
        if (end == std::wstring_view::npos) end = all.size();
        const std::wstring_view line = all.substr(pos, end - pos);
        pos = end + 1;
        if (line.size() < 2 || line[1] != L'\t') continue;
        const wchar_t tag = line[0];
        const auto f = Split(line, L'\t');
        if (tag == L'H' && f.size() >= 7) {
            header = true;
            xml_ = f[1] == L"xml";
            truncated_ = f[3] == L"1";
            error_ = Unescape(f[4]);
            error_line_ = ToU32(f[5]);
            error_column_ = ToU32(f[6]);
        } else if (tag == L'N' && f.size() >= 7) {
            Node n;
            n.depth = ToU32(f[1]);
            n.type = f[2].empty() ? L's' : f[2][0];
            n.key = Unescape(f[3]);
            n.value = Unescape(f[4]);
            n.children = ToU32(f[5]);
            n.text = Unescape(f[6]);
            n.attributes_known = f.size() >= 8 && f[7] == L"attributes-v1";
            // Depth may only grow by one; anything else is a broken payload.
            if (nodes_.empty() ? n.depth != 0 : n.depth > nodes_.back().depth + 1) continue;
            if (!nodes_.empty() && n.depth == 0) continue;  // one root
            stack.resize(n.depth);
            siblings.resize(n.depth + 2, 0);
            if (n.depth > 0) n.parent = stack[n.depth - 1];
            n.index = siblings[n.depth]++;
            siblings[n.depth + 1] = 0;  // a new parent starts counting afresh
            stack.push_back(static_cast<int>(nodes_.size()));
            nodes_.push_back(std::move(n));
        } else if (tag == L'A' && f.size() == 3 && !nodes_.empty() && nodes_.back().type == L'e' && nodes_.back().attributes_known) {
            nodes_.back().attributes.emplace_back(Unescape(f[1]), Unescape(f[2]));
        } else if (tag == L'X' && f.size() >= 2) {
            source_ = Unescape(line.substr(2));
        }
    }
    if (!header) { Clear(); return false; }
    StorePayload(payload, std::move(owner));
    // Subtree ends (pre-order: the next node at the same or a lower depth).
    std::vector<uint32_t> open;
    for (uint32_t i = 0; i < nodes_.size(); ++i) {
        while (!open.empty() && nodes_[open.back()].depth >= nodes_[i].depth) {
            nodes_[open.back()].end = i;
            open.pop_back();
        }
        open.push_back(i);
    }
    for (uint32_t i : open) nodes_[i].end = static_cast<uint32_t>(nodes_.size());
    // Searchable text.
    plain_.clear();
    for (size_t i = 0; i < nodes_.size(); ++i) {
        nodes_[i].offset = static_cast<uint32_t>(plain_.size());
        plain_ += RowText(i, nullptr);
        plain_ += L'\n';
    }
    // Root open; the first level too when the document is small.
    expanded_.assign(nodes_.size(), 0);
    if (!nodes_.empty()) {
        expanded_[0] = 1;
        if (nodes_.size() <= kAutoExpandNodes)
            for (size_t i = 1; i < nodes_.size(); ++i)
                if (nodes_[i].depth == 1) expanded_[i] = 1;
    }
    BuildRows();
    return true;
}

void TreeView::Clear() {
    payload_identity_.reset();
    ++text_revision_;
    decltype(payload_){}.swap(payload_);
    shared_payload_.reset();
    decltype(source_){}.swap(source_);
    decltype(plain_){}.swap(plain_);
    error_.clear();
    error_line_ = error_column_ = 0;
    xml_ = truncated_ = false;
    decltype(nodes_){}.swap(nodes_);
    decltype(expanded_){}.swap(expanded_);
    decltype(rows_){}.swap(rows_);
    current_ = -1;
    hover_row_ = -1;
    sy_ = 0.0f;
    dragging_ = false;
}

std::wstring TreeView::ErrorText() const {
    struct Reason { const wchar_t* id; const wchar_t* zh; const wchar_t* en; };
    static constexpr Reason kReasons[] = {
        {L"comma", L"\u7F3A\u5C11\u9017\u53F7\u6216\u53F3\u62EC\u53F7", L"missing comma or closing bracket"},
        {L"colon", L"\u7F3A\u5C11\u5192\u53F7", L"missing colon"},
        {L"key", L"\u7F3A\u5C11\u5E26\u5F15\u53F7\u7684\u952E\u540D", L"expected a quoted key"},
        {L"string", L"\u5B57\u7B26\u4E32\u6CA1\u6709\u7ED3\u675F", L"unterminated string"},
        {L"escape", L"\u65E0\u6548\u7684\u8F6C\u4E49\u5E8F\u5217", L"invalid escape"},
        {L"value", L"\u65E0\u6548\u7684\u503C", L"invalid value"},
        {L"eof", L"\u6587\u4EF6\u610F\u5916\u7ED3\u675F", L"unexpected end of file"},
        {L"trailing", L"\u7ED3\u5C3E\u6709\u591A\u4F59\u5185\u5BB9", L"unexpected content after the end"},
        {L"depth", L"\u5D4C\u5957\u5C42\u7EA7\u8FC7\u6DF1", L"nested too deeply"},
        {L"too-large", L"\u6587\u4EF6\u8FC7\u5927\uFF0C\u53EA\u663E\u793A\u6E90\u7801", L"too large for the tree view"},
        {L"comment", L"\u6CE8\u91CA\u6CA1\u6709\u7ED3\u675F", L"unterminated comment"},
        {L"cdata", L"CDATA \u6CA1\u6709\u7ED3\u675F", L"unterminated CDATA"},
        {L"pi", L"\u5904\u7406\u6307\u4EE4\u6CA1\u6709\u7ED3\u675F", L"unterminated processing instruction"},
        {L"tag", L"\u6807\u7B7E\u4E0D\u5B8C\u6574", L"malformed tag"},
        {L"mismatch", L"\u7ED3\u675F\u6807\u7B7E\u4E0D\u5339\u914D", L"mismatched closing tag"},
        {L"attribute", L"\u5C5E\u6027\u683C\u5F0F\u9519\u8BEF", L"malformed attribute"},
        {L"root", L"\u5B58\u5728\u591A\u4E2A\u6839\u5143\u7D20", L"more than one root element"},
        {L"empty", L"\u6CA1\u6709\u6839\u5143\u7D20", L"no root element"},
    };
    const bool zh = Zh();
    std::wstring reason = error_;
    for (const Reason& r : kReasons)
        if (error_ == r.id) { reason = zh ? pulse::l10n::Cn(r.zh) : r.en; break; }
    if (error_ == L"too-large" || error_line_ == 0) return reason;
    std::wstring where = zh ? pulse::l10n::Cn(L"\u7B2C ") + std::to_wstring(error_line_) + pulse::l10n::Cn(L" \u884C\uFF0C\u7B2C ") +
                                  std::to_wstring(error_column_) + pulse::l10n::Cn(L" \u5217\uFF1A")
                            : L"Line " + std::to_wstring(error_line_) + L", column " +
                                  std::to_wstring(error_column_) + L": ";
    return where + reason;
}

std::vector<std::wstring> TreeView::StatusParts() const {
    std::vector<std::wstring> parts;
    parts.push_back(xml_ ? L"XML" : L"JSON");
    if (current_ >= 0) {
        parts.push_back(PathOf(static_cast<size_t>(current_)));
        if (std::wstring type = CurrentType(); !type.empty()) parts.push_back(std::move(type));
    } else {
        std::wstring count = std::to_wstring(nodes_.size()) + (pulse::l10n::Get(pulse::l10n::StringId::Nodes).c_str());
        if (truncated_) count += pulse::l10n::Get(pulse::l10n::StringId::Truncated).c_str();
        parts.push_back(std::move(count));
    }
    return parts;
}

std::wstring TreeView::CurrentType() const {
    if (current_ < 0 || current_ >= static_cast<int>(nodes_.size()) || xml_) return {};
    const Node& n = nodes_[static_cast<size_t>(current_)];
    switch (n.type) {
    case L'o': return L"object";
    case L'a': return L"array";
    case L's': return L"string";
    case L'n': return L"number";
    default: return n.value == L"null" ? L"null" : L"boolean";
    }
}

bool TreeView::AnyCollapsed() const noexcept {
    for (size_t i = 0; i < nodes_.size(); ++i)
        if (Expandable(i) && !expanded_[i]) return true;
    return false;
}

void TreeView::ExpandAll(bool expand) {
    if (nodes_.empty()) return;
    for (size_t i = 0; i < nodes_.size(); ++i)
        if (Expandable(i)) expanded_[i] = expand || nodes_[i].depth == 0 ? 1 : 0;
    // A node folded out of sight hands the focus to its nearest visible ancestor.
    if (!expand && current_ >= 0) {
        int top = current_;
        for (int p = nodes_[static_cast<size_t>(current_)].parent; p >= 0; p = nodes_[static_cast<size_t>(p)].parent)
            if (!expanded_[static_cast<size_t>(p)]) top = p;
        current_ = top;
    }
    BuildRows();
}

// ---- Rows -------------------------------------------------------------------

bool TreeView::IsContainer(size_t i) const noexcept {
    const Node& n = nodes_[i];
    return n.type == L'o' || n.type == L'a' || (n.type == L'e' && n.children > 0 && n.text.empty());
}

std::wstring TreeView::RowText(size_t i, std::vector<Piece>* pieces) const {
    const Node& n = nodes_[i];
    std::wstring s;
    const auto put = [&](const std::wstring& text, int color) {
        if (text.empty()) return;
        if (pieces) pieces->push_back({static_cast<uint32_t>(s.size()), static_cast<uint32_t>(text.size()), color});
        s += text;
    };
    if (!xml_) {
        if (n.parent >= 0) {
            if (nodes_[n.parent].type == L'a') {
                put(std::to_wstring(n.index), kDim);
                put(L": ", kDim);
            } else {
                put(L"\"" + Visible(n.key) + L"\"", kKey);
                put(L": ", kText);
            }
        }
        switch (n.type) {
        case L'o': put(L"{ " + std::to_wstring(n.children) + L" }", kDim); break;
        case L'a': put(L"[ " + std::to_wstring(n.children) + L" ]", kDim); break;
        case L's': put(L"\"" + Visible(n.value) + L"\"", kString); break;
        case L'n': put(n.value, kNumber); break;
        default: put(n.value, kKeyword); break;  // true / false / null
        }
        return s;
    }
    switch (n.type) {
    case L'e': {
        put(L"<" + n.key, kTag);
        // Attributes: name="value" ...
        const std::wstring& a = n.value;
        size_t p = 0;
        while (p < a.size()) {
            while (p < a.size() && a[p] == L' ') ++p;
            if (p >= a.size()) break;
            const size_t eq = a.find(L'=', p);
            if (eq == std::wstring::npos) { put(L" " + a.substr(p), kAttr); break; }
            put(L" " + a.substr(p, eq - p), kAttr);
            put(L"=", kText);
            size_t q = eq + 1;
            if (q < a.size() && a[q] == L'"') {
                const size_t close = a.find(L'"', q + 1);
                const size_t stop = close == std::wstring::npos ? a.size() : close + 1;
                put(Visible(XmlAttributeDisplay(std::wstring_view(a).substr(q, stop - q))), kString);
                p = stop;
            } else {
                const size_t sp = a.find(L' ', q);
                const size_t stop = sp == std::wstring::npos ? a.size() : sp;
                put(Visible(XmlAttributeDisplay(std::wstring_view(a).substr(q, stop - q))), kString);
                p = stop;
            }
        }
        if (!n.text.empty()) {
            put(L">", kTag);
            put(Visible(n.text), kText);
            put(L"</" + n.key + L">", kTag);
        } else if (n.children == 0) {
            put(L" />", kTag);
        } else {
            put(L">", kTag);
        }
        break;
    }
    case L't': put(Visible(n.value), kText); break;
    case L'c': put(L"<!-- " + Visible(n.value) + L" -->", kDim); break;
    case L'd':
        put(L"<![CDATA[", kDim);
        put(Visible(n.value), kString);
        put(L"]]>", kDim);
        break;
    case L'p': put(L"<?" + n.key + (n.value.empty() ? L"" : L" " + Visible(n.value)) + L"?>", kDim); break;
    default: put(Visible(n.value), kText); break;
    }
    return s;
}

// Drawn after the row text but not searchable (depends on folding).
std::wstring TreeView::Suffix(size_t i, std::vector<Piece>* pieces, size_t base) const {
    const Node& n = nodes_[i];
    if (expanded_[i] || !IsContainer(i)) return {};
    std::wstring s;
    const auto put = [&](const std::wstring& text, int color) {
        if (pieces) pieces->push_back({static_cast<uint32_t>(base + s.size()), static_cast<uint32_t>(text.size()), color});
        s += text;
    };
    if (!xml_) {
        // A lone scalar child is previewed inline: { 1 } … "node": ">=18"
        if (n.children == 1 && Expandable(i) && !IsContainer(i + 1)) {
            put(L" \x2026 " + RowText(i + 1, nullptr), kDim);
        }
        return s;
    }
    put(L"\x2026", kDim);
    put(L"</" + n.key + L">", kTag);
    put(L"  " + std::to_wstring(n.children), kDim);
    return s;
}

void TreeView::BuildRows() {
    rows_.clear();
    for (uint32_t i = 0; i < nodes_.size();) {
        rows_.push_back(i);
        i = expanded_[i] || !Expandable(i) ? i + 1 : nodes_[i].end;
    }
    hover_row_ = -1;
    Clamp();
}

void TreeView::SetExpanded(size_t i, bool expanded) {
    if (!Expandable(i) || (expanded_[i] != 0) == expanded) return;
    expanded_[i] = expanded ? 1 : 0;
    // A collapsed ancestor of the current node takes the focus.
    if (!expanded && current_ > static_cast<int>(i) && current_ < static_cast<int>(nodes_[i].end))
        current_ = static_cast<int>(i);
    BuildRows();
}

int TreeView::RowOf(size_t node) const {
    const auto it = std::lower_bound(rows_.begin(), rows_.end(), static_cast<uint32_t>(node));
    if (it == rows_.end() || *it != node) return -1;
    return static_cast<int>(it - rows_.begin());
}

float TreeView::ContentHeight() const noexcept {
    return rows_.size() * RowHeight() + 12.0f * scale_ + 52.0f * scale_;  // room for the pill
}

void TreeView::Clamp() {
    const float view = view_.bottom - view_.top;
    const float max_sy = (std::max)(0.0f, ContentHeight() - view);
    sy_ = (std::clamp)(sy_, 0.0f, max_sy);
}

void TreeView::EnsureVisible(int row) {
    if (row < 0) return;
    const float top = 12.0f * scale_ + row * RowHeight();
    const float view = view_.bottom - view_.top;
    if (view <= 0) return;
    if (top < sy_ + RowHeight()) sy_ = top - RowHeight();
    else if (top + RowHeight() > sy_ + view - 52.0f * scale_) sy_ = top + RowHeight() - view + 52.0f * scale_;
    Clamp();
}

// ---- Paths and copying ------------------------------------------------------

std::wstring TreeView::PathOf(size_t i) const {
    std::vector<size_t> chain;
    for (int k = static_cast<int>(i); k >= 0; k = nodes_[k].parent) chain.push_back(static_cast<size_t>(k));
    std::reverse(chain.begin(), chain.end());
    std::wstring path;
    if (!xml_) {
        path = L"$";
        for (size_t c = 1; c < chain.size(); ++c) {
            const Node& n = nodes_[chain[c]];
            if (nodes_[n.parent].type == L'a') path += L"[" + std::to_wstring(n.index) + L"]";
            else if (Identifier(n.key)) path += L"." + n.key;
            else path += L"[" + JsonQuote(n.key) + L"]";
        }
        return path;
    }
    for (size_t c = 0; c < chain.size(); ++c) {
        const Node& n = nodes_[chain[c]];
        if (n.type == L't' || n.type == L'd') { path += L"/text()"; continue; }
        if (n.type == L'c') { path += L"/comment()"; continue; }
        if (n.type == L'p') { path += L"/processing-instruction()"; continue; }
        path += L"/" + n.key;
        if (n.parent < 0) continue;
        // Position among same-name siblings, only when there are several.
        size_t same = 0, position = 0;
        for (size_t j = static_cast<size_t>(n.parent) + 1; j < nodes_[n.parent].end; j = nodes_[j].end) {
            if (nodes_[j].type == L'e' && nodes_[j].key == n.key) {
                ++same;
                if (j == chain[c]) position = same;
            }
        }
        if (same > 1) path += L"[" + std::to_wstring(position) + L"]";
    }
    return path;
}

void TreeView::AppendJson(size_t i, int indent, std::wstring& out) const {
    const Node& n = nodes_[i];
    if (n.type == L's') { out += JsonQuote(n.value); return; }
    if (n.type != L'o' && n.type != L'a') { out += n.value; return; }
    const bool object = n.type == L'o';
    if (n.children == 0) { out += object ? L"{}" : L"[]"; return; }
    out += object ? L"{\n" : L"[\n";
    bool first = true;
    for (size_t j = i + 1; j < n.end; j = nodes_[j].end) {
        if (!first) out += L",\n";
        first = false;
        out.append(static_cast<size_t>(indent + 2), L' ');
        if (object) { out += JsonQuote(nodes_[j].key); out += L": "; }
        AppendJson(j, indent + 2, out);
    }
    if (n.end == i + 1) { out.append(static_cast<size_t>(indent + 2), L' '); out += L"\x2026"; }
    out += L'\n';
    out.append(static_cast<size_t>(indent), L' ');
    out += object ? L'}' : L']';
}

void TreeView::AppendXml(size_t i, int indent, std::wstring& out, bool subtree_root) const {
    const Node& n = nodes_[i];
    out.append(static_cast<size_t>(indent), L' ');
    switch (n.type) {
    case L'e': {
        out += L"<" + n.key;
        const auto append_attribute = [&](const auto& attribute) {
            out += L" " + attribute.first + L"=\"";
            for (wchar_t c : attribute.second) {
                switch (c) {
                case L'&': out += L"&amp;"; break;
                case L'<': out += L"&lt;"; break;
                case L'"': out += L"&quot;"; break;
                case L'\r': out += L"&#13;"; break;
                case L'\n': out += L"&#10;"; break;
                case L'\t': out += L"&#9;"; break;
                default: out += c; break;
                }
            }
            out += L'"';
        };
        for (const auto& attribute : n.attributes) append_attribute(attribute);
        if (subtree_root) {
            std::vector<std::wstring> declared;
            const auto is_namespace = [](const std::wstring& key) { return key == L"xmlns" || key.starts_with(L"xmlns:"); };
            for (const auto& attribute : n.attributes) if (is_namespace(attribute.first)) declared.push_back(attribute.first);
            for (int ancestor = n.parent; ancestor >= 0; ancestor = nodes_[ancestor].parent) {
                for (const auto& attribute : nodes_[ancestor].attributes) {
                    if (!is_namespace(attribute.first) || std::find(declared.begin(), declared.end(), attribute.first) != declared.end()) continue;
                    append_attribute(attribute); declared.push_back(attribute.first);
                }
            }
        }
        if (!n.text.empty()) { out += L">" + XmlEscape(n.text) + L"</" + n.key + L">"; break; }
        if (n.end == i + 1) { out += L" />"; break; }
        out += L">\n";
        for (size_t j = i + 1; j < n.end; j = nodes_[j].end) { AppendXml(j, indent + 2, out); out += L'\n'; }
        out.append(static_cast<size_t>(indent), L' ');
        out += L"</" + n.key + L">";
        break;
    }
    case L'c': out += L"<!-- " + n.value + L" -->"; break;
    case L'd': out += L"<![CDATA[" + n.value + L"]]>"; break;
    case L'p': out += L"<?" + n.key + (n.value.empty() ? L"" : L" " + n.value) + L"?>"; break;
    default: out += XmlEscape(n.value); break;
    }
}

std::wstring TreeView::CurrentValue() const {
    if (current_ < 0) return source_;
    const size_t i = static_cast<size_t>(current_);
    const Node& n = nodes_[i];
    std::wstring out;
    if (!xml_) {
        if (n.type == L'o' || n.type == L'a') AppendJson(i, 0, out);
        else out = n.value;
        return out;
    }
    if (n.type == L'e' && !n.text.empty()) return n.text;
    if (n.type != L'e') return n.value;
    // Old payloads flatten attribute delimiters and decoded values together;
    // they cannot be reconstructed without inventing attribute boundaries.
    for (size_t j = i; j < n.end; ++j)
        if (nodes_[j].type == L'e' && !nodes_[j].attributes_known && !nodes_[j].value.empty()) return {};
    for (int ancestor = n.parent; ancestor >= 0; ancestor = nodes_[ancestor].parent)
        if (!nodes_[ancestor].attributes_known && !nodes_[ancestor].value.empty()) return {};
    AppendXml(i, 0, out, true);
    return out;
}

std::wstring TreeView::CurrentPath() const {
    return current_ >= 0 ? PathOf(static_cast<size_t>(current_)) : std::wstring();
}

bool TreeView::ClearCurrent() {
    if (current_ < 0) return false;
    current_ = -1;
    return true;
}

// ---- Input ------------------------------------------------------------------

bool TreeView::Scroll(float wheel_steps) {
    const float before = sy_;
    sy_ -= wheel_steps * RowHeight() * 3.0f;
    Clamp();
    return sy_ != before;
}

bool TreeView::Key(UINT vk) {
    const float view = view_.bottom - view_.top;
    const int page = (std::max)(1, static_cast<int>((view - 64.0f * scale_) / RowHeight()));
    const int row = current_ >= 0 ? RowOf(static_cast<size_t>(current_)) : -1;
    const auto move_to = [&](int r) {
        if (rows_.empty()) return;
        r = (std::clamp)(r, 0, static_cast<int>(rows_.size()) - 1);
        current_ = static_cast<int>(rows_[r]);
        EnsureVisible(r);
    };
    switch (vk) {
    case VK_UP:
    case VK_DOWN:
        if (row < 0) return false;
        move_to(row + (vk == VK_UP ? -1 : 1));
        return true;
    case VK_LEFT: {
        if (current_ < 0) return false;
        const size_t i = static_cast<size_t>(current_);
        if (expanded_[i] && Expandable(i)) SetExpanded(i, false);
        else if (nodes_[i].parent >= 0) current_ = nodes_[i].parent;
        EnsureVisible(RowOf(static_cast<size_t>(current_)));
        return true;
    }
    case VK_RIGHT: {
        if (current_ < 0) return false;
        const size_t i = static_cast<size_t>(current_);
        if (Expandable(i) && !expanded_[i]) SetExpanded(i, true);
        else if (Expandable(i)) current_ = static_cast<int>(i + 1);
        EnsureVisible(RowOf(static_cast<size_t>(current_)));
        return true;
    }
    case VK_RETURN:
        if (current_ < 0 || !Expandable(static_cast<size_t>(current_))) return false;
        SetExpanded(static_cast<size_t>(current_), !expanded_[current_]);
        return true;
    case VK_PRIOR:
    case VK_NEXT:
        if (row >= 0) move_to(row + (vk == VK_PRIOR ? -page : page));
        else { sy_ += (vk == VK_PRIOR ? -1.0f : 1.0f) * page * RowHeight(); Clamp(); }
        return true;
    case VK_HOME:
    case VK_END:
        if (row >= 0) move_to(vk == VK_HOME ? 0 : static_cast<int>(rows_.size()) - 1);
        else { sy_ = vk == VK_HOME ? 0.0f : 1.0e9f; Clamp(); }
        return true;
    default:
        return false;
    }
}

void TreeView::Reveal(uint32_t offset) {
    if (nodes_.empty()) return;
    size_t lo = 0, hi = nodes_.size();
    while (hi - lo > 1) {
        const size_t mid = (lo + hi) / 2;
        if (nodes_[mid].offset <= offset) lo = mid; else hi = mid;
    }
    bool changed = false;
    for (int p = nodes_[lo].parent; p >= 0; p = nodes_[p].parent)
        if (!expanded_[p]) { expanded_[p] = 1; changed = true; }
    if (changed) BuildRows();
    current_ = static_cast<int>(lo);
    const int row = RowOf(lo);
    // Centre the match when it is off screen.
    const float view = view_.bottom - view_.top;
    const float top = 12.0f * scale_ + row * RowHeight();
    if (row >= 0 && view > 0 && (top < sy_ || top + RowHeight() > sy_ + view - 52.0f * scale_)) {
        sy_ = top - view * 0.4f;
        Clamp();
    }
}

bool TreeView::MouseDown(float x, float y) {
    if (x < view_.left || x >= view_.right || y < view_.top || y >= view_.bottom) return false;
    if (thumb_.right > thumb_.left && x >= view_.right - 12.0f * scale_) {
        if (y >= thumb_.top && y < thumb_.bottom) {
            dragging_ = true;
            drag_origin_ = y;
            drag_scroll_ = sy_;
        } else {
            const float view = view_.bottom - view_.top;
            sy_ += (y < thumb_.top ? -1.0f : 1.0f) * view * 0.9f;
            Clamp();
        }
        return true;
    }
    const float local = y - view_.top + sy_ - 12.0f * scale_;
    const int row = local < 0 ? -1 : static_cast<int>(local / RowHeight());
    if (row < 0 || row >= static_cast<int>(rows_.size())) return ClearCurrent();
    const size_t i = rows_[row];
    current_ = static_cast<int>(i);
    // Containers fold on click (anywhere on the row, like the ▸).
    if (Expandable(i)) SetExpanded(i, !expanded_[i]);
    return true;
}

bool TreeView::MouseMove(float, float y) {
    if (!dragging_) return false;
    const float view = view_.bottom - view_.top;
    const float track = view - 8.0f * scale_;
    const float thumb = thumb_.bottom - thumb_.top;
    const float room = (std::max)(1.0f, track - thumb);
    const float max_sy = (std::max)(0.0f, ContentHeight() - view);
    sy_ = drag_scroll_ + (y - drag_origin_) / room * max_sy;
    Clamp();
    return true;
}

bool TreeView::Hover(float x, float y) {
    int row = -1;
    if (x >= view_.left && x < view_.right - 12.0f * scale_ && y >= view_.top && y < view_.bottom) {
        const float local = y - view_.top + sy_ - 12.0f * scale_;
        if (local >= 0) row = static_cast<int>(local / RowHeight());
        if (row >= static_cast<int>(rows_.size())) row = -1;
    }
    if (row == hover_row_) return false;
    hover_row_ = row;
    return true;
}

bool TreeView::Leave() {
    if (hover_row_ < 0) return false;
    hover_row_ = -1;
    return true;
}

// ---- Drawing ----------------------------------------------------------------

bool TreeView::EnsureFormats(IDWriteFactory2* factory, float scale) {
    if (!factory) return false;
    if (factory == factory_ && scale == format_scale_ && mono_) return true;
    factory_ = factory;
    format_scale_ = scale;
    mono_.Reset();
    typography::CreateTextFormat(factory, {typography::FontRole::Monospace, 12.5f * scale}, &mono_);
    if (!mono_) return false;
    WrlPtr<IDWriteInlineObject> ellipsis;
    factory->CreateEllipsisTrimmingSign(mono_.Get(), &ellipsis);
    const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    mono_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    mono_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    mono_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    mono_->SetTrimming(&trimming, ellipsis.Get());
    return true;
}

void TreeView::Draw(ID2D1DeviceContext* dc, IDWriteFactory2* factory, const D2D1_RECT_F& rect,
                    const Theme& theme, bool dark, float scale, SearchHighlights matches) {
    if (!dc || nodes_.empty() || !EnsureFormats(factory, scale)) return;
    view_ = rect;
    scale_ = scale;
    if (brush_owner_ != dc || !brush_) {
        brush_.Reset();
        brush_owner_ = dc;
        dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &brush_);
    }
    if (!brush_) return;
    Clamp();
    const float s = scale;
    const float rh = RowHeight();
    const float left = rect.left + 18.0f * s;
    const float step = 20.0f * s;   // indent per level (guide 6 + 1 + 13 in the mockup)
    const float chevron = 14.0f * s;

    // Palette (the mockup's VS Code-like colours).
    D2D1_COLOR_F colors[kColorCount];
    colors[kText] = theme.text;
    colors[kDim] = WithAlpha(theme.text, 0.45f);
    colors[kKey] = dark ? HexColor(0x9CDCFE) : HexColor(0x0451A5);
    colors[kString] = dark ? HexColor(0xCE9178) : HexColor(0xA31515);
    colors[kNumber] = dark ? HexColor(0xB5CEA8) : HexColor(0x098658);
    colors[kKeyword] = dark ? HexColor(0x569CD6) : HexColor(0x0000FF);
    colors[kTag] = dark ? HexColor(0x569CD6) : HexColor(0x800000);
    colors[kAttr] = dark ? HexColor(0x9CDCFE) : HexColor(0xE50000);
    WrlPtr<ID2D1SolidColorBrush> brushes[kColorCount];
    for (int c = 0; c < kColorCount; ++c) dc->CreateSolidColorBrush(colors[c], &brushes[c]);
    WrlPtr<ID2D1SolidColorBrush> on_hit;
    dc->CreateSolidColorBrush(HexColor(0x111111), &on_hit);

    const D2D1_COLOR_F guide = WithAlpha(theme.text, 0.10f);
    const D2D1_COLOR_F current_fill = dark ? D2D1::ColorF(96.0f / 255, 205.0f / 255, 1.0f, 0.13f)
                                           : D2D1::ColorF(0.0f, 95.0f / 255, 184.0f / 255, 0.09f);

    dc->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_ALIASED);
    const float top0 = rect.top + 12.0f * s - sy_;
    const int first = (std::max)(0, static_cast<int>((sy_ - 12.0f * s) / rh));
    const int last = (std::min)(static_cast<int>(rows_.size()),
                                static_cast<int>((sy_ + (rect.bottom - rect.top)) / rh) + 2);
    for (int r = first; r < last; ++r) {
        const size_t i = rows_[r];
        const Node& n = nodes_[i];
        const float y = top0 + r * rh;
        // Row backgrounds.
        if (static_cast<int>(i) == current_ || r == hover_row_) {
            brush_->SetColor(static_cast<int>(i) == current_ ? current_fill : WithAlpha(theme.text, 0.04f));
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(rect.left + 8.0f * s, y, rect.right - 14.0f * s, y + rh),
                                                       3.0f * s, 3.0f * s), brush_.Get());
        }
        // Indent guides of the open ancestors.
        brush_->SetColor(guide);
        for (uint32_t d = 0; d < n.depth; ++d) {
            const float gx = std::floor(left + d * step + 6.0f * s) + 0.5f;
            dc->FillRectangle(D2D1::RectF(gx - 0.5f, y, gx + 0.5f, y + rh), brush_.Get());
        }
        const float x0 = left + n.depth * step;
        // Chevron (none for a collapsed container whose lone value shows inline).
        const bool inline_preview = !xml_ && !expanded_[i] && IsContainer(i) && n.children == 1 &&
                                    Expandable(i) && !IsContainer(i + 1);
        if (Expandable(i) && !inline_preview) {
            brush_->SetColor(WithAlpha(theme.text, 0.55f));
            const float cx = x0 + 5.0f * s, cy = y + rh * 0.5f, a = 3.2f * s;
            const float w = 1.3f * s;
            if (expanded_[i]) {
                dc->DrawLine({cx - a, cy - a * 0.5f}, {cx, cy + a * 0.5f}, brush_.Get(), w);
                dc->DrawLine({cx, cy + a * 0.5f}, {cx + a, cy - a * 0.5f}, brush_.Get(), w);
            } else {
                dc->DrawLine({cx - a * 0.5f, cy - a}, {cx + a * 0.5f, cy}, brush_.Get(), w);
                dc->DrawLine({cx + a * 0.5f, cy}, {cx - a * 0.5f, cy + a}, brush_.Get(), w);
            }
        }
        // Text.
        std::vector<Piece> pieces;
        std::wstring text = RowText(i, &pieces);
        const size_t base = text.size();
        text += Suffix(i, &pieces, base);
        const float tx = x0 + chevron;
        const float avail = (std::max)(8.0f * s, rect.right - 18.0f * s - tx);
        WrlPtr<IDWriteTextLayout> layout;
        factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), mono_.Get(), avail, rh, &layout);
        if (!layout) continue;
        for (const Piece& p : pieces)
            if (brushes[p.color]) layout->SetDrawingEffect(brushes[p.color].Get(), {p.start, p.length});
        // Find highlights inside this node's searchable text.
        const uint32_t start = n.offset, end = n.offset + static_cast<uint32_t>(base);
        for (const Highlight match : matches.Intersecting(start, end)) {
            const uint32_t a = (std::max)(match.start, start) - start;
            const uint32_t b = (std::min)(match.start + match.length, end) - start;
            if (b <= a) continue;
            UINT32 count = 0;
            layout->HitTestTextRange(a, b - a, 0, 0, nullptr, 0, &count);
            std::vector<DWRITE_HIT_TEST_METRICS> hits(count);
            if (count && SUCCEEDED(layout->HitTestTextRange(a, b - a, 0, 0, hits.data(), count, &count))) {
                brush_->SetColor(match.current ? HexColor(0xF59E0B) : HexColor(0xFACC15));
                for (const auto& h : hits)
                    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tx + h.left, y + 2.0f * s, tx + h.left + h.width,
                                                                           y + rh - 2.0f * s), 2.0f * s, 2.0f * s), brush_.Get());
            }
            if (on_hit) layout->SetDrawingEffect(on_hit.Get(), {a, b - a});
        }
        brush_->SetColor(theme.text);
        dc->DrawTextLayout(D2D1::Point2F(tx, y), layout.Get(), brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    // Scroll position.
    const float view = rect.bottom - rect.top;
    const float content = ContentHeight();
    thumb_ = {};
    if (content > view + 1.0f) {
        const float track = view - 8.0f * s;
        const float thumb = (std::max)(24.0f * s, track * view / content);
        const float t = sy_ / (content - view);
        const float ty = rect.top + 4.0f * s + (track - thumb) * t;
        thumb_ = D2D1::RectF(rect.right - 7.0f * s, ty, rect.right - 2.0f * s, ty + thumb);
        brush_->SetColor(WithAlpha(theme.text, dragging_ ? 0.40f : 0.25f));
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(rect.right - 6.0f * s, ty, rect.right - 3.0f * s, ty + thumb),
                                                   1.5f * s, 1.5f * s), brush_.Get());
    }
    dc->PopAxisAlignedClip();
}

}  // namespace pulse::ui

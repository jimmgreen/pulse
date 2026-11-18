// table_view.cpp — see table_view.h.
#include "table_view.h"
#include "typography.h"
#include "../common/localization.h"
#include "../ipc/preview_protocol.h"
#include <algorithm>
#include <cmath>

namespace pulse::ui {
namespace {

template <class T> using WrlPtr = Microsoft::WRL::ComPtr<T>;

enum : uint8_t { kBold = 1, kNumeric = 2 };

constexpr size_t kMeasureRows = 1000;

std::wstring Unescape(std::wstring_view field, bool* bold) {
    std::wstring out;
    out.reserve(field.size());
    if (bold) *bold = false;
    size_t i = 0;
    if (field.size() >= 2 && field[0] == L'\\' && field[1] == L'B') {
        if (bold) *bold = true;
        i = 2;
    }
    for (; i < field.size(); ++i) {
        if (field[i] == L'\\' && i + 1 < field.size()) {
            const wchar_t n = field[++i];
            out += n == L't' ? L'\t' : n == L'n' ? L'\n' : n;
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

size_t ToSize(std::wstring_view v) {
    size_t n = 0;
    for (const wchar_t c : v) { if (c < L'0' || c > L'9') break; n = n * 10 + static_cast<size_t>(c - L'0'); }
    return n;
}

bool LooksNumeric(std::wstring_view s) {
    size_t i = 0;
    while (i < s.size() && s[i] == L' ') ++i;
    if (i < s.size() && (s[i] == L'-' || s[i] == L'+')) ++i;
    if (i < s.size() && (s[i] == L'$' || s[i] == L'\x00A5' || s[i] == L'\x20AC' || s[i] == L'\xFFE5')) ++i;
    bool digit = false;
    for (; i < s.size(); ++i) {
        const wchar_t c = s[i];
        if (c >= L'0' && c <= L'9') digit = true;
        else if (c != L'.' && c != L',' && c != L'%' && c != L' ' && c != L'e' && c != L'E' &&
                 c != L'-' && c != L'+')
            return false;
    }
    return digit;
}

std::wstring ColumnName(size_t index) {
    std::wstring name;
    ++index;
    while (index) {
        const size_t rem = (index - 1) % 26;
        name.insert(name.begin(), static_cast<wchar_t>(L'A' + rem));
        index = (index - 1) / 26;
    }
    return name;
}

std::wstring Grouped(size_t value) {
    std::wstring digits = std::to_wstring(value), out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0) out += L',';
        out += digits[i];
    }
    return out;
}

bool Zh() { return pulse::l10n::IsChinese(); }

// Column types shown under the header: what most filled cells look like.
enum : uint8_t { kTypeNone = 0, kTypeText, kTypeNumber, kTypePercent, kTypeDate };

bool LooksDate(std::wstring_view s) {
    while (!s.empty() && s.front() == L' ') s.remove_prefix(1);
    // 2026-10-22, 2026/10/22, 2026.10.22 (time may follow), or 10/22/2026.
    size_t digits[3]{}, part = 0, i = 0;
    wchar_t sep = 0;
    for (; i < s.size() && part < 3; ++i) {
        const wchar_t c = s[i];
        if (c >= L'0' && c <= L'9') { ++digits[part]; continue; }
        if ((c == L'-' || c == L'/' || c == L'.') && part < 2 && digits[part] && (!sep || c == sep)) {
            sep = c; ++part; continue;
        }
        break;
    }
    if (part != 2 || !digits[2]) return false;
    if (i < s.size() && s[i] != L' ' && s[i] != L'T') return false;
    const bool ymd = digits[0] == 4 && digits[1] <= 2 && digits[2] <= 2;
    const bool mdy = digits[0] <= 2 && digits[1] <= 2 && digits[2] == 4;
    return ymd || mdy;
}

uint8_t CellType(const std::wstring& text, uint8_t flags) {
    if (text.empty()) return kTypeNone;
    if (LooksDate(text)) return kTypeDate;
    if ((flags & kNumeric) || LooksNumeric(text)) {
        size_t end = text.size();
        while (end > 0 && text[end - 1] == L' ') --end;
        return end > 0 && text[end - 1] == L'%' ? kTypePercent : kTypeNumber;
    }
    return kTypeText;
}

D2D1_RECT_F R(float l, float t, float r, float b) { return D2D1::RectF(l, t, r, b); }

}  // namespace

// ---------------------------------------------------------------- payload

bool TableView::SetPayload(const std::shared_ptr<const std::wstring>& payload) {
    if (payload && payload_identity_.lock() == payload && !sheets_.empty()) {
        if (lazy_sheets_ && sheet_ == response_sheet_)
            awaiting_sheet_ = pending_sheet_request_ = false;
        return true;
    }
    if (!payload) return false;
    const bool accepted = SetPayloadImpl(*payload, payload);
    // Table responses may be valid but superseded: remember only committed content.
    if (accepted && Payload() == *payload) {
        StorePayload(*payload, payload);
        payload_identity_ = payload;
    }
    return accepted;
}

bool TableView::SetPayload(const std::wstring& payload) {
    return SetPayloadImpl(payload, {});
}

bool TableView::SetPayloadImpl(const std::wstring& payload,
                           std::shared_ptr<const std::wstring> owner) {
    payload_identity_.reset();
    if (payload == Payload() && !sheets_.empty()) {
        if (lazy_sheets_ && sheet_ == response_sheet_)
            awaiting_sheet_ = pending_sheet_request_ = false;
        return true;
    }
    if (payload.rfind(L"PULSETBL\t1\n", 0) != 0) return false;
    std::vector<Sheet> sheets;
    std::wstring source;
    bool spreadsheet = false;
    size_t total_sheets = 0, loaded_sheets = 0, selected_sheet = 0;
    bool lazy_sheets = false;
    size_t pos = payload.find(L'\n') + 1;
    while (pos < payload.size()) {
        size_t end = payload.find(L'\n', pos);
        if (end == std::wstring::npos) end = payload.size();
        const std::wstring_view line(payload.data() + pos, end - pos);
        pos = end + 1;
        if (line.size() < 2 || line[1] != L'\t') continue;
        if (line[0] == L'S') {
            const auto f = Split(line.substr(2), L'\t');
            if (f.size() < 7) continue;
            Sheet sheet;
            spreadsheet = f[0] == L"xlsx";
            sheet.name = Unescape(f[1], nullptr);
            sheet.cols = (std::min)(ToSize(f[2]), size_t{4096});
            sheet.truncated = f[4] == L"1";
            sheet.detail = Unescape(f[5], nullptr);
            sheet.header = f[6] == L"1";
            const size_t rows = (std::min)(ToSize(f[3]), size_t{200000});
            sheet.cells.reserve(rows);
            sheet.flags.reserve(rows);
            sheets.push_back(std::move(sheet));
        } else if (line[0] == L'R' && !sheets.empty()) {
            Sheet& sheet = sheets.back();
            const auto f = Split(line.substr(2), L'\t');
            std::vector<std::wstring> row(sheet.cols);
            std::vector<uint8_t> flags(sheet.cols, 0);
            for (size_t c = 0; c < f.size() && c < sheet.cols; ++c) {
                bool bold = false;
                std::wstring text = Unescape(f[c], &bold);
                // One line per cell: newlines and tabs show as spaces (same length,
                // so find offsets stay aligned).
                for (auto& ch : text) if (ch == L'\n' || ch == L'\t' || ch == L'\r') ch = L' ';
                if (bold) flags[c] |= kBold;
                if (LooksNumeric(text)) flags[c] |= kNumeric;
                row[c] = std::move(text);
            }
            sheet.fills.emplace_back(sheet.cols, 0);
            sheet.cells.push_back(std::move(row));
            sheet.flags.push_back(std::move(flags));
        } else if (line[0] == L'F' && !sheets.empty()) {
            const auto f = Split(line.substr(2), L'\t');
            if (f.size() == 3) {
                auto& sheet = sheets.back();
                const size_t r = ToSize(f[0]), c = ToSize(f[1]);
                if (r < sheet.fills.size() && c < sheet.cols && f[2].size() == 8 && std::all_of(f[2].begin(), f[2].end(), [](wchar_t ch) { return (ch >= L'0' && ch <= L'9') || (ch >= L'A' && ch <= L'F') || (ch >= L'a' && ch <= L'f'); }))
                    sheet.fills[r][c] = static_cast<unsigned>(std::wcstoul(std::wstring(f[2]).c_str(), nullptr, 16));
            }
        } else if (line[0] == L'W') {
            const auto f = Split(line.substr(2), L'\t');
            if (f.size() >= 2) { total_sheets = ToSize(f[0]); loaded_sheets = ToSize(f[1]); }
            if (f.size() >= 3) { selected_sheet = ToSize(f[2]); lazy_sheets = true; }
        } else if (line[0] == L'X') {
            source = Unescape(line.substr(2), nullptr);
        }
    }
    if (sheets.empty()) return false;
    if (selected_sheet >= sheets.size()) return false;
    // A response for a superseded sheet must not undo a more recent switch.
    // File changes are separated by Clear(); compare names as an additional
    // guard against treating a different workbook as an in-flight response.
    if (lazy_sheets && lazy_sheets_ && sheets.size() == sheets_.size()) {
        bool same_workbook = true;
        for (size_t i = 0; i < sheets.size(); ++i)
            if (sheets[i].name != sheets_[i].name) { same_workbook = false; break; }
        if (same_workbook && selected_sheet != sheet_) return true;
    }
    for (size_t i = 0; i < (std::min)(sheets.size(), sheets_.size()); ++i) {
        if (sheets[i].name == sheets_[i].name) {
            sheets[i].tab_width = sheets_[i].tab_width;
            sheets[i].selected_tab_width = sheets_[i].selected_tab_width;
        }
    }
    StorePayload(payload, std::move(owner));
    sheets_ = std::move(sheets);
    source_ = std::move(source);
    spreadsheet_ = spreadsheet;
    total_sheets_ = total_sheets ? total_sheets : sheets_.size();
    loaded_sheets_ = total_sheets ? loaded_sheets : sheets_.size();
    sheet_ = selected_sheet;
    response_sheet_ = selected_sheet;
    lazy_sheets_ = lazy_sheets;
    awaiting_sheet_ = pending_sheet_request_ = false;
    sel_valid_ = false;
    drag_ = Drag::None;
    hover_tab_ = tip_row_ = tip_col_ = -1;
    tip_shown_ = false;
    BuildPlain();
    return true;
}

void TableView::Clear() {
    payload_identity_.reset();
    ++text_revision_;
    decltype(payload_){}.swap(payload_);
    shared_payload_.reset();
    decltype(source_){}.swap(source_);
    decltype(plain_){}.swap(plain_);
    decltype(sheets_){}.swap(sheets_);
    total_sheets_ = loaded_sheets_ = 0;
    lazy_sheets_ = awaiting_sheet_ = pending_sheet_request_ = false;
    spreadsheet_ = false;
    decltype(row_offsets_){}.swap(row_offsets_);
    sheet_ = 0;
    first_visible_sheet_ = 0;
    response_sheet_ = 0;
    sel_valid_ = false;
    drag_ = Drag::None;
    hover_tab_ = tip_row_ = tip_col_ = -1;
    tip_shown_ = false;
}

size_t TableView::HeaderRows() const noexcept {
    const Sheet* s = Current();
    return s && s->header && !s->cells.empty() ? 1 : 0;
}

size_t TableView::BodyRows() const noexcept {
    const Sheet* s = Current();
    return s ? s->cells.size() - HeaderRows() : 0;
}

void TableView::BuildPlain() {
    ++text_revision_;
    plain_.clear();
    row_offsets_.clear();
    const Sheet* s = Current();
    if (!s) return;
    row_offsets_.reserve(s->cells.size());
    for (const auto& row : s->cells) {
        row_offsets_.push_back(static_cast<uint32_t>(plain_.size()));
        for (size_t c = 0; c < row.size(); ++c) {
            if (c) plain_ += L'\t';
            plain_ += row[c];
        }
        plain_ += L'\n';
    }
}

std::vector<std::wstring> TableView::StatusParts(uint32_t encoding) const {
    std::vector<std::wstring> parts;
    const Sheet* s = Current();
    if (!s) return parts;
    const bool zh = Zh();
    const auto shape = [&] {
        const std::wstring rows = Grouped(BodyRows()), cols = Grouped(s->cols);
        std::wstring size = zh ? rows + pulse::l10n::Cn(L" \x884C \x00D7 ") + cols + pulse::l10n::Cn(L" \x5217")
                               : rows + L" rows \x00D7 " + cols + L" columns";
        if (s->truncated) size = zh ? pulse::l10n::Cn(L"\x524D ") + size + pulse::l10n::Cn(L"\xFF08\x5DF2\x622A\x65AD\xFF09")
                                    : L"first " + size + L" (truncated)";
        return size;
    };
    if (spreadsheet_) {
        // The window adds the format and size around: sheet name, then its shape.
        if (!s->name.empty() && !footer_tabs_) parts.push_back(s->name);
        parts.push_back(shape());
        return parts;
    }
    parts.push_back(s->detail == L"tab" ? L"TSV" : L"CSV");
    std::wstring detail;
    if (s->detail == L"tab") detail = pulse::l10n::Get(pulse::l10n::StringId::Tab).c_str();
    else if (s->detail == L"semicolon") detail = pulse::l10n::Get(pulse::l10n::StringId::Semicolon).c_str();
    else if (s->detail == L"pipe") detail = pulse::l10n::Get(pulse::l10n::StringId::Pipe).c_str();
    else detail = pulse::l10n::Get(pulse::l10n::StringId::Comma).c_str();
    std::wstring name;
    switch (static_cast<ipc::PreviewTextEncoding>(encoding)) {
    case ipc::PreviewTextEncoding::Utf8: name = L"UTF-8"; break;
    case ipc::PreviewTextEncoding::Utf8Bom: name = L"UTF-8 BOM"; break;
    case ipc::PreviewTextEncoding::Utf16Le: name = L"UTF-16 LE"; break;
    case ipc::PreviewTextEncoding::Utf16Be: name = L"UTF-16 BE"; break;
    case ipc::PreviewTextEncoding::Ansi:
        switch (GetACP()) {
        case 936: name = L"GBK"; break;
        case 950: name = L"Big5"; break;
        case 932: name = L"Shift-JIS"; break;
        case 949: name = L"EUC-KR"; break;
        case 1252: name = L"Windows-1252"; break;
        default: name = L"ANSI " + std::to_wstring(GetACP()); break;
        }
        break;
    default: break;
    }
    parts.push_back(std::move(detail));
    if (!name.empty()) parts.push_back(std::move(name));
    parts.push_back(shape());
    return parts;
}

// ---------------------------------------------------------------- layout

bool TableView::EnsureFormats(IDWriteFactory2* factory, float scale) {
    if (!factory) return false;
    if (factory == factory_ && scale == format_scale_ && text_) return true;
    for (auto& sheet : sheets_) sheet.tab_width = sheet.selected_tab_width = -1.0f;
    factory_ = factory;
    format_scale_ = scale;
    text_.Reset(); text_right_.Reset(); bold_.Reset(); bold_right_.Reset(); small_.Reset(); tip_.Reset();
    const float size = 12.5f * scale;
    typography::CreateTextFormat(factory, {typography::FontRole::Text, size}, &text_);
    typography::CreateTextFormat(factory, {typography::FontRole::Text, size}, &text_right_);
    typography::CreateTextFormat(factory, {typography::FontRole::Text, size, DWRITE_FONT_WEIGHT_SEMI_BOLD}, &bold_);
    typography::CreateTextFormat(factory, {typography::FontRole::Text, size, DWRITE_FONT_WEIGHT_SEMI_BOLD}, &bold_right_);
    typography::CreateTextFormat(factory, {typography::FontRole::Text, 12.0f * scale}, &small_);
    typography::CreateTextFormat(factory, {typography::FontRole::Text, size}, &tip_);
    if (!text_ || !text_right_ || !bold_ || !bold_right_ || !small_ || !tip_) return false;
    WrlPtr<IDWriteInlineObject> ellipsis;
    factory->CreateEllipsisTrimmingSign(text_.Get(), &ellipsis);
    const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    for (IDWriteTextFormat* f : {text_.Get(), text_right_.Get(), bold_.Get(), bold_right_.Get(), small_.Get()}) {
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        f->SetTrimming(&trimming, ellipsis.Get());
    }
    text_right_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    bold_right_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    tip_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    tip_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    // Reference advances for the column-width estimate.
    const auto advance = [&](const wchar_t* sample, float fallback) {
        WrlPtr<IDWriteTextLayout> layout;
        const UINT32 n = static_cast<UINT32>(wcslen(sample));
        if (FAILED(factory->CreateTextLayout(sample, n, text_.Get(), 10000.0f, 100.0f, &layout)) || !layout)
            return fallback;
        DWRITE_TEXT_METRICS m{};
        if (FAILED(layout->GetMetrics(&m)) || m.widthIncludingTrailingWhitespace <= 0.0f) return fallback;
        return m.widthIncludingTrailingWhitespace / static_cast<float>(n);
    };
    digit_w_ = advance(L"0123456789", 7.0f * scale);
    cjk_w_ = advance(L"\x4E2D\x6587\x8868\x683C\x9884", 12.5f * scale);
    latin_w_ = advance(L"abcdefghijklmnopqrstuvwxyz ,.-", 6.5f * scale);
    upper_w_ = advance(L"ABCDEFGHIJKLMNOPQRSTUVWXYZ", 8.0f * scale);
    for (Sheet& s : sheets_) s.measured_scale = 0.0f;
    return true;
}

float TableView::Estimate(const std::wstring& text) const {
    float w = 0.0f;
    for (const wchar_t c : text) {
        if (c >= L'0' && c <= L'9') w += digit_w_;
        else if (c >= 0x2E80) w += cjk_w_;
        else if (c >= L'A' && c <= L'Z') w += upper_w_;
        else w += latin_w_;
    }
    return w;
}

void TableView::EnsureWidths(Sheet& s) {
    if (s.col_types.size() != s.cols) {
        // A type wins a column when at least 80% of its filled cells agree.
        s.col_types.assign(s.cols, kTypeNone);
        const size_t first = s.header && !s.cells.empty() ? 1 : 0;
        const size_t last = (std::min)(s.cells.size(), first + kMeasureRows);
        for (size_t c = 0; c < s.cols; ++c) {
            size_t counts[5]{};
            size_t filled = 0;
            for (size_t r = first; r < last; ++r) {
                if (c >= s.cells[r].size()) continue;
                const uint8_t type = CellType(s.cells[r][c], s.flags[r][c]);
                if (type == kTypeNone) continue;
                ++counts[type];
                ++filled;
            }
            if (!filled) continue;
            s.col_types[c] = kTypeText;
            const size_t numeric = counts[kTypeNumber] + counts[kTypePercent];
            if (counts[kTypeDate] * 5 >= filled * 4) s.col_types[c] = kTypeDate;
            else if (counts[kTypePercent] * 5 >= filled * 4) s.col_types[c] = kTypePercent;
            else if (numeric * 5 >= filled * 4) s.col_types[c] = kTypeNumber;
        }
    }
    if (s.measured_scale == scale_ && s.col_x.size() == s.cols + 1) return;
    s.measured_scale = scale_;
    const float pad = 10.0f * scale_;
    const float min_w = 48.0f * scale_, max_w = 320.0f * scale_;
    std::vector<float> widths(s.cols, 0.0f);
    const bool letters = !s.header;
    for (size_t c = 0; c < s.cols && letters; ++c) widths[c] = Estimate(ColumnName(c));
    const size_t rows = (std::min)(s.cells.size(), kMeasureRows);
    for (size_t r = 0; r < rows; ++r) {
        const auto& row = s.cells[r];
        for (size_t c = 0; c < s.cols && c < row.size(); ++c) {
            if (row[c].empty()) continue;
            float w = Estimate(row[c]);
            if ((s.flags[r][c] & kBold) || (s.header && r == 0)) w *= 1.08f;
            widths[c] = (std::max)(widths[c], w);
        }
    }
    s.col_x.assign(s.cols + 1, 0.0f);
    for (size_t c = 0; c < s.cols; ++c)
        s.col_x[c + 1] = s.col_x[c] + std::clamp(widths[c] + pad * 2.0f + 2.0f * scale_, min_w, max_w);
    const size_t body = s.cells.size() - (s.header && !s.cells.empty() ? 1 : 0);
    const size_t digits = std::to_wstring((std::max)(body, size_t{1})).size();
    s.num_w = (std::max)(38.0f * scale_, static_cast<float>(digits) * digit_w_ + pad * 1.6f);
}

TableView::Geometry TableView::Measure() const {
    Geometry g;
    const Sheet* s = Current();
    const float k = scale_;
    g.row_h = 26.0f * k;
    g.head_h = (s && !s->col_types.empty() ? 42.0f : 28.0f) * k;  // name over the inferred type
    g.pad = 10.0f * k;
    g.grid = view_;
    if (spreadsheet_ && !footer_tabs_) {
        g.tabs = R(view_.left, view_.bottom - 34.0f * k, view_.right, view_.bottom);
        g.grid.bottom = g.tabs.top;
    }
    if (!s) return g;
    g.body = R(g.grid.left + s->num_w, g.grid.top + g.head_h, g.grid.right, g.grid.bottom);
    g.content_w = s->col_x.empty() ? 0.0f : s->col_x.back();
    // CSV leaves room under the last row for the status pill.
    g.content_h = static_cast<float>(BodyRows()) * g.row_h + 48.0f * k;  // room for the end line
    g.max_sx = (std::max)(0.0f, g.content_w - (g.body.right - g.body.left));
    g.max_sy = (std::max)(0.0f, g.content_h - (g.body.bottom - g.body.top));
    return g;
}

void TableView::Clamp() {
    Sheet* s = Current();
    if (!s) return;
    const Geometry g = Measure();
    s->sx = std::clamp(s->sx, 0.0f, g.max_sx);
    s->sy = std::clamp(s->sy, 0.0f, g.max_sy);
}

WrlPtr<IDWriteTextLayout> TableView::CellLayout(const std::wstring& text, bool bold, bool right,
                                               float width) const {
    WrlPtr<IDWriteTextLayout> layout;
    IDWriteTextFormat* f = bold ? (right ? bold_right_.Get() : bold_.Get())
                                : (right ? text_right_.Get() : text_.Get());
    if (factory_ && f)
        factory_->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), f,
                                   (std::max)(1.0f, width), 26.0f * scale_, &layout);
    return layout;
}

// ---------------------------------------------------------------- drawing

void TableView::Draw(ID2D1DeviceContext* dc, IDWriteFactory2* factory, const D2D1_RECT_F& rect,
                     const Theme& theme, bool dark, float scale, const D2D1_COLOR_F& background,
                     SearchHighlights matches) {
    Sheet* sheet = Current();
    if (!dc || !sheet || !EnsureFormats(factory, scale)) return;
    view_ = rect;
    scale_ = scale;
    if (brush_owner_ != dc || !brush_) {
        brush_.Reset();
        brush_owner_ = dc;
        dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &brush_);
    }
    if (!brush_) return;
    EnsureWidths(*sheet);
    Clamp();
    Sheet& s = *sheet;
    const Geometry g = Measure();
    const float k = scale;
    const size_t hdr = HeaderRows();
    const size_t body_rows = BodyRows();
    ID2D1SolidColorBrush* b = brush_.Get();
    const auto fill = [&](const D2D1_RECT_F& r, const D2D1_COLOR_F& c) { b->SetColor(c); dc->FillRectangle(r, b); };
    const D2D1_COLOR_F zebra = dark ? D2D1::ColorF(1, 1, 1, 0.025f) : D2D1::ColorF(0, 0, 0, 0.022f);
    const D2D1_COLOR_F divider = dark ? D2D1::ColorF(1, 1, 1, 0.05f) : D2D1::ColorF(0, 0, 0, 0.05f);
    const D2D1_COLOR_F header_bg = dark ? HexColor(0x2A2A2A) : HexColor(0xFAFAFA);
    const D2D1_COLOR_F header_line = dark ? HexColor(0x3A3A3A) : HexColor(0xDDDDDD);
    const D2D1_COLOR_F sel_line = dark ? HexColor(0x60CDFF) : HexColor(0x005FB8);
    const D2D1_COLOR_F sel_fill = WithAlpha(sel_line, dark ? 0.16f : 0.12f);
    const D2D1_COLOR_F dim = WithAlpha(theme.text, 0.45f);

    dc->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (s.cols == 0 || s.cells.empty()) {
        std::wstring empty = pulse::l10n::Get(pulse::l10n::StringId::EmptySheet).c_str();
        if (spreadsheet_ && !s.detail.empty()) {
            if (awaiting_sheet_) empty = pulse::l10n::Get(pulse::l10n::StringId::LoadingSheet).c_str();
            else if (s.detail == L"hidden") empty = pulse::l10n::Get(pulse::l10n::StringId::HiddenSheetOpenInTheOriginalApp).c_str();
            else if (s.detail == L"not-loaded") empty = pulse::l10n::Get(pulse::l10n::StringId::SelectASheetToLoadItsContent).c_str();
            else if (s.detail == L"time-limit") empty = pulse::l10n::Get(pulse::l10n::StringId::NotLoadedPreviewTimeLimitReached).c_str();
            else if (s.detail == L"payload-limit") empty = pulse::l10n::Get(pulse::l10n::StringId::NotLoadedPreviewSizeLimitReached).c_str();
            else if (s.detail == L"missing-relationship") empty = pulse::l10n::Get(pulse::l10n::StringId::NotLoadedMissingWorksheetRelationship).c_str();
            else empty = pulse::l10n::Get(pulse::l10n::StringId::NotLoadedWorksheetContentCouldNotBeRead).c_str();
        } else if (spreadsheet_ && s.truncated) {
            empty = pulse::l10n::Get(pulse::l10n::StringId::NoCellsAvailablePreviewContentWasTruncated).c_str();
        }
        text_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        text_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        b->SetColor(theme.text_secondary);
        dc->DrawTextW(empty.data(), static_cast<UINT32>(empty.size()), text_.Get(), g.grid, b);
        text_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        text_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }

    // Visible ranges.
    const float view_w = g.body.right - g.body.left;
    const size_t r0 = static_cast<size_t>((std::max)(0.0f, s.sy / g.row_h));
    const size_t r1 = (std::min)(body_rows, static_cast<size_t>((s.sy + (g.body.bottom - g.body.top)) / g.row_h) + 1);
    size_t c0 = 0;
    while (c0 < s.cols && s.col_x[c0 + 1] <= s.sx) ++c0;
    size_t c1 = c0;
    while (c1 < s.cols && s.col_x[c1] < s.sx + view_w) ++c1;
    const auto row_top = [&](size_t r) { return g.body.top + static_cast<float>(r) * g.row_h - s.sy; };
    const auto col_left = [&](size_t c) { return g.body.left + s.col_x[c] - s.sx; };

    // Body: zebra, highlights, selection, text, dividers.
    dc->PushAxisAlignedClip(g.body, D2D1_ANTIALIAS_MODE_ALIASED);
    for (size_t r = r0; r < r1; ++r)
        if (r % 2 == 1) fill(R(g.body.left, row_top(r), g.body.right, row_top(r) + g.row_h), zebra);
    for (size_t r = r0; r < r1; ++r) {
        const size_t source = r + HeaderRows();
        if (source >= s.fills.size()) continue;
        for (size_t c = c0; c < c1; ++c)
            if (s.fills[source][c]) fill(R(col_left(c), row_top(r), col_left(c + 1), row_top(r) + g.row_h), HexColor(s.fills[source][c] & 0xFFFFFF));
    }
    // Crosshair: the hovered row and column, faintly.
    const D2D1_COLOR_F cross = WithAlpha(sel_line, dark ? 0.07f : 0.05f);
    const bool crosshair = drag_ == Drag::None && hover_row_ >= 0 && hover_col_ >= 0 &&
                           static_cast<size_t>(hover_row_) < body_rows && static_cast<size_t>(hover_col_) < s.cols;
    if (crosshair) {
        const size_t hr = static_cast<size_t>(hover_row_), hc = static_cast<size_t>(hover_col_);
        fill(R(g.body.left, row_top(hr), (std::min)(g.body.right, col_left(s.cols)), row_top(hr) + g.row_h), cross);
        fill(R(col_left(hc), g.body.top, col_left(hc + 1), (std::min)(g.body.bottom, row_top(body_rows))), cross);
    }
    int sr0 = 0, sr1 = -1, sc0 = 0, sc1 = -1;
    if (sel_valid_) {
        sr0 = (std::min)(sel_r0_, sel_r1_); sr1 = (std::max)(sel_r0_, sel_r1_);
        sc0 = (std::min)(sel_c0_, sel_c1_); sc1 = (std::max)(sel_c0_, sel_c1_);
        const D2D1_RECT_F box = R(col_left(static_cast<size_t>(sc0)), row_top(static_cast<size_t>(sr0)),
                                  col_left(static_cast<size_t>(sc1) + 1), row_top(static_cast<size_t>(sr1) + 1));
        fill(box, sel_fill);
    }
    // Find matches in the visible rows.
    if (!matches.empty() && r1 > r0) {
        const uint32_t first = row_offsets_[r0 + hdr];
        const uint32_t last = r1 + hdr < row_offsets_.size() ? row_offsets_[r1 + hdr]
                                                             : static_cast<uint32_t>(plain_.size());
        for (const Highlight match : matches.Intersecting(first, last)) {
            size_t row = 0, col = 0;
            uint32_t in_cell = 0;
            if (!OffsetToCell(match.start, row, col, in_cell) || row < hdr + r0 || col < c0 || col >= c1) continue;
            const std::wstring& text = s.cells[row][col];
            const uint32_t len = (std::min)(match.length, static_cast<uint32_t>(text.size()) - (std::min)(in_cell, static_cast<uint32_t>(text.size())));
            if (!len) continue;
            const bool right = (s.flags[row][col] & kNumeric) != 0;
            const float cw = s.col_x[col + 1] - s.col_x[col] - g.pad * 2.0f;
            auto layout = CellLayout(text, (s.flags[row][col] & kBold) != 0, right, cw);
            if (!layout) continue;
            UINT32 count = 0;
            layout->HitTestTextRange(in_cell, len, 0, 0, nullptr, 0, &count);
            if (!count) continue;
            std::vector<DWRITE_HIT_TEST_METRICS> m(count);
            if (FAILED(layout->HitTestTextRange(in_cell, len, 0, 0, m.data(), count, &count))) continue;
            const float ox = col_left(col) + g.pad;
            const float oy = row_top(row - hdr);
            b->SetColor(match.current ? HexColor(0xF59E0B, 0.85f) : HexColor(0xFACC15, dark ? 0.45f : 0.55f));
            for (UINT32 i = 0; i < count; ++i) {
                const float l = ox + m[i].left, rr = (std::min)(ox + m[i].left + m[i].width, ox + cw);
                if (rr > l) dc->FillRoundedRectangle(D2D1::RoundedRect(R(l, oy + 4.0f * k, rr, oy + g.row_h - 4.0f * k), 2.0f * k, 2.0f * k), b);
            }
        }
    }
    for (size_t r = r0; r < r1; ++r) {
        const auto& row = s.cells[r + hdr];
        const auto& flags = s.flags[r + hdr];
        const float top = row_top(r);
        for (size_t c = c0; c < c1; ++c) {
            if (row[c].empty()) continue;
            const bool bold = (flags[c] & kBold) != 0, right = (flags[c] & kNumeric) != 0;
            IDWriteTextFormat* f = bold ? (right ? bold_right_.Get() : bold_.Get())
                                        : (right ? text_right_.Get() : text_.Get());
            auto text_color = theme.text;
            if (r + hdr < s.fills.size() && s.fills[r + hdr][c]) {
                const auto color = HexColor(s.fills[r + hdr][c] & 0xFFFFFF);
                text_color = 0.2126f * color.r + 0.7152f * color.g + 0.0722f * color.b > 0.55f
                    ? HexColor(0x202020) : HexColor(0xFFFFFF);
            }
            b->SetColor(text_color);
            dc->DrawTextW(row[c].data(), static_cast<UINT32>(row[c].size()), f,
                          R(col_left(c) + g.pad, top, col_left(c + 1) - g.pad, top + g.row_h), b,
                          D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }
    b->SetColor(divider);
    for (size_t c = c0; c < c1; ++c) {
        const float x = std::floor(col_left(c + 1)) - 0.5f;
        dc->DrawLine(D2D1::Point2F(x, g.body.top), D2D1::Point2F(x, (std::min)(g.body.bottom, row_top(body_rows))), b, 1.0f);
    }
    // Below the last row: how many rows there are and whether that is all.
    if (body_rows > 0 && row_top(body_rows) < g.body.bottom) {
        wchar_t line[160];
        swprintf_s(line, pulse::l10n::Get(s.truncated ? pulse::l10n::StringId::PreviewTablePartialRows
                                                      : pulse::l10n::StringId::PreviewTableAllRows).c_str(),
                   Grouped(body_rows).c_str());
        b->SetColor(dim);
        dc->DrawTextW(line, static_cast<UINT32>(wcslen(line)), small_.Get(),
                      R(g.body.left + 2.0f * g.pad - s.sx, row_top(body_rows) + 4.0f * k,
                        g.body.right + 2000.0f * k, row_top(body_rows) + 40.0f * k), b);
    }
    if (sel_valid_) {
        const D2D1_RECT_F box = R(col_left(static_cast<size_t>(sc0)) + 1.0f * k, row_top(static_cast<size_t>(sr0)) + 1.0f * k,
                                  col_left(static_cast<size_t>(sc1) + 1) - 1.0f * k, row_top(static_cast<size_t>(sr1) + 1) - 1.0f * k);
        b->SetColor(sel_line);
        dc->DrawRectangle(box, b, 2.0f * k);
    }
    dc->PopAxisAlignedClip();

    // Pinned row-number column.
    const D2D1_RECT_F numbers = R(g.grid.left, g.body.top, g.body.left, g.grid.bottom);
    fill(numbers, background);
    dc->PushAxisAlignedClip(numbers, D2D1_ANTIALIAS_MODE_ALIASED);
    for (size_t r = r0; r < r1; ++r) {
        const float top = row_top(r);
        const D2D1_RECT_F cell = R(numbers.left, top, numbers.right, top + g.row_h);
        if (r % 2 == 1) fill(cell, zebra);
        const bool selected = sel_valid_ && static_cast<int>(r) >= sr0 && static_cast<int>(r) <= sr1;
        if (selected) fill(cell, sel_fill);
        else if (crosshair && static_cast<int>(r) == hover_row_) fill(cell, cross);
        const std::wstring n = std::to_wstring(r + 1);
        b->SetColor(selected ? theme.text : dim);
        dc->DrawTextW(n.data(), static_cast<UINT32>(n.size()), text_right_.Get(),
                      R(cell.left + 4.0f * k, top, cell.right - g.pad * 0.8f, top + g.row_h), b);
    }
    b->SetColor(divider);
    dc->DrawLine(D2D1::Point2F(numbers.right - 0.5f, numbers.top), D2D1::Point2F(numbers.right - 0.5f, numbers.bottom), b, 1.0f);
    dc->PopAxisAlignedClip();

    // Pinned header row: column names (CSV header) or letters.
    const D2D1_RECT_F head = R(g.grid.left, g.grid.top, g.grid.right, g.body.top);
    fill(head, header_bg);
    {
        const D2D1_RECT_F cols = R(g.body.left, head.top, head.right, head.bottom);
        dc->PushAxisAlignedClip(cols, D2D1_ANTIALIAS_MODE_ALIASED);
        for (size_t c = c0; c < c1; ++c) {
            const bool selected = sel_valid_ && static_cast<int>(c) >= sc0 && static_cast<int>(c) <= sc1;
            if (selected) fill(R(col_left(c), head.top, col_left(c + 1), head.bottom), sel_fill);
            else if (crosshair && static_cast<int>(c) == hover_col_)
                fill(R(col_left(c), head.top, col_left(c + 1), head.bottom), cross);
            const std::wstring label = hdr ? s.cells[0][c] : ColumnName(c);
            const float cw = s.col_x[c + 1] - s.col_x[c] - g.pad * 2.0f;
            auto layout = CellLayout(label, true, false, cw);
            const bool typed = c < s.col_types.size() && s.col_types[c] != kTypeNone;
            const float name_h = s.col_types.empty() ? head.bottom - head.top : 28.0f * k;
            if (layout) layout->SetMaxHeight(name_h);
            if (typed) {
                static constexpr pulse::l10n::StringId kTypeNames[] = {
                    pulse::l10n::StringId::PreviewColumnText, pulse::l10n::StringId::PreviewColumnText,
                    pulse::l10n::StringId::PreviewColumnNumber, pulse::l10n::StringId::PreviewColumnPercent,
                    pulse::l10n::StringId::PreviewColumnDate};
                const std::wstring type = pulse::l10n::Get(kTypeNames[s.col_types[c]]).c_str();
                b->SetColor(dim);
                dc->DrawTextW(type.data(), static_cast<UINT32>(type.size()), small_.Get(),
                              R(col_left(c) + g.pad, head.top + 22.0f * k, col_left(c + 1) - g.pad, head.bottom - 2.0f * k),
                              b, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            if (hdr && layout) {
                for (const Highlight match : matches.Intersecting(0, row_offsets_.size() > 1 ? row_offsets_[1] : plain_.size())) {
                    size_t row = 0, col = 0;
                    uint32_t in_cell = 0;
                    if (!OffsetToCell(match.start, row, col, in_cell) || row != 0 || col != c) continue;
                    const uint32_t len = (std::min)(match.length, static_cast<uint32_t>(label.size()) - (std::min)(in_cell, static_cast<uint32_t>(label.size())));
                    UINT32 count = 0;
                    layout->HitTestTextRange(in_cell, len, 0, 0, nullptr, 0, &count);
                    if (!count) continue;
                    std::vector<DWRITE_HIT_TEST_METRICS> metrics(count);
                    if (FAILED(layout->HitTestTextRange(in_cell, len, 0, 0, metrics.data(), count, &count))) continue;
                    b->SetColor(match.current ? HexColor(0xF59E0B, 0.85f) : HexColor(0xFACC15, dark ? 0.45f : 0.55f));
                    for (UINT32 i = 0; i < count; ++i) {
                        const float left = col_left(c) + g.pad + metrics[i].left;
                        const float right = (std::min)(left + metrics[i].width, col_left(c + 1) - g.pad);
                        if (right > left) dc->FillRoundedRectangle(D2D1::RoundedRect(R(left, head.top + 4.0f * k,
                            right, head.bottom - 4.0f * k), 2.0f * k, 2.0f * k), b);
                    }
                }
            }
            b->SetColor(theme.text);
            if (layout) dc->DrawTextLayout(D2D1::Point2F(col_left(c) + g.pad, head.top), layout.Get(), b,
                                          D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        dc->PopAxisAlignedClip();
        if (hdr) {
            const std::wstring corner = L"#";
            b->SetColor(dim);
            dc->DrawTextW(corner.data(), 1, bold_right_.Get(),
                          R(head.left + 4.0f * k, head.top, g.body.left - g.pad * 0.8f, head.bottom), b);
        }
        b->SetColor(header_line);
        dc->DrawLine(D2D1::Point2F(head.left, head.bottom - 0.5f), D2D1::Point2F(head.right, head.bottom - 0.5f), b, 1.0f);
    }

    // Scroll indicators (draggable thumbs).
    vthumb_ = hthumb_ = D2D1_RECT_F{};
    const D2D1_COLOR_F thumb = WithAlpha(theme.text, drag_ == Drag::VThumb || drag_ == Drag::HThumb ? 0.45f : 0.28f);
    if (g.max_sy > 0.0f) {
        const float track = g.body.bottom - g.body.top - 8.0f * k;
        const float h = (std::max)(28.0f * k, track * (g.body.bottom - g.body.top) / g.content_h);
        const float y = g.body.top + 4.0f * k + (track - h) * (s.sy / g.max_sy);
        vthumb_ = R(g.grid.right - 7.0f * k, y, g.grid.right - 3.0f * k, y + h);
        b->SetColor(thumb);
        dc->FillRoundedRectangle(D2D1::RoundedRect(vthumb_, 2.0f * k, 2.0f * k), b);
    }
    if (g.max_sx > 0.0f) {
        const float track = view_w - 8.0f * k;
        const float w = (std::max)(28.0f * k, track * view_w / g.content_w);
        const float x = g.body.left + 4.0f * k + (track - w) * (s.sx / g.max_sx);
        hthumb_ = R(x, g.grid.bottom - 7.0f * k, x + w, g.grid.bottom - 3.0f * k);
        b->SetColor(thumb);
        dc->FillRoundedRectangle(D2D1::RoundedRect(hthumb_, 2.0f * k, 2.0f * k), b);
    }

    // Workbook sheet tabs (the window may draw them in its footer instead).
    if (spreadsheet_ && !footer_tabs_) DrawTabsIn(dc, factory, g.tabs, theme, dark, true);
    else if (!spreadsheet_) tab_rects_.clear();

    // Full text of a clipped cell.
    if (tip_shown_ && tip_row_ >= 0 && tip_col_ >= 0 &&
        static_cast<size_t>(tip_row_) < body_rows && static_cast<size_t>(tip_col_) < s.cols) {
        const std::wstring& text = s.cells[static_cast<size_t>(tip_row_) + hdr][static_cast<size_t>(tip_col_)];
        WrlPtr<IDWriteTextLayout> layout;
        factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), tip_.Get(), 420.0f * k, 2000.0f * k, &layout);
        if (layout) {
            DWRITE_TEXT_METRICS m{};
            layout->GetMetrics(&m);
            const float h = (std::min)(m.height, 300.0f * k);
            float x = tip_x_ + 12.0f * k, y = tip_y_ + 20.0f * k;
            const float w = m.width + 20.0f * k, bh = h + 14.0f * k;
            if (x + w > rect.right - 6.0f * k) x = (std::max)(rect.left + 6.0f * k, rect.right - 6.0f * k - w);
            if (y + bh > rect.bottom - 6.0f * k) y = (std::max)(rect.top + 6.0f * k, tip_y_ - 8.0f * k - bh);
            const D2D1_RECT_F box = R(x, y, x + w, y + bh);
            b->SetColor(dark ? HexColor(0x2C2C2C) : HexColor(0xFFFFFF));
            dc->FillRoundedRectangle(D2D1::RoundedRect(box, 6.0f * k, 6.0f * k), b);
            b->SetColor(dark ? HexColor(0x454545) : HexColor(0xD0D0D0));
            dc->DrawRoundedRectangle(D2D1::RoundedRect(box, 6.0f * k, 6.0f * k), b, 1.0f);
            dc->PushAxisAlignedClip(R(box.left + 10.0f * k, box.top + 7.0f * k, box.right - 10.0f * k, box.bottom - 7.0f * k), D2D1_ANTIALIAS_MODE_ALIASED);
            b->SetColor(theme.text);
            dc->DrawTextLayout(D2D1::Point2F(box.left + 10.0f * k, box.top + 7.0f * k), layout.Get(), b);
            dc->PopAxisAlignedClip();
        }
    }
    dc->PopAxisAlignedClip();
}

// Sheet tabs: under the grid (with the sheet count on the right), or in the
// window's footer when the window hosts them there.
float TableView::DrawTabsIn(ID2D1DeviceContext* dc, IDWriteFactory2* factory, const D2D1_RECT_F& tabs,
                            const Theme& theme, bool dark, bool status) {
    tab_rects_.clear();
    Sheet* sheet = Current();
    if (!dc || !factory || !sheet || !brush_ || !small_ || !text_) return tabs.left;
    const Sheet& s = *sheet;
    const float k = scale_;
    ID2D1SolidColorBrush* b = brush_.Get();
    const auto fill = [&](const D2D1_RECT_F& r, const D2D1_COLOR_F& c) { b->SetColor(c); dc->FillRectangle(r, b); };
    const D2D1_COLOR_F dim = WithAlpha(theme.text, 0.45f);
    if (status) {
        fill(tabs, dark ? HexColor(0x1B1B1B) : HexColor(0xEBEBEB));
        b->SetColor(dark ? HexColor(0x333333) : HexColor(0xD6D6D6));
        dc->DrawLine(D2D1::Point2F(tabs.left, tabs.top + 0.5f), D2D1::Point2F(tabs.right, tabs.top + 0.5f), b, 1.0f);
    }
    // Size on the right.
    const bool zh = Zh();
    std::wstring size = std::to_wstring(sheet_ + 1) + L"/" + std::to_wstring(total_sheets_) +
        (zh ? L" 表 · 已读取 " : L" sheets · read ") + std::to_wstring(loaded_sheets_);
    if (sheets_.size() < total_sheets_)
        size += zh ? L" · 列表已截断（容量上限）" : L" · list capped (size limit)";
    if (s.truncated) size += pulse::l10n::Get(pulse::l10n::StringId::Incomplete).c_str();
    WrlPtr<IDWriteTextLayout> size_layout;
    const float status_width = (std::max)(1.0f, (tabs.right - tabs.left - 100.0f * k) * 0.55f);
    factory->CreateTextLayout(size.data(), static_cast<UINT32>(size.size()), small_.Get(), status_width, 30.0f * k, &size_layout);
    float size_w = 0.0f;
    if (size_layout && status) {
        size_layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        WrlPtr<IDWriteInlineObject> ellipsis;
        factory->CreateEllipsisTrimmingSign(small_.Get(), &ellipsis);
        const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        size_layout->SetTrimming(&trimming, ellipsis.Get());
        DWRITE_TEXT_METRICS m{};
        size_layout->GetMetrics(&m);
        size_w = (std::min)(m.width, (std::max)(0.0f, (tabs.right - tabs.left - 100.0f * k) * 0.55f));
        b->SetColor(dim);
        dc->PushAxisAlignedClip(R(tabs.right - 14.0f * k - size_w, tabs.top,
                                 tabs.right - 14.0f * k, tabs.bottom), D2D1_ANTIALIAS_MODE_ALIASED);
        dc->DrawTextLayout(D2D1::Point2F(tabs.right - 14.0f * k - size_w,
                                         (tabs.top + tabs.bottom) * 0.5f - 15.0f * k), size_layout.Get(), b);
        dc->PopAxisAlignedClip();
    }
    previous_sheet_ = R(tabs.left + 4.0f * k, tabs.top, tabs.left + 30.0f * k, tabs.bottom);
    next_sheet_ = R(tabs.left + 30.0f * k, tabs.top, tabs.left + 56.0f * k, tabs.bottom);
    b->SetColor(sheet_ > 0 ? theme.text : dim);
    dc->DrawTextW(L"‹", 1, text_.Get(), previous_sheet_, b);
    b->SetColor(sheet_ + 1 < sheets_.size() ? theme.text : dim);
    dc->DrawTextW(L"›", 1, text_.Get(), next_sheet_, b);
    float x = tabs.left + 60.0f * k;
    const float limit = (std::max)(x, tabs.right - size_w - 28.0f * k);
    // Keep earlier tabs visible when they fit; selection is not a scroll origin.
    const auto make_layout = [&](size_t i) {
        WrlPtr<IDWriteTextLayout> layout;
        const auto& name = sheets_[i].name;
        factory->CreateTextLayout(name.data(), static_cast<UINT32>(name.size()), small_.Get(),
            400.0f * k, 24.0f * k, &layout);
        if (layout && i == sheet_) layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_TEXT_RANGE{0, static_cast<UINT32>(name.size())});
        return layout;
    };
    const auto width_for = [&](size_t i, IDWriteTextLayout* ready = nullptr) {
        auto& sheet = sheets_[i];
        float& width = i == sheet_ ? sheet.selected_tab_width : sheet.tab_width;
        if (width >= 0.0f) return width;
        auto measured = ready ? WrlPtr<IDWriteTextLayout>{} : make_layout(i);
        IDWriteTextLayout* layout = ready ? ready : measured.Get();
        DWRITE_TEXT_METRICS metrics{};
        if (layout && SUCCEEDED(layout->GetMetrics(&metrics)))
            width = (std::min)(metrics.width, 240.0f * k) + 24.0f * k;
        return width >= 0.0f ? width : 24.0f * k;
    };
    float selected_extent = 0.0f;
    for (size_t i = 0; i <= sheet_; ++i) selected_extent += width_for(i) + 2.0f * k;
    first_visible_sheet_ = 0;
    while (first_visible_sheet_ < sheet_ && selected_extent - 2.0f * k > limit - x)
        selected_extent -= width_for(first_visible_sheet_++) + 2.0f * k;
    dc->PushAxisAlignedClip(R(x, tabs.top, limit, tabs.bottom), D2D1_ANTIALIAS_MODE_ALIASED);
    for (size_t i = first_visible_sheet_; i < sheets_.size() && x < limit; ++i) {
        const bool on = i == sheet_;
        auto layout = make_layout(i);
        const float w = width_for(i, layout.Get());
        const float inset = (std::max)(2.0f * k, (tabs.bottom - tabs.top - 23.0f * k) * 0.5f);
        const D2D1_RECT_F tab = R(x, tabs.top + inset, x + w, tabs.bottom - inset);
        tab_rects_.push_back(R(tab.left, tab.top, (std::min)(tab.right, limit), tab.bottom));
        if (on || static_cast<int>(i - first_visible_sheet_) == hover_tab_) {
            b->SetColor(on ? (dark ? D2D1::ColorF(1, 1, 1, 0.09f) : HexColor(0xFFFFFF))
                           : (dark ? D2D1::ColorF(1, 1, 1, 0.05f) : D2D1::ColorF(0, 0, 0, 0.04f)));
            dc->FillRoundedRectangle(D2D1::RoundedRect(tab, 5.0f * k, 5.0f * k), b);
        }
        b->SetColor(on ? theme.text : WithAlpha(theme.text, 0.7f));
        if (layout) {
            layout->SetMaxWidth(w - 22.0f * k);  // a hair over the measured width: no false ellipsis
            dc->PushAxisAlignedClip(tab, D2D1_ANTIALIAS_MODE_ALIASED);
            dc->DrawTextLayout(D2D1::Point2F(tab.left + 12.0f * k, (tab.top + tab.bottom) * 0.5f - 12.0f * k), layout.Get(), b);
            dc->PopAxisAlignedClip();
        }
        x += w + 2.0f * k;
    }
    dc->PopAxisAlignedClip();
    return (std::min)(x, limit);
}

// ---------------------------------------------------------------- input

bool TableView::Scroll(float wheel_steps, bool horizontal) {
    Sheet* s = Current();
    if (!s) return false;
    const float before_x = s->sx, before_y = s->sy;
    if (horizontal) s->sx -= wheel_steps * 80.0f * scale_;
    else s->sy -= wheel_steps * 26.0f * 3.0f * scale_;
    Clamp();
    if (s->sx != before_x || s->sy != before_y) {
        tip_row_ = tip_col_ = -1;
        tip_shown_ = false;
        return true;
    }
    return false;
}

void TableView::EnsureVisible(int row, int col) {
    Sheet* s = Current();
    if (!s || row < 0 || col < 0 || static_cast<size_t>(col) >= s->cols) return;
    const Geometry g = Measure();
    const float top = static_cast<float>(row) * g.row_h;
    const float view_h = g.body.bottom - g.body.top - (spreadsheet_ ? 0.0f : 40.0f * scale_);
    if (top < s->sy) s->sy = top;
    else if (top + g.row_h > s->sy + view_h) s->sy = top + g.row_h - view_h;
    const float left = s->col_x[static_cast<size_t>(col)], right = s->col_x[static_cast<size_t>(col) + 1];
    const float view_w = g.body.right - g.body.left;
    if (left < s->sx) s->sx = left;
    else if (right > s->sx + view_w) s->sx = (std::min)(left, right - view_w);
    Clamp();
}

bool TableView::Key(UINT vk, bool shift, bool ctrl) {
    Sheet* s = Current();
    if (!s) return false;
    if (spreadsheet_ && ctrl && (vk == VK_PRIOR || vk == VK_NEXT)) {
        if (vk == VK_PRIOR && sheet_ > 0) SwitchSheet(sheet_ - 1);
        if (vk == VK_NEXT && sheet_ + 1 < sheets_.size()) SwitchSheet(sheet_ + 1);
        return true;
    }
    const Geometry g = Measure();
    const float page = (std::max)(g.row_h, g.body.bottom - g.body.top - g.row_h);
    const int rows = static_cast<int>(BodyRows()), cols = static_cast<int>(s->cols);
    if (sel_valid_ && rows > 0 && cols > 0 &&
        (vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT ||
         vk == VK_PRIOR || vk == VK_NEXT || vk == VK_HOME || vk == VK_END)) {
        int r = sel_r1_, c = sel_c1_;
        const int page_rows = (std::max)(1, static_cast<int>(page / g.row_h));
        switch (vk) {
        case VK_UP: r = ctrl ? 0 : r - 1; break;
        case VK_DOWN: r = ctrl ? rows - 1 : r + 1; break;
        case VK_LEFT: c = ctrl ? 0 : c - 1; break;
        case VK_RIGHT: c = ctrl ? cols - 1 : c + 1; break;
        case VK_PRIOR: r -= page_rows; break;
        case VK_NEXT: r += page_rows; break;
        case VK_HOME: c = 0; if (ctrl) r = 0; break;
        case VK_END: c = cols - 1; if (ctrl) r = rows - 1; break;
        default: break;
        }
        sel_r1_ = std::clamp(r, 0, rows - 1);
        sel_c1_ = std::clamp(c, 0, cols - 1);
        if (!shift) { sel_r0_ = sel_r1_; sel_c0_ = sel_c1_; }
        EnsureVisible(sel_r1_, sel_c1_);
        tip_row_ = tip_col_ = -1;
        tip_shown_ = false;
        return true;
    }
    switch (vk) {
    case VK_UP: s->sy -= g.row_h; break;
    case VK_DOWN: s->sy += g.row_h; break;
    case VK_PRIOR: s->sy -= page; break;
    case VK_NEXT: s->sy += page; break;
    case VK_HOME: s->sy = 0.0f; if (ctrl) s->sx = 0.0f; break;
    case VK_END: s->sy = g.max_sy; break;
    default: return false;  // Left/Right step through files
    }
    Clamp();
    return true;
}

bool TableView::OffsetToCell(uint32_t offset, size_t& row, size_t& col, uint32_t& in_cell) const {
    const Sheet* s = Current();
    if (!s || row_offsets_.empty()) return false;
    auto it = std::upper_bound(row_offsets_.begin(), row_offsets_.end(), offset);
    if (it == row_offsets_.begin()) return false;
    row = static_cast<size_t>(it - row_offsets_.begin()) - 1;
    uint32_t at = row_offsets_[row];
    const auto& cells = s->cells[row];
    for (size_t c = 0; c < cells.size(); ++c) {
        const uint32_t end = at + static_cast<uint32_t>(cells[c].size());
        if (offset <= end) {
            col = c;
            in_cell = offset - at;
            return true;
        }
        at = end + 1;
    }
    return false;
}

void TableView::Reveal(uint32_t offset) {
    size_t row = 0, col = 0;
    uint32_t in_cell = 0;
    if (!OffsetToCell(offset, row, col, in_cell)) return;
    const size_t hdr = HeaderRows();
    if (row < hdr) {
        EnsureVisible(0, static_cast<int>(col));
        return;
    }
    EnsureVisible(static_cast<int>(row - hdr), static_cast<int>(col));
}

bool TableView::CellAt(float x, float y, int& row, int& col, bool clamp) const {
    const Sheet* s = Current();
    if (!s || s->cols == 0 || s->col_x.size() != s->cols + 1) return false;
    const Geometry g = Measure();
    const size_t rows = BodyRows();
    if (!rows) return false;
    if (!clamp && (x < g.body.left || x >= g.body.right || y < g.body.top || y >= g.body.bottom)) return false;
    const float doc_x = x - g.body.left + s->sx;
    const float doc_y = y - g.body.top + s->sy;
    int r = static_cast<int>(std::floor(doc_y / g.row_h));
    if (!clamp && (r < 0 || static_cast<size_t>(r) >= rows)) return false;
    r = std::clamp(r, 0, static_cast<int>(rows) - 1);
    auto it = std::upper_bound(s->col_x.begin(), s->col_x.end(), doc_x);
    int c = static_cast<int>(it - s->col_x.begin()) - 1;
    if (!clamp && (c < 0 || static_cast<size_t>(c) >= s->cols)) return false;
    c = std::clamp(c, 0, static_cast<int>(s->cols) - 1);
    row = r;
    col = c;
    return true;
}

void TableView::SwitchSheet(size_t index) {
    if (index >= sheets_.size() || index == sheet_) return;
    sheet_ = index;
    const auto& detail = sheets_[sheet_].detail;
    awaiting_sheet_ = lazy_sheets_ && (detail == L"not-loaded" || detail == L"read-failed" ||
        detail == L"time-limit" || detail == L"payload-limit");
    pending_sheet_request_ = awaiting_sheet_;
    sel_valid_ = false;
    tip_row_ = tip_col_ = -1;
    tip_shown_ = false;
    BuildPlain();
}

bool TableView::TakePendingSheetRequest(uint32_t& index) {
    if (!pending_sheet_request_) return false;
    index = static_cast<uint32_t>(sheet_);
    pending_sheet_request_ = false;
    return true;
}

void TableView::FailPendingSheetRequest(uint32_t index) {
    if (index != sheet_ || !awaiting_sheet_) return;
    awaiting_sheet_ = pending_sheet_request_ = false;
    sheets_[sheet_].detail = L"read-failed";
    sheets_[sheet_].truncated = true;
}

bool TableView::TabClick(float x, float y) {
    if (!spreadsheet_) return false;
    const auto inside = [&](const D2D1_RECT_F& r) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; };
    if (inside(previous_sheet_)) { if (sheet_ > 0) SwitchSheet(sheet_ - 1); return true; }
    if (inside(next_sheet_)) { if (sheet_ + 1 < sheets_.size()) SwitchSheet(sheet_ + 1); return true; }
    for (size_t i = 0; i < tab_rects_.size(); ++i)
        if (inside(tab_rects_[i])) { SwitchSheet(first_visible_sheet_ + i); return true; }
    return false;
}

bool TableView::MouseDown(float x, float y, bool shift) {
    Sheet* s = Current();
    if (!s) return false;
    const auto inside = [&](const D2D1_RECT_F& r, float pad) {
        return x >= r.left - pad && x < r.right + pad && y >= r.top - pad && y < r.bottom + pad;
    };
    tip_row_ = tip_col_ = -1;
    tip_shown_ = false;
    if (spreadsheet_ && inside(previous_sheet_, 0.0f)) {
        if (sheet_ > 0) SwitchSheet(sheet_ - 1);
        return true;
    }
    if (spreadsheet_ && inside(next_sheet_, 0.0f)) {
        if (sheet_ + 1 < sheets_.size()) SwitchSheet(sheet_ + 1);
        return true;
    }
    for (size_t i = 0; i < tab_rects_.size(); ++i)
        if (inside(tab_rects_[i], 0.0f)) { SwitchSheet(first_visible_sheet_ + i); return true; }
    const Geometry g = Measure();
    if (spreadsheet_ && !footer_tabs_ && y >= g.tabs.top) return true;
    if (vthumb_.bottom > vthumb_.top && inside(vthumb_, 4.0f * scale_)) {
        drag_ = Drag::VThumb; drag_origin_ = y; drag_scroll_ = s->sy; return true;
    }
    if (hthumb_.right > hthumb_.left && inside(hthumb_, 4.0f * scale_)) {
        drag_ = Drag::HThumb; drag_origin_ = x; drag_scroll_ = s->sx; return true;
    }
    const int rows = static_cast<int>(BodyRows()), cols = static_cast<int>(s->cols);
    if (rows <= 0 || cols <= 0) return true;
    // Corner selects all; header selects a column; row numbers a row.
    const bool in_head = y >= g.grid.top && y < g.body.top;
    const bool in_numbers = x >= g.grid.left && x < g.body.left;
    if (in_head && in_numbers) { SelectAll(); return true; }
    int r = 0, c = 0;
    if (in_head) {
        if (!CellAt(x, g.body.top, r, c, true)) return true;
        if (shift && sel_valid_) { sel_c1_ = c; } else { sel_c0_ = sel_c1_ = c; }
        sel_r0_ = 0; sel_r1_ = rows - 1;
        sel_valid_ = true;
        return true;
    }
    if (in_numbers) {
        if (!CellAt(g.body.left, y, r, c, true)) return true;
        if (shift && sel_valid_) { sel_r1_ = r; } else { sel_r0_ = sel_r1_ = r; }
        sel_c0_ = 0; sel_c1_ = cols - 1;
        sel_valid_ = true;
        return true;
    }
    if (!CellAt(x, y, r, c, false)) {
        ClearSelection();
        return true;
    }
    if (shift && sel_valid_) { sel_r1_ = r; sel_c1_ = c; }
    else { sel_r0_ = sel_r1_ = r; sel_c0_ = sel_c1_ = c; }
    sel_valid_ = true;
    drag_ = Drag::Cells;
    drag_x_ = x; drag_y_ = y;
    return true;
}

bool TableView::MouseMove(float x, float y) {
    Sheet* s = Current();
    if (!s || drag_ == Drag::None) return false;
    const Geometry g = Measure();
    if (drag_ == Drag::VThumb) {
        const float track = g.body.bottom - g.body.top - 8.0f * scale_ - (vthumb_.bottom - vthumb_.top);
        if (track > 0.0f) s->sy = drag_scroll_ + (y - drag_origin_) / track * g.max_sy;
        Clamp();
        return true;
    }
    if (drag_ == Drag::HThumb) {
        const float track = g.body.right - g.body.left - 8.0f * scale_ - (hthumb_.right - hthumb_.left);
        if (track > 0.0f) s->sx = drag_scroll_ + (x - drag_origin_) / track * g.max_sx;
        Clamp();
        return true;
    }
    // Range drag; past the edges the grid scrolls.
    if (y > g.body.bottom) s->sy += (std::min)(y - g.body.bottom, 60.0f * scale_);
    else if (y < g.body.top) s->sy -= (std::min)(g.body.top - y, 60.0f * scale_);
    if (x > g.body.right) s->sx += (std::min)(x - g.body.right, 60.0f * scale_);
    else if (x < g.body.left) s->sx -= (std::min)(g.body.left - x, 60.0f * scale_);
    Clamp();
    int r = 0, c = 0;
    if (!CellAt(x, y, r, c, true)) return false;
    const bool changed = r != sel_r1_ || c != sel_c1_;
    sel_r1_ = r;
    sel_c1_ = c;
    return changed || y > g.body.bottom || y < g.body.top || x > g.body.right || x < g.body.left;
}

void TableView::MouseUp() { drag_ = Drag::None; }

bool TableView::Clipped(int row, int col) {
    const Sheet* s = Current();
    if (!s || !factory_) return false;
    const std::wstring& text = s->cells[static_cast<size_t>(row) + HeaderRows()][static_cast<size_t>(col)];
    if (text.empty()) return false;
    const float width = s->col_x[static_cast<size_t>(col) + 1] - s->col_x[static_cast<size_t>(col)] - 20.0f * scale_;
    if (Estimate(text) < width * 0.8f) return false;
    WrlPtr<IDWriteTextLayout> layout;
    IDWriteTextFormat* f = (s->flags[static_cast<size_t>(row) + HeaderRows()][static_cast<size_t>(col)] & kBold) ? bold_.Get() : text_.Get();
    if (FAILED(factory_->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), f, 100000.0f,
                                          100.0f, &layout)) || !layout)
        return false;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return m.width > width + 0.5f;
}

bool TableView::Hover(float x, float y) {
    bool repaint = false;
    int tab = -1;
    for (size_t i = 0; i < tab_rects_.size(); ++i) {
        const auto& r = tab_rects_[i];
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) tab = static_cast<int>(i);
    }
    if (tab != hover_tab_) { hover_tab_ = tab; repaint = true; }
    {
        int hr = -1, hc = -1;
        if (drag_ != Drag::None || !CellAt(x, y, hr, hc, false)) hr = hc = -1;
        if (hr != hover_row_ || hc != hover_col_) { hover_row_ = hr; hover_col_ = hc; repaint = true; }
    }
    int r = -1, c = -1;
    if (drag_ != Drag::None || !CellAt(x, y, r, c, false) || !Clipped(r, c)) r = c = -1;
    if (r != tip_row_ || c != tip_col_) {
        if (tip_shown_) repaint = true;
        tip_row_ = r;
        tip_col_ = c;
        tip_shown_ = false;
        tip_x_ = x;
        tip_y_ = y;
    }
    return repaint;
}

bool TableView::Leave() {
    const bool repaint = hover_tab_ >= 0 || tip_shown_ || hover_row_ >= 0;
    hover_tab_ = tip_row_ = tip_col_ = hover_row_ = hover_col_ = -1;
    tip_shown_ = false;
    return repaint;
}

bool TableView::ShowTip() {
    if (tip_row_ < 0 || tip_shown_) return false;
    tip_shown_ = true;
    return true;
}

void TableView::SelectAll() {
    const Sheet* s = Current();
    if (!s || !BodyRows() || !s->cols) return;
    sel_r0_ = 0; sel_c0_ = 0;
    sel_r1_ = static_cast<int>(BodyRows()) - 1;
    sel_c1_ = static_cast<int>(s->cols) - 1;
    sel_valid_ = true;
}

bool TableView::ClearSelection() {
    const bool had = sel_valid_;
    sel_valid_ = false;
    return had;
}

std::wstring TableView::SelectionText() const {
    const Sheet* s = Current();
    if (!s) return {};
    const size_t hdr = HeaderRows();
    size_t r0 = 0, r1 = s->cells.size(), c0 = 0, c1 = s->cols;  // whole sheet incl. header
    if (sel_valid_) {
        r0 = static_cast<size_t>((std::min)(sel_r0_, sel_r1_)) + hdr;
        r1 = static_cast<size_t>((std::max)(sel_r0_, sel_r1_)) + hdr + 1;
        c0 = static_cast<size_t>((std::min)(sel_c0_, sel_c1_));
        c1 = static_cast<size_t>((std::max)(sel_c0_, sel_c1_)) + 1;
        // A whole-column selection brings its header name along.
        if (hdr && r0 == hdr && r1 == s->cells.size()) r0 = 0;
    }
    std::wstring out;
    for (size_t r = r0; r < r1 && r < s->cells.size(); ++r) {
        for (size_t c = c0; c < c1 && c < s->cols; ++c) {
            if (c > c0) out += L'\t';
            out += s->cells[r][c];
        }
        if (r1 - r0 > 1 || c1 - c0 > 1) out += L"\r\n";
    }
    return out;
}

}  // namespace pulse::ui

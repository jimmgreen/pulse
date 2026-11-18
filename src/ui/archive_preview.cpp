// archive_preview.cpp — see archive_preview.h.
#include "archive_preview.h"

#include "ui_compositor.h"
#include "ui_renderer_internal.h"
#include "typography.h"
#include "../common/localization.h"
#include "../common/text_format.h"

#include <algorithm>
#include <cmath>
#include <cwctype>

namespace pulse::ui {
namespace {

using pulse::l10n::StringId;

constexpr uint32_t kArchiveAmber = 0xCA8A04;
constexpr uint32_t kFolderGold = 0xE0A526;

std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

// Type families for the composition bar; Word/Excel/PowerPoint fold into one.
struct FamilyDef { uint32_t rgb; StringId label; };
constexpr FamilyDef kFamilies[] = {
    {0xD97706, StringId::ArcFamCode},    {0xA855F7, StringId::ArcFamImage},
    {0x2563EB, StringId::ArcFamDoc},     {0xDC2626, StringId::ArcFamDoc},
    {0x0891B2, StringId::ArcFamCad},     {0x6366F1, StringId::ArcFam3d},
    {0xDB2777, StringId::ArcFamVideo},   {0x0D9488, StringId::ArcFamAudio},
    {0xCA8A04, StringId::ArcFamArchive}, {0x65A30D, StringId::ArcFamProgram},
    {0x3B82F6, StringId::ArcFamProgram}, {0x64748B, StringId::ArcFamOther},
};

uint32_t FamilyColor(uint32_t rgb) {
    if (rgb == 0x16A34A || rgb == 0xEA580C) return 0x2563EB;  // Office -> documents
    if (rgb == 0x3B82F6) return 0x65A30D;                     // system -> programs
    return rgb;
}

StringId FamilyLabel(uint32_t family_rgb) {
    for (const auto& f : kFamilies) if (f.rgb == family_rgb) return f.label;
    return StringId::ArcFamOther;
}

std::wstring Fmt(StringId id, int a) {
    wchar_t buf[160]{};
    swprintf_s(buf, pulse::l10n::Get(id).c_str(), a);
    return buf;
}

std::wstring Fmt2(StringId id, int a, int b) {
    wchar_t buf[160]{};
    swprintf_s(buf, pulse::l10n::Get(id).c_str(), a, b);
    return buf;
}

bool IsDarkTheme(const Theme& theme) {
    const auto& c = theme.text;
    return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b > 0.5f;
}

D2D1_RECT_F R(float l, float t, float r, float b) { return D2D1::RectF(l, t, r, b); }

} // namespace

// ---------------------------------------------------------------- model

void ArchivePreview::Clear() {
    payload_identity_.reset();
    parsed_ = false;
    decltype(payload_){}.swap(payload_);
    shared_payload_.reset();
    decltype(nodes_){}.swap(nodes_);
    decltype(roots_){}.swap(roots_);
    decltype(visible_){}.swap(visible_);
    mix_.clear();
    biggest_.clear();
    filter_.clear();
    file_count_ = dir_count_ = 0;
    unpacked_ = 0;
    any_encrypted_ = false;
    folder_ = counting_ = false;
    scroll_ = 0.0f;
    hover_ = selected_ = -1;
}

bool ArchivePreview::SetPayload(const std::shared_ptr<const std::wstring>& payload, const std::wstring& file_name, uint64_t file_size) {
    if (payload && payload_identity_.lock() == payload && parsed_ && file_name == file_name_) {
        file_size_ = file_size;
        return true;
    }
    if (!payload) return false;
    const bool accepted = SetPayloadImpl(*payload, file_name, file_size, payload);
    if (accepted) {
        StorePayload(*payload, payload);
        payload_identity_ = payload;
    }
    return accepted;
}

bool ArchivePreview::SetPayload(const std::wstring& payload, const std::wstring& file_name, uint64_t file_size) {
    return SetPayloadImpl(payload, file_name, file_size, {});
}

bool ArchivePreview::SetPayloadImpl(const std::wstring& payload, const std::wstring& file_name, uint64_t file_size,
                           std::shared_ptr<const std::wstring> owner) {
    payload_identity_.reset();
    if (parsed_ && payload == Payload() && file_name == file_name_) {
        file_size_ = file_size;
        return true;
    }
    Clear();
    if (payload.rfind(L"PULSEARC\t1\t", 0) != 0) return false;
    StorePayload(payload, std::move(owner));
    file_name_ = file_name;
    file_size_ = file_size;

    std::vector<std::wstring_view> fields;
    auto split = [&](std::wstring_view line) {
        fields.clear();
        size_t start = 0;
        for (int i = 0; i < 5; ++i) {
            const size_t tab = line.find(L'\t', start);
            if (tab == std::wstring_view::npos) break;
            fields.push_back(line.substr(start, tab - start));
            start = tab + 1;
        }
        fields.push_back(line.substr(start));
    };
    auto to_u64 = [](std::wstring_view v) -> uint64_t {
        uint64_t n = 0;
        for (wchar_t c : v) { if (c < L'0' || c > L'9') break; n = n * 10 + (c - L'0'); }
        return n;
    };

    std::wstring_view all(Payload());
    size_t pos = 0;
    bool header = true;
    // Folder listings: whole-folder totals (#S row), as rows may be left out.
    bool summary = false;
    uint64_t summary_files = 0, summary_dirs = 0, summary_total = 0;
    std::vector<std::pair<std::wstring, uint64_t>> summary_types;
    std::vector<int> stack;
    while (pos < all.size()) {
        size_t end = all.find(L'\n', pos);
        if (end == std::wstring_view::npos) end = all.size();
        const std::wstring_view line = all.substr(pos, end - pos);
        pos = end + 1;
        if (line.empty()) continue;
        split(line);
        if (header) {
            // PULSEARC 1 fmt packed incomplete
            header = false;
            if (fields.size() < 5) return false;
            format_.assign(fields[2]);
            has_packed_ = fields[3] != L"-";
            packed_total_ = to_u64(fields[3]);
            incomplete_ = fields[4] == L"1";
            counting_ = fields[4] == L"2";
            folder_ = format_ == L"DIR";
            continue;
        }
        if (fields.size() >= 6 && fields[0] == L"#S") {
            summary = true;
            summary_files = to_u64(fields[1]);
            summary_dirs = to_u64(fields[2]);
            summary_total = to_u64(fields[3]);
            std::wstring_view types = fields[5];
            while (!types.empty()) {
                const size_t bar = types.find(L'|');
                const std::wstring_view item = types.substr(0, bar);
                types = bar == std::wstring_view::npos ? std::wstring_view() : types.substr(bar + 1);
                const size_t colon = item.find(L':');
                if (colon == std::wstring_view::npos) continue;
                summary_types.push_back({std::wstring(item.substr(0, colon)), to_u64(item.substr(colon + 1))});
            }
            continue;
        }
        if (fields.size() < 6) continue;
        Node node;
        node.depth = static_cast<int>(to_u64(fields[0]));
        node.dir = fields[1].find(L'd') != std::wstring_view::npos;
        node.encrypted = fields[1].find(L'e') != std::wstring_view::npos;
        node.size = to_u64(fields[2]);
        node.packed = to_u64(fields[3]);
        if (fields[4] != L"-") node.date.assign(fields[4]);
        node.name.assign(fields[5]);
        if (!node.dir) {
            const size_t dot = node.name.find_last_of(L'.');
            if (dot != std::wstring::npos && dot > 0 && node.name.size() - dot - 1 <= 5) {
                std::wstring ext = node.name.substr(dot + 1);
                node.rgb = TypeChipRgb(ext);
                for (wchar_t& c : ext) c = static_cast<wchar_t>(std::towupper(c));
                node.chip = ext.substr(0, 4);
            }
            if (node.chip.empty()) node.chip = L"·";
        }
        if (node.depth > static_cast<int>(stack.size())) node.depth = static_cast<int>(stack.size());
        stack.resize(node.depth);
        const int index = static_cast<int>(nodes_.size());
        node.parent = stack.empty() ? -1 : stack.back();
        nodes_.push_back(std::move(node));
        if (nodes_[index].parent < 0) roots_.push_back(index);
        else nodes_[nodes_[index].parent].kids.push_back(index);
        if (nodes_[index].dir) stack.push_back(index);
    }
    if (header) return false;

    // Aggregates: nodes arrive in pre-order, so walking backwards finishes
    // every child before its parent.
    std::vector<std::pair<uint64_t, uint32_t>> fam;
    for (int i = static_cast<int>(nodes_.size()) - 1; i >= 0; --i) {
        Node& n = nodes_[i];
        if (!n.dir) {
            ++file_count_;
            n.files = 1;
            const uint32_t f = FamilyColor(n.rgb);
            auto it = std::find_if(fam.begin(), fam.end(), [&](auto& p) { return p.second == f; });
            if (it == fam.end()) fam.push_back({n.size, f}); else it->first += n.size;
        } else {
            ++dir_count_;
        }
        any_encrypted_ = any_encrypted_ || n.encrypted;
        if (n.parent >= 0 && !n.dir) {
            for (int p = n.parent; p >= 0; p = nodes_[p].parent) ++nodes_[p].files;
        }
    }
    for (int r : roots_) unpacked_ += nodes_[r].size;
    std::sort(fam.begin(), fam.end(), [](auto& a, auto& b) { return a.first > b.first; });
    if (summary) {
        file_count_ = static_cast<uint32_t>((std::min<uint64_t>)(summary_files, UINT32_MAX));
        dir_count_ = static_cast<uint32_t>((std::min<uint64_t>)(summary_dirs, UINT32_MAX));
        unpacked_ = summary_total;
        fam.clear();
        for (const auto& [ext, bytes] : summary_types) {
            const uint32_t f = FamilyColor(ext.empty() ? 0x64748B : TypeChipRgb(ext));
            auto it = std::find_if(fam.begin(), fam.end(), [&](auto& p) { return p.second == f; });
            if (it == fam.end()) fam.push_back({bytes, f}); else it->first += bytes;
        }
        std::sort(fam.begin(), fam.end(), [](auto& a, auto& b) { return a.first > b.first; });
    }
    for (auto& [bytes, rgb] : fam)
        mix_.push_back({rgb, bytes, static_cast<int>(FamilyLabel(rgb))});

    std::vector<int> files;
    for (int i = 0; i < static_cast<int>(nodes_.size()); ++i) if (!nodes_[i].dir) files.push_back(i);
    const size_t top = (std::min<size_t>)(5, files.size());
    std::partial_sort(files.begin(), files.begin() + top, files.end(),
                      [&](int a, int b) { return nodes_[a].size > nodes_[b].size; });
    biggest_.assign(files.begin(), files.begin() + top);

    // Open the top level; a lone wrapper folder opens one level further.
    // A folder listing starts with its top level collapsed, like the mockup.
    if (!folder_) {
        for (int r : roots_) nodes_[r].open = nodes_[r].dir;
        if (roots_.size() == 1 && nodes_[roots_[0]].dir)
            for (int k : nodes_[roots_[0]].kids) if (nodes_[k].dir && nodes_[k].kids.size() <= 24) nodes_[k].open = true;
    }

    parsed_ = true;
    RebuildVisible();
    return true;
}

std::wstring ArchivePreview::SelectedPath() const {
    if (selected_ < 0 || selected_ >= static_cast<int>(visible_.size())) return {};
    std::wstring path;
    for (int i = visible_[selected_]; i >= 0; i = nodes_[i].parent)
        path = path.empty() ? nodes_[i].name : nodes_[i].name + L"\\" + path;
    return path;
}

std::wstring ArchivePreview::StateNote() const {
    if (!filter_.empty()) {
        if (format_ == L"ISO/UDF")
            return pulse::l10n::Get(pulse::l10n::StringId::SearchLoadedISOEntriesOnlyUDFMayBeMissing).c_str();
        if (incomplete_ || counting_)
            return pulse::l10n::Get(pulse::l10n::StringId::PartialListingSearchCoversLoadedEntriesOnly).c_str();
        return pulse::l10n::Get(pulse::l10n::StringId::SearchCoversLoadedEntriesOnly).c_str();
    }
    if (format_ == L"ISO/UDF")
        return pulse::l10n::Get(pulse::l10n::StringId::ISOCompatibilityDirectoryOnlyUDFMayBeMissing).c_str();
    if (counting_) return pulse::l10n::Get(pulse::l10n::StringId::Counting).c_str();
    if (!incomplete_) return {};
    if (folder_)
        return pulse::l10n::Get(pulse::l10n::StringId::ScanLimitReachedTotalsAreMinimums).c_str();
    return pulse::l10n::Get(StringId::ArcIncomplete);
}

void ArchivePreview::AppendVisible(int index) {
    const Node& n = nodes_[index];
    if (!filter_.empty() && !n.match) return;
    visible_.push_back(index);
    const bool expanded = filter_.empty() ? n.open : true;
    if (n.dir && expanded)
        for (int k : n.kids) AppendVisible(k);
}

void ArchivePreview::RebuildVisible() {
    const int keep = (selected_ >= 0 && selected_ < static_cast<int>(visible_.size()))
        ? visible_[selected_] : -1;
    visible_.clear();
    for (int r : roots_) AppendVisible(r);
    selected_ = -1;
    if (keep >= 0) {
        const auto it = std::find(visible_.begin(), visible_.end(), keep);
        if (it != visible_.end()) selected_ = static_cast<int>(it - visible_.begin());
    }
    hover_ = -1;
    ClampScroll();
}

bool ArchivePreview::SetFilter(const std::wstring& query) {
    const std::wstring q = Lower(query);
    if (q == filter_) return false;
    filter_ = q;
    filter_hits_ = 0;
    for (auto& n : nodes_) { n.match = filter_.empty(); n.self_match = false; }
    if (!filter_.empty()) {
        for (int i = static_cast<int>(nodes_.size()) - 1; i >= 0; --i) {
            Node& n = nodes_[i];
            if (Lower(n.name).find(filter_) != std::wstring::npos) {
                n.self_match = true;
                n.match = true;
                if (!n.dir) ++filter_hits_;
            }
            if (n.match && n.parent >= 0) nodes_[n.parent].match = true;
        }
    }
    scroll_ = 0.0f;
    selected_ = -1;
    RebuildVisible();
    return true;
}

// ---------------------------------------------------------------- input

float ArchivePreview::RowHeight() const noexcept {
    return (large_ ? 30.0f : 26.0f) * scale_;
}

void ArchivePreview::ClampScroll() {
    const float view = tree_rect_.bottom - tree_rect_.top;
    const float content = RowHeight() * static_cast<float>(visible_.size());
    const float max_scroll = (std::max)(0.0f, content - view);
    scroll_ = (std::clamp)(scroll_, 0.0f, max_scroll);
}

bool ArchivePreview::ScrollPixels(float dy) {
    const float before = scroll_;
    scroll_ += dy;
    ClampScroll();
    return scroll_ != before;
}

bool ArchivePreview::Scroll(float wheel_steps) {
    return ScrollPixels(-wheel_steps * RowHeight() * 3.0f);
}

bool ArchivePreview::Contains(float x, float y) const noexcept {
    return x >= view_rect_.left && x < view_rect_.right && y >= view_rect_.top &&
           y < view_rect_.bottom;
}

int ArchivePreview::RowAt(float x, float y) const {
    if (x < tree_rect_.left || x >= tree_rect_.right || y < tree_rect_.top ||
        y >= tree_rect_.bottom)
        return -1;
    const int row = static_cast<int>((y - tree_rect_.top + scroll_) / RowHeight());
    return row >= 0 && row < static_cast<int>(visible_.size()) ? row : -1;
}

bool ArchivePreview::SelectedIsDirectory() const {
    return selected_ >= 0 && selected_ < static_cast<int>(visible_.size()) && nodes_[visible_[selected_]].dir;
}

bool ArchivePreview::SelectAt(float x, float y) {
    selected_ = RowAt(x, y);
    if (selected_ < 0) return false;
    RevealSelection();
    return true;
}

bool ArchivePreview::Click(float x, float y) {
    const int row = RowAt(x, y);
    if (row < 0) return false;
    selected_ = row;
    Node& n = nodes_[visible_[row]];
    if (n.dir && filter_.empty()) {
        n.open = !n.open;
        RebuildVisible();
    }
    return true;
}

bool ArchivePreview::Hover(float x, float y) {
    const int row = RowAt(x, y);
    if (row == hover_) return false;
    hover_ = row;
    return true;
}

bool ArchivePreview::Leave() {
    if (hover_ < 0) return false;
    hover_ = -1;
    return true;
}

void ArchivePreview::RevealSelection() {
    if (selected_ < 0) return;
    const float h = RowHeight();
    const float top = selected_ * h;
    const float view = tree_rect_.bottom - tree_rect_.top;
    if (top < scroll_) scroll_ = top;
    else if (top + h > scroll_ + view) scroll_ = top + h - view;
    ClampScroll();
}

bool ArchivePreview::Key(UINT vk) {
    if (visible_.empty()) return false;
    const int last = static_cast<int>(visible_.size()) - 1;
    const int page = (std::max)(1, static_cast<int>((tree_rect_.bottom - tree_rect_.top) / RowHeight()) - 1);
    switch (vk) {
    case VK_DOWN: selected_ = selected_ < 0 ? 0 : (std::min)(last, selected_ + 1); break;
    case VK_UP: selected_ = selected_ < 0 ? 0 : (std::max)(0, selected_ - 1); break;
    case VK_NEXT: selected_ = (std::min)(last, (std::max)(0, selected_) + page); break;
    case VK_PRIOR: selected_ = (std::max)(0, selected_ - page); break;
    case VK_HOME: selected_ = 0; break;
    case VK_END: selected_ = last; break;
    case VK_RIGHT:
    case VK_LEFT: {
        if (selected_ < 0) { selected_ = 0; break; }
        Node& n = nodes_[visible_[selected_]];
        if (!filter_.empty()) return false;
        if (vk == VK_RIGHT) {
            if (!n.dir) return false;
            if (!n.open) { n.open = true; RebuildVisible(); }
            else if (!n.kids.empty()) ++selected_;
        } else if (n.dir && n.open) {
            n.open = false;
            RebuildVisible();
        } else if (n.parent >= 0) {
            const auto it = std::find(visible_.begin(), visible_.end(), n.parent);
            if (it != visible_.end()) selected_ = static_cast<int>(it - visible_.begin());
        } else {
            return false;
        }
        break;
    }
    case VK_RETURN: {
        if (selected_ < 0) return false;
        Node& n = nodes_[visible_[selected_]];
        if (!n.dir || !filter_.empty()) return false;
        n.open = !n.open;
        RebuildVisible();
        break;
    }
    default: return false;
    }
    RevealSelection();
    return true;
}

// ---------------------------------------------------------------- drawing helpers

void ArchivePreview::EnsureResources(ID2D1DeviceContext* dc, Compositor* compositor, float scale) {
    if (dc != brush_owner_ || !brush_) {
        brush_.Reset();
        dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &brush_);
        brush_owner_ = dc;
    }
    compositor_ = compositor;
    IDWriteFactory2* factory = compositor ? compositor->DwriteFactory() : nullptr;
    if (factory == factory_ && scale == format_scale_ && body_) return;
    factory_ = factory;
    format_scale_ = scale;
    body_.Reset(); small_.Reset(); chip_.Reset(); value_.Reset(); title_.Reset(); icon_.Reset();
    if (!factory) return;
    using typography::FontRole;
    auto make = [&](FontRole role, float size, DWRITE_FONT_WEIGHT weight,
                    Microsoft::WRL::ComPtr<IDWriteTextFormat>& out) {
        typography::TextFormatSpec spec;
        spec.role = role;
        spec.size = size * scale;
        spec.weight = weight;
        if (FAILED(typography::CreateTextFormat(factory, spec, &out)) || !out) return;
        out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        Microsoft::WRL::ComPtr<IDWriteInlineObject> sign;
        if (SUCCEEDED(factory->CreateEllipsisTrimmingSign(out.Get(), &sign))) {
            DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            out->SetTrimming(&trimming, sign.Get());
        }
    };
    make(FontRole::Text, 13.0f, DWRITE_FONT_WEIGHT_NORMAL, body_);
    make(FontRole::Text, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, small_);
    make(FontRole::Text, 8.5f, DWRITE_FONT_WEIGHT_BOLD, chip_);
    make(FontRole::Text, 17.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, value_);
    make(FontRole::Text, 14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, title_);
    make(FontRole::Icon, 13.0f, DWRITE_FONT_WEIGHT_NORMAL, icon_);
}

void ArchivePreview::DrawText(ID2D1DeviceContext* dc, IDWriteTextFormat* format,
                              const std::wstring& text, const D2D1_RECT_F& rect,
                              const D2D1_COLOR_F& color, DWRITE_TEXT_ALIGNMENT align) {
    if (!format || text.empty() || rect.right <= rect.left) return;
    brush_->SetColor(color);
    // The same LumaText path as the rest of the window; the layout inherits
    // the format's trimming and paragraph alignment.
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if (compositor_ && factory_ &&
        SUCCEEDED(factory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format,
                                             rect.right - rect.left, (std::max)(1.0f, rect.bottom - rect.top),
                                             &layout)) && layout) {
        layout->SetTextAlignment(align);
        compositor_->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), layout.Get(), brush_.Get(),
                                    D2D1_DRAW_TEXT_OPTIONS_CLIP);
        return;
    }
    format->SetTextAlignment(align);
    dc->DrawText(text.c_str(), static_cast<UINT32>(text.size()), format, rect, brush_.Get(),
                 D2D1_DRAW_TEXT_OPTIONS_CLIP);
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
}

void ArchivePreview::Fill(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, float radius,
                          const D2D1_COLOR_F& color) {
    if (rect.right <= rect.left || rect.bottom <= rect.top) return;
    brush_->SetColor(color);
    if (radius > 0.0f) dc->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush_.Get());
    else dc->FillRectangle(rect, brush_.Get());
}

float ArchivePreview::Measure(IDWriteTextFormat* format, const std::wstring& text) const {
    if (!format || !factory_ || text.empty()) return 0.0f;
    return typography::MeasureAdvance(factory_, format, text);
}

void ArchivePreview::DrawChip(ID2D1DeviceContext* dc, const Node& node, float x, float cy) {
    const float s = scale_;
    const D2D1_RECT_F chip = R(x, cy - 8.0f * s, x + 28.0f * s, cy + 8.0f * s);
    Fill(dc, chip, 4.0f * s, HexColor(node.rgb, 0.16f));
    DrawText(dc, chip_.Get(), node.chip, chip, HexColor(node.rgb), DWRITE_TEXT_ALIGNMENT_CENTER);
}

// ---------------------------------------------------------------- sections

// Tile + title/subtitle. Returns the height used.
float ArchivePreview::DrawHeader(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect,
                                 const Theme& theme, bool large) {
    const float s = scale_;
    const float tile = (large ? 44.0f : 38.0f) * s;
    const D2D1_RECT_F t = R(rect.left, rect.top, rect.left + tile, rect.top + tile);
    const float tx = t.right + 10.0f * s;
    std::wstring title, sub;
    if (folder_) {
        // Folder tile: the tree's folder glyph, enlarged.
        Fill(dc, t, 9.0f * s, HexColor(kFolderGold, 0.15f));
        const float cx = (t.left + t.right) * 0.5f, cy = (t.top + t.bottom) * 0.5f;
        const float u = tile / 44.0f;
        Fill(dc, R(cx - 12.0f * u, cy - 10.0f * u, cx - 1.0f * u, cy - 4.0f * u), 2.0f * u, HexColor(kFolderGold));
        Fill(dc, R(cx - 12.0f * u, cy - 7.0f * u, cx + 12.0f * u, cy + 9.0f * u), 3.0f * u, HexColor(kFolderGold));
        Fill(dc, R(cx - 12.0f * u, cy - 3.5f * u, cx + 12.0f * u, cy + 9.0f * u), 3.0f * u, HexColor(0xF5C542));
        title = file_name_;
        sub = std::to_wstring(dir_count_) + L" " + pulse::l10n::Get(StringId::ArcFolders) + L" · " +
              std::to_wstring(file_count_) + L" " + pulse::l10n::Get(StringId::ArcFiles) + L" · " +
              (incomplete_ || counting_ ? L"\x2265 " : L"") + pulse::format::ByteSize(unpacked_);
        if (counting_) sub += pulse::l10n::Get(pulse::l10n::StringId::Counting_2).c_str();
    } else {
    Fill(dc, t, 9.0f * s, HexColor(kArchiveAmber, 0.15f));
    DrawText(dc, chip_.Get(), format_, t, HexColor(kArchiveAmber), DWRITE_TEXT_ALIGNMENT_CENTER);
    const bool image = format_ == L"ISO" || format_ == L"ISO/UDF" || format_ == L"UDF";
    const std::wstring kind = image ? pulse::l10n::Get(pulse::l10n::StringId::Image).c_str() : pulse::l10n::Get(StringId::ArcArchive);
    if (large) {
        title = file_name_;
        sub = format_ + L" " + kind + L" · " +
              pulse::format::ByteSize(file_size_);
    } else {
        title = format_ + L" " + kind + L" · " +
                pulse::format::ByteSize(file_size_);
        sub = std::to_wstring(file_count_) + L" " + pulse::l10n::Get(StringId::ArcFiles) + L" · " +
              std::to_wstring(dir_count_) + L" " + pulse::l10n::Get(StringId::ArcFolders) + L" · " +
              (image ? pulse::l10n::Get(pulse::l10n::StringId::LoadedContent).c_str() : pulse::l10n::Get(StringId::ArcUnpacked)) +
              L" " + pulse::format::ByteSize(unpacked_);
    }
    }
    DrawText(dc, title_.Get(), title, R(tx, t.top, rect.right, t.top + tile * 0.55f), theme.text);
    float sx = tx;
    const D2D1_RECT_F subRect = R(sx, t.top + tile * 0.52f, rect.right, t.bottom);
    if (any_encrypted_) {
        // Lock badge ahead of the subtitle.
        const std::wstring label = pulse::l10n::Get(StringId::ArcEncrypted);
        const float w = Measure(small_.Get(), label) + 26.0f * s;
        const float cy = (subRect.top + subRect.bottom) * 0.5f;
        const D2D1_RECT_F badge = R(sx, cy - 9.0f * s, sx + w, cy + 9.0f * s);
        Fill(dc, badge, 9.0f * s, HexColor(0xD97706, 0.14f));
        DrawText(dc, icon_.Get(), L"\uE72E", R(badge.left + 6.0f * s, badge.top, badge.left + 20.0f * s, badge.bottom),
                 HexColor(0xD97706));
        DrawText(dc, small_.Get(), label, R(badge.left + 20.0f * s, badge.top, badge.right, badge.bottom),
                 HexColor(0xB45309));
        sx = badge.right + 8.0f * s;
    }
    DrawText(dc, small_.Get(), sub, R(sx, subRect.top, rect.right, subRect.bottom), theme.text_secondary);
    return tile;
}

// Stacked type bar + legend. Returns the height used.
float ArchivePreview::DrawMix(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, const Theme& theme,
                              size_t legend_items) {
    const float s = scale_;
    const float barH = 6.0f * s;
    const float width = rect.right - rect.left;
    uint64_t total = 0;
    for (const auto& f : mix_) total += f.bytes;
    Fill(dc, R(rect.left, rect.top, rect.right, rect.top + barH), barH * 0.5f, theme.fill_hover);
    if (total > 0) {
        dc->PushAxisAlignedClip(R(rect.left, rect.top, rect.right, rect.top + barH),
                                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        // Rounded ends come from a rounded fill over the whole bar, split in segments.
        float x = rect.left;
        for (size_t i = 0; i < mix_.size(); ++i) {
            float w = width * static_cast<float>(static_cast<double>(mix_[i].bytes) / total);
            if (i + 1 == mix_.size()) w = rect.right - x;
            if (w <= 0.0f) continue;
            Fill(dc, R(x, rect.top, x + w + 0.5f, rect.top + barH), 0.0f, HexColor(mix_[i].rgb));
            x += w;
            if (x < rect.right - 1.0f)
                Fill(dc, R(x - 0.5f * s, rect.top, x + 0.5f * s, rect.top + barH), 0.0f, theme.surface_card);
        }
        dc->PopAxisAlignedClip();
    }
    // Legend, flowing onto as many lines as needed (max 2).
    float y = rect.top + barH + 6.0f * s;
    float x = rect.left;
    const float lineH = 18.0f * s;
    int lines = 1;
    for (size_t i = 0; i < mix_.size() && i < legend_items; ++i) {
        const int pct = total ? static_cast<int>(std::lround(100.0 * mix_[i].bytes / total)) : 0;
        const std::wstring label = std::wstring(pulse::l10n::Get(static_cast<StringId>(mix_[i].label))) +
                                   L" " + std::to_wstring((std::max)(pct, pct == 0 && mix_[i].bytes ? 1 : 0)) + L"%";
        const float w = 12.0f * s + Measure(small_.Get(), label) + 12.0f * s;
        if (x + w > rect.right && x > rect.left) {
            if (lines == 2) break;
            ++lines;
            x = rect.left;
            y += lineH;
        }
        const float cy = y + lineH * 0.5f;
        Fill(dc, R(x, cy - 4.0f * s, x + 8.0f * s, cy + 4.0f * s), 2.0f * s, HexColor(mix_[i].rgb));
        DrawText(dc, small_.Get(), label, R(x + 12.0f * s, y, rect.right, y + lineH), theme.text_secondary);
        x += w;
    }
    return barH + 6.0f * s + lineH * lines;
}

void ArchivePreview::DrawBiggest(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, const Theme& theme) {
    const float s = scale_;
    DrawText(dc, small_.Get(), pulse::l10n::Get(StringId::ArcBiggest),
             R(rect.left, rect.top, rect.right, rect.top + 20.0f * s), theme.text_secondary);
    float y = rect.top + 24.0f * s;
    const uint64_t max = biggest_.empty() ? 1 : (std::max<uint64_t>)(1, nodes_[biggest_[0]].size);
    const float rowH = 34.0f * s;
    for (int idx : biggest_) {
        if (y + rowH > rect.bottom) break;
        const Node& n = nodes_[idx];
        DrawChip(dc, n, rect.left, y + 10.0f * s);
        const std::wstring size = pulse::format::ByteSize(n.size);
        const float sw = Measure(small_.Get(), size) + 4.0f * s;
        DrawText(dc, body_.Get(), n.name, R(rect.left + 34.0f * s, y, rect.right - sw - 6.0f * s, y + 20.0f * s), theme.text);
        DrawText(dc, small_.Get(), size, R(rect.right - sw, y, rect.right, y + 20.0f * s), theme.text_secondary,
                 DWRITE_TEXT_ALIGNMENT_TRAILING);
        const float barY = y + 24.0f * s;
        const float bw = (rect.right - rect.left - 34.0f * s);
        Fill(dc, R(rect.left + 34.0f * s, barY, rect.right, barY + 3.0f * s), 1.5f * s, theme.fill_hover);
        Fill(dc, R(rect.left + 34.0f * s, barY,
                   rect.left + 34.0f * s + (std::max)(3.0f * s, bw * static_cast<float>(static_cast<double>(n.size) / max)),
                   barY + 3.0f * s), 1.5f * s, HexColor(n.rgb, 0.75f));
        y += rowH;
    }
}

void ArchivePreview::DrawTree(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, const Theme& theme,
                              bool large) {
    const float s = scale_;
    tree_rect_ = rect;
    ClampScroll();
    const float rowH = RowHeight();
    const float indent = 16.0f * s;
    const bool dark = IsDarkTheme(theme);
    const bool showPacked = large && has_packed_ && (rect.right - rect.left) > 560.0f * s;
    const bool showDate = (rect.right - rect.left) > (large ? 420.0f : 330.0f) * s;
    const float dateW = showDate ? 118.0f * s : 0.0f;
    const float sizeW = 70.0f * s;
    const float packedW = showPacked ? 70.0f * s : 0.0f;
    const float colRight = rect.right - 10.0f * s;

    if (visible_.empty()) {
        const std::wstring msg = pulse::l10n::Get(!filter_.empty() ? StringId::ArcNoMatch
                                                       : folder_ ? StringId::EmptyFolder : StringId::ArcEmpty);
        DrawText(dc, body_.Get(), msg, rect, theme.text_disabled, DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }

    dc->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_ALIASED);
    const int first = (std::max)(0, static_cast<int>(scroll_ / rowH));
    const int last = (std::min)(static_cast<int>(visible_.size()) - 1,
                                static_cast<int>((scroll_ + (rect.bottom - rect.top)) / rowH));
    for (int row = first; row <= last; ++row) {
        const Node& n = nodes_[visible_[row]];
        const float top = rect.top + row * rowH - scroll_;
        const D2D1_RECT_F rowRect = R(rect.left, top, rect.right, top + rowH);
        const float cy = top + rowH * 0.5f;
        if (row == selected_) Fill(dc, R(rowRect.left + 2.0f * s, top + 1.0f * s, rowRect.right - 2.0f * s, rowRect.bottom - 1.0f * s), 5.0f * s, theme.fill_selected);
        else if (row == hover_) Fill(dc, R(rowRect.left + 2.0f * s, top + 1.0f * s, rowRect.right - 2.0f * s, rowRect.bottom - 1.0f * s), 5.0f * s, theme.fill_hover);

        // Indent guides.
        const float base = rect.left + 8.0f * s;
        for (int d = 0; d < n.depth; ++d) {
            const float gx = std::floor(base + d * indent + 7.0f * s) + 0.5f;
            Fill(dc, R(gx, top, gx + 1.0f, top + rowH), 0.0f, theme.stroke_divider);
        }
        float x = base + n.depth * indent;
        if (n.dir) {
            // Chevron: right when closed, down when open.
            const bool open = filter_.empty() ? n.open : true;
            const float c = 3.5f * s;
            const float cx = x + 7.0f * s;
            brush_->SetColor(theme.text_secondary);
            D2D1_POINT_2F a, b, e;
            if (open) { a = {cx - c, cy - c * 0.5f}; b = {cx, cy + c * 0.5f}; e = {cx + c, cy - c * 0.5f}; }
            else { a = {cx - c * 0.5f, cy - c}; b = {cx + c * 0.5f, cy}; e = {cx - c * 0.5f, cy + c}; }
            dc->DrawLine(a, b, brush_.Get(), 1.3f * s);
            dc->DrawLine(b, e, brush_.Get(), 1.3f * s);
            x += 16.0f * s;
            // Folder glyph (Segoe Fluent "FolderFill"-style tile).
            const D2D1_RECT_F body = R(x, cy - 5.5f * s, x + 18.0f * s, cy + 6.5f * s);
            Fill(dc, R(x, cy - 7.5f * s, x + 8.0f * s, cy - 3.0f * s), 1.5f * s, HexColor(kFolderGold, dark ? 0.9f : 1.0f));
            Fill(dc, body, 2.0f * s, HexColor(kFolderGold, dark ? 0.9f : 1.0f));
            Fill(dc, R(x, cy - 3.5f * s, x + 18.0f * s, cy + 6.5f * s), 2.0f * s, HexColor(0xF5C542, dark ? 0.95f : 1.0f));
            x += 24.0f * s;
        } else {
            x += 16.0f * s;
            DrawChip(dc, n, x - 4.0f * s, cy);
            x += 30.0f * s;
        }

        float nameRight = colRight - sizeW - dateW - packedW - 10.0f * s;
        std::wstring suffix;
        // Folder listings may leave a folder's rows out; its packed column
        // then carries the item count (folder_listing.h).
        if (n.dir) suffix = Fmt(StringId::ArcItems, static_cast<int>(folder_ && n.kids.empty()
                                                                      ? n.packed : n.kids.size()));
        const float suffixW = suffix.empty() ? 0.0f : Measure(small_.Get(), suffix) + 8.0f * s;
        const float nameW = Measure(body_.Get(), n.name);
        const float nameEnd = (std::min)(x + nameW + 2.0f * s, nameRight - suffixW);
        // Filter highlight behind the matched span.
        if (n.self_match && !filter_.empty()) {
            const size_t at = Lower(n.name).find(filter_);
            if (at != std::wstring::npos) {
                const float hx = x + Measure(body_.Get(), n.name.substr(0, at));
                const float hw = Measure(body_.Get(), n.name.substr(at, filter_.size()));
                if (hx < nameEnd)
                    Fill(dc, R(hx - 1.0f * s, cy - 9.0f * s, (std::min)(hx + hw + 1.0f * s, nameEnd), cy + 9.0f * s),
                         3.0f * s, HexColor(0xFACC15, dark ? 0.35f : 0.45f));
            }
        }
        DrawText(dc, body_.Get(), n.name, R(x, top, nameEnd, top + rowH), theme.text);
        float after = nameEnd + 6.0f * s;
        if (n.encrypted && !n.dir) {
            DrawText(dc, icon_.Get(), L"\uE72E", R(after, top, after + 16.0f * s, top + rowH), HexColor(0xD97706));
            after += 16.0f * s;
        }
        if (!suffix.empty())
            DrawText(dc, small_.Get(), suffix, R(after, top, nameRight, top + rowH), theme.text_disabled);

        float cx = colRight;
        if (showDate) {
            DrawText(dc, small_.Get(), n.date, R(cx - dateW, top, cx, top + rowH), theme.text_secondary,
                     DWRITE_TEXT_ALIGNMENT_TRAILING);
            cx -= dateW;
        }
        DrawText(dc, small_.Get(), pulse::format::ByteSize(n.size), R(cx - sizeW, top, cx, top + rowH),
                 n.dir ? theme.text_disabled : theme.text_secondary, DWRITE_TEXT_ALIGNMENT_TRAILING);
        cx -= sizeW;
        if (showPacked)
            DrawText(dc, small_.Get(), pulse::format::ByteSize(n.packed), R(cx - packedW, top, cx, top + rowH),
                     theme.text_disabled, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    dc->PopAxisAlignedClip();

    // Overlay scrollbar.
    const float content = rowH * visible_.size();
    const float view = rect.bottom - rect.top;
    if (content > view + 0.5f) {
        const float trackH = view - 4.0f * s;
        const float thumbH = (std::max)(24.0f * s, trackH * view / content);
        const float t = scroll_ / (content - view);
        const float ty = rect.top + 2.0f * s + (trackH - thumbH) * t;
        Fill(dc, R(rect.right - 5.0f * s, ty, rect.right - 2.0f * s, ty + thumbH), 1.5f * s, theme.scrollbar_thumb);
    }
}

float ArchivePreview::StateNoteHeight(float width) const {
    const std::wstring note = StateNote();
    if (note.empty() || width <= 0.0f || !factory_ || !small_) return 0.0f;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if (FAILED(factory_->CreateTextLayout(note.c_str(), static_cast<UINT32>(note.size()),
        small_.Get(), width, 1000.0f * scale_, &layout))) return 40.0f * scale_;
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    return metrics.height + 8.0f * scale_;
}

void ArchivePreview::DrawStateNote(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, const Theme& theme) {
    small_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    DrawText(dc, small_.Get(), StateNote(), rect, theme.text_disabled);
    small_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
}

// ---------------------------------------------------------------- layout

void ArchivePreview::Draw(ID2D1DeviceContext* dc, Compositor* compositor, const D2D1_RECT_F& rect,
                          const Theme& theme, float scale, bool large) {
    if (!dc || !parsed_) return;
    scale_ = scale > 0.0f ? scale : 1.0f;
    const float s = scale_;
    EnsureResources(dc, compositor, s);
    if (!brush_ || !body_) return;
    view_rect_ = rect;
    const float width = rect.right - rect.left;
    // Quick Look collapses to the compact layout when the window is narrow.
    large_ = large && width >= 640.0f * s;
    const float pad = (large_ ? 18.0f : 12.0f) * s;

    if (!large_) {
        // Compact: header, type bar, divider, tree, optional partial note.
        D2D1_RECT_F r = R(rect.left + pad, rect.top + pad, rect.right - pad, rect.bottom - pad);
        const float h = r.bottom - r.top;
        float y = r.top;
        y += DrawHeader(dc, R(r.left, y, r.right, r.bottom), theme, false) + 10.0f * s;
        if (h > 220.0f * s && !mix_.empty()) y += DrawMix(dc, R(r.left, y, r.right, r.bottom), theme, 4) + 6.0f * s;
        Fill(dc, R(r.left, y, r.right, y + 1.0f), 0.0f, theme.stroke_divider);
        y += 4.0f * s;
        float bottom = r.bottom;
        if (incomplete_ || counting_ || !filter_.empty()) {
            const float note_height = StateNoteHeight(r.right - r.left);
            DrawStateNote(dc, R(r.left, bottom - note_height, r.right, bottom), theme);
            bottom -= note_height;
        }
        DrawTree(dc, R(rect.left + 4.0f * s, y, rect.right - 4.0f * s, bottom), theme, false);
        return;
    }

    // Large: summary column on the left, full tree on the right.
    const float side = (std::min)(300.0f * s, width * 0.34f);
    const D2D1_RECT_F left = R(rect.left + pad, rect.top + pad, rect.left + side, rect.bottom - pad);
    float y = left.top;
    // The window header already names the file, its type and size; only the
    // lock stays here.
    if (any_encrypted_) {
        const std::wstring label = pulse::l10n::Get(StringId::ArcEncrypted);
        const float w = Measure(small_.Get(), label) + 26.0f * s;
        const D2D1_RECT_F badge = R(left.left, y, left.left + w, y + 20.0f * s);
        Fill(dc, badge, 10.0f * s, HexColor(0xD97706, 0.14f));
        DrawText(dc, icon_.Get(), L"\uE72E", R(badge.left + 6.0f * s, badge.top, badge.left + 20.0f * s, badge.bottom),
                 HexColor(0xD97706));
        DrawText(dc, small_.Get(), label, R(badge.left + 20.0f * s, badge.top, badge.right, badge.bottom),
                 HexColor(0xB45309));
        y = badge.bottom + 12.0f * s;
    }

    // Stat cards.
    const float gap = 8.0f * s;
    const float cardW = (left.right - left.left - gap * 2.0f) / 3.0f;
    const float cardH = 62.0f * s;
    struct Card { std::wstring value, label; bool ratio; };
    const Card cards[3] = {
        {std::to_wstring(file_count_), pulse::l10n::Get(StringId::ArcFiles), false},
        {std::to_wstring(dir_count_), pulse::l10n::Get(StringId::ArcFolders), false},
        {(folder_ && (incomplete_ || counting_) ? L"\x2265" : L"") + pulse::format::ByteSize(unpacked_),
         folder_ ? std::wstring(pulse::l10n::Get(pulse::l10n::StringId::TotalSize).c_str())
                 : std::wstring(pulse::l10n::Get(StringId::ArcUnpacked)),
         !folder_ && format_ != L"ISO" && format_ != L"ISO/UDF" && format_ != L"UDF"},
    };
    for (int i = 0; i < 3; ++i) {
        const float cx = left.left + i * (cardW + gap);
        const D2D1_RECT_F card = R(cx, y, cx + cardW, y + cardH);
        Fill(dc, card, 8.0f * s, theme.fill_hover);
        DrawText(dc, value_.Get(), cards[i].value, R(card.left + 10.0f * s, card.top + 8.0f * s, card.right - 6.0f * s, card.top + 32.0f * s), theme.text);
        DrawText(dc, small_.Get(), cards[i].label, R(card.left + 10.0f * s, card.top + 32.0f * s, card.right - 6.0f * s, card.top + 48.0f * s), theme.text_secondary);
        if (cards[i].ratio && unpacked_ > 0) {
            const uint64_t packed = has_packed_ ? packed_total_ : file_size_;
            const double ratio = (std::min)(1.0, static_cast<double>(packed) / unpacked_);
            const D2D1_RECT_F bar = R(card.left + 10.0f * s, card.bottom - 9.0f * s, card.right - 10.0f * s, card.bottom - 6.0f * s);
            Fill(dc, bar, 1.5f * s, WithAlpha(theme.text, 0.10f));
            Fill(dc, R(bar.left, bar.top, bar.left + (std::max)(3.0f * s, (bar.right - bar.left) * static_cast<float>(ratio)), bar.bottom),
                 1.5f * s, HexColor(kArchiveAmber));
        }
    }
    y += cardH + 6.0f * s;
    if (unpacked_ > 0 && !folder_ && format_ != L"ISO" && format_ != L"ISO/UDF" && format_ != L"UDF") {
        const uint64_t packed = has_packed_ ? packed_total_ : file_size_;
        const int pct = static_cast<int>(std::lround(100.0 * (std::min)(1.0, static_cast<double>(packed) / unpacked_)));
        DrawText(dc, small_.Get(), Fmt(StringId::ArcRatio, pct), R(left.left, y, left.right, y + 18.0f * s), theme.text_disabled,
                 DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    y += 26.0f * s;
    if (!mix_.empty()) y += DrawMix(dc, R(left.left, y, left.right, left.bottom), theme, 8) + 18.0f * s;
    if (!biggest_.empty() && left.bottom - y > 80.0f * s) DrawBiggest(dc, R(left.left, y, left.right, left.bottom), theme);

    // Divider between the columns.
    const float divX = std::floor(rect.left + side + pad) + 0.5f;
    Fill(dc, R(divX, rect.top + pad, divX + 1.0f, rect.bottom - pad), 0.0f, theme.stroke_divider);

    // Right: column header, tree, footer.
    const D2D1_RECT_F right = R(divX + pad * 0.5f, rect.top + pad * 0.5f, rect.right - pad * 0.5f, rect.bottom - pad * 0.5f);
    const float headH = 30.0f * s;
    const std::wstring state_note = StateNote();
    const float noteH = StateNoteHeight(right.right - right.left - 22.0f * s);
    const float footH = noteH > 0.0f ? noteH + 6.0f * s : 0.0f;
    {
        const float w = right.right - right.left;
        const bool showPacked = has_packed_ && w > 560.0f * s;
        const bool showDate = w > 420.0f * s;
        float cx = right.right - 10.0f * s;
        const D2D1_RECT_F head = R(right.left, right.top, right.right, right.top + headH);
        DrawText(dc, small_.Get(), pulse::l10n::Get(StringId::ArcColName), R(head.left + 12.0f * s, head.top, head.right, head.bottom), theme.text_secondary);
        if (showDate) {
            DrawText(dc, small_.Get(), pulse::l10n::Get(StringId::ArcColModified), R(cx - 118.0f * s, head.top, cx, head.bottom), theme.text_secondary, DWRITE_TEXT_ALIGNMENT_TRAILING);
            cx -= 118.0f * s;
        }
        DrawText(dc, small_.Get(), pulse::l10n::Get(StringId::ArcColSize), R(cx - 70.0f * s, head.top, cx, head.bottom), theme.text_secondary, DWRITE_TEXT_ALIGNMENT_TRAILING);
        cx -= 70.0f * s;
        if (showPacked)
            DrawText(dc, small_.Get(), pulse::l10n::Get(StringId::ArcColPacked), R(cx - 70.0f * s, head.top, cx, head.bottom), theme.text_secondary, DWRITE_TEXT_ALIGNMENT_TRAILING);
        Fill(dc, R(head.left, head.bottom - 1.0f, head.right, head.bottom), 0.0f, theme.stroke_divider);
    }
    DrawTree(dc, R(right.left, right.top + headH + 2.0f * s, right.right, right.bottom - footH), theme, true);
    if (!state_note.empty() && footH > 0.0f) {
        Fill(dc, R(right.left, right.bottom - footH, right.right, right.bottom - footH + 1.0f), 0.0f, theme.stroke_divider);
        DrawStateNote(dc, R(right.left + 12.0f * s, right.bottom - footH + 4.0f * s, right.right - 10.0f * s, right.bottom), theme);
    }
}

std::wstring ArchivePreview::Hint() const {
    if (!parsed_ || (file_count_ == 0 && dir_count_ == 0)) return {};
    return pulse::l10n::Get(StringId::ArcSearchHint);
}

std::vector<std::wstring> ArchivePreview::StatusParts() const {
    std::vector<std::wstring> parts;
    if (!parsed_) return parts;
    const std::wstring at_least = incomplete_ || counting_ ? L"\x2265 " : L"";
    const std::wstring files = !filter_.empty()
        ? Fmt2(StringId::ArcMatches, static_cast<int>(filter_hits_), static_cast<int>(file_count_))
        : std::to_wstring(file_count_) + L" " + pulse::l10n::Get(StringId::ArcFiles);
    const std::wstring dirs = std::to_wstring(dir_count_) + L" " + pulse::l10n::Get(StringId::ArcFolders);
    if (folder_) {
        parts.push_back(files);
        parts.push_back(dirs);
        std::wstring total = at_least + pulse::format::ByteSize(unpacked_);
        if (counting_) total += pulse::l10n::Get(pulse::l10n::StringId::Counting_2).c_str();
        parts.push_back(std::move(total));
        // "YYYY-MM-DD HH:MM" sorts as text.
        const std::wstring* newest = nullptr;
        for (const Node& n : nodes_)
            if (!n.date.empty() && (!newest || n.date > *newest)) newest = &n.date;
        if (newest) {
            wchar_t buffer[96];
            swprintf_s(buffer, pulse::l10n::Get(pulse::l10n::StringId::PreviewLatestModified).c_str(), newest->c_str());
            parts.push_back(buffer);
        }
        return parts;
    }
    const bool image = format_ == L"ISO" || format_ == L"ISO/UDF" || format_ == L"UDF";
    parts.push_back(format_ + L" " + (image ? std::wstring(pulse::l10n::Get(pulse::l10n::StringId::Image).c_str())
                                            : std::wstring(pulse::l10n::Get(StringId::ArcArchive))));
    parts.push_back(files);
    if (unpacked_ > 0)
        parts.push_back((image ? std::wstring(pulse::l10n::Get(pulse::l10n::StringId::LoadedContent).c_str())
                               : std::wstring(pulse::l10n::Get(StringId::ArcUnpacked))) +
                        L" " + at_least + pulse::format::ByteSize(unpacked_));
    if (any_encrypted_) parts.push_back(pulse::l10n::Get(StringId::ArcEncrypted));
    if (file_size_) parts.push_back(pulse::format::ByteSize(file_size_, true));
    return parts;
}

} // namespace pulse::ui

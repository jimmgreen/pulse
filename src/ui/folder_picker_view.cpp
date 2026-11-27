#include "../common/windows_compat.h"
#include "folder_picker_view.h"

#include "../common/localization.h"
#include "store_icons.h"
#include "command_icons.h"
#include "typography.h"
#include "window_helpers.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <iterator>

namespace pulse::ui {
namespace {

constexpr float kFieldH = 34.0f;
constexpr float kButtonMinW = 120.0f;
constexpr float kSidePad = 16.0f;

float Dip(float scale, float value) { return value * scale; }

fluent::ControlState StateFor(const FolderPickerChrome& c, int id, bool enabled = true) {
    fluent::ControlState state{};
    state.enabled = enabled;
    state.hovered = enabled && c.hover == id;
    state.pressed = enabled && c.pressed == id;
    state.keyboard_focus = c.show_focus && c.focus == id;
    return state;
}

std::wstring FitEnd(Compositor& compositor, IDWriteTextFormat* format, const std::wstring& text,
                    float width) {
    return FitTextEnd(text, width, [&](std::wstring_view s) {
        return typography::MeasureLine(&compositor, format, s);
    });
}

// Text widths from the painter's own measure (body and caption faces).
float BodyWidth(const fluent::Painter& p, float scale, const std::wstring& text) {
    return text.empty() ? 0.0f : std::max(0.0f, p.MeasureButtonWidth(text) - Dip(scale, 16.0f));
}
float CaptionWidth(const fluent::Painter& p, float scale, const std::wstring& text) {
    // A keycap adds its caption text plus 18 DIP of chip padding to an empty button.
    return text.empty() ? 0.0f : std::max(0.0f, p.MeasureButtonWidth(L"", {}, false, text) - Dip(scale, 34.0f));
}
float MediumWidth(const fluent::Painter& p, float scale, const std::wstring& text) {
    // FileNameFormat / TabFormat (13 DIP) sits halfway between Caption (12 DIP) and Body (14 DIP).
    return text.empty() ? 0.0f : (CaptionWidth(p, scale, text) + BodyWidth(p, scale, text)) * 0.5f + Dip(scale, 1.0f);
}
float KeycapWidth(const fluent::Painter& p, float scale, const std::wstring& text) {
    return text.empty() ? 0.0f : std::max(Dip(scale, 18.0f), CaptionWidth(p, scale, text) + Dip(scale, 10.0f));
}
void DrawTinyText(Compositor& compositor, ID2D1DeviceContext* dc, float scale,
                  std::wstring_view text, const D2D1_RECT_F& box, float dip_size,
                  DWRITE_FONT_WEIGHT weight, const D2D1_COLOR_F& color,
                  DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_CENTER) {
    if (!dc || !compositor.DwriteFactory() || text.empty()) return;
    ComPtr<IDWriteTextFormat> fmt;
    typography::CreateTextFormat(compositor.DwriteFactory(), {typography::FontRole::Text, Dip(scale, dip_size), weight}, &fmt);
    if (!fmt.get()) return;
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    fmt->SetTextAlignment(align);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    ComPtr<IDWriteTextLayout> layout;
    const float bw = std::max(1.0f, box.right - box.left);
    const float bh = std::max(1.0f, box.bottom - box.top);
    if (FAILED(compositor.DwriteFactory()->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()),
            fmt.get(), bw, bh, &layout)) || !layout.get()) return;
    ID2D1SolidColorBrush* br = nullptr;
    if (SUCCEEDED(dc->CreateSolidColorBrush(color, &br)) && br) {
        dc->DrawTextLayout(D2D1::Point2F(box.left, box.top), layout.get(), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        br->Release();
    }
}
void DrawPickerKeycap(Compositor& compositor, ID2D1DeviceContext* dc, fluent::Painter& painter,
                      const Theme& theme, float scale, const D2D1_RECT_F& r,
                      std::wstring_view text, bool high_contrast, bool enabled = true) {
    if (r.right - r.left <= Dip(scale, 10.0f) || r.bottom <= r.top || text.empty()) return;
    const D2D1_COLOR_F k_bg = high_contrast ? theme.bg : theme.surface_card;
    const D2D1_COLOR_F k_bd = high_contrast ? theme.text : theme.stroke_card;
    const D2D1_COLOR_F k_fg = high_contrast ? theme.text : (enabled ? theme.text_secondary : theme.text_disabled);
    painter.FillRoundedRect(r, Dip(scale, 4.0f), k_bg);
    painter.StrokeRoundedRect(r, Dip(scale, 4.0f), k_bd, std::max(1.0f, Dip(scale, 1.0f)));
    DrawTinyText(compositor, dc, scale, text, r, 11.0f, DWRITE_FONT_WEIGHT_NORMAL, k_fg);
}
constexpr float kChipH = 28.0f, kChipIcon = 14.0f, kChipLabelMax = 150.0f;
std::wstring OpenFolderKey(int index) { return L"Alt " + std::to_wstring(index + 1); }
float KeyWidth(const fluent::Painter& p, float scale, int index) {
    return KeycapWidth(p, scale, OpenFolderKey(index));
}
float ChipWidth(const fluent::Painter& p, float scale, const PickerOpenChip& chip, int key_index) {
    const float key = key_index >= 0 ? KeyWidth(p, scale, key_index) + Dip(scale, 7.0f) : 0.0f;
    const float label = std::min(MediumWidth(p, scale, chip.label), Dip(scale, kChipLabelMax));
    const float source = chip.source.empty() ? 0.0f : CaptionWidth(p, scale, chip.source) + Dip(scale, 7.0f);
    return Dip(scale, 6.0f) + key + Dip(scale, kChipIcon + 7.0f) + label + source + Dip(scale, 10.0f);
}
float CompactNewFolderWidth(const fluent::Painter& p, float scale, const std::wstring& text, const std::wstring& keycap) {
    float w = Dip(scale, 8.0f) + Dip(scale, 14.0f) + Dip(scale, 6.0f) + CaptionWidth(p, scale, text) + Dip(scale, 8.0f);
    if (!keycap.empty()) w += Dip(scale, 6.0f) + KeycapWidth(p, scale, keycap);
    return w;
}
// Strip shapes from the shared command set (HTML clock / jump), at a store-icon stroke.
void DrawCommandShape(ID2D1DeviceContext* dc, command_icons::Icon icon, D2D1_RECT_F r, D2D1_COLOR_F color, float stroke_px) {
    const float side = std::min(r.right - r.left, r.bottom - r.top);
    ID2D1SolidColorBrush* brush = nullptr;
    if (!dc || side <= 0.0f || FAILED(dc->CreateSolidColorBrush(color, &brush))) return;
    ID2D1StrokeStyle* stroke = nullptr;
    command_icons::CreateStrokeStyle(dc, &stroke);
    command_icons::Draw(dc, brush, stroke, icon, r, 1.0f, stroke_px * command_icons::kCanvas / side);
    if (stroke) stroke->Release();
    brush->Release();
}
// An Explorer window (HTML "window"): a frame with a title bar line, on the 16-unit icon grid.
void DrawWindowShape(fluent::Painter& painter, D2D1_RECT_F r, D2D1_COLOR_F color, float stroke_px) {
    const float u = std::min(r.right - r.left, r.bottom - r.top) / 16.0f;
    const auto frame = D2D1::RectF(r.left + 1.75f * u, r.top + 2.75f * u, r.left + 14.25f * u, r.top + 13.25f * u);
    painter.StrokeRoundedRect(frame, 1.5f * u, color, stroke_px);
    painter.FillRoundedRect(D2D1::RectF(frame.left, r.top + 5.75f * u - stroke_px * 0.5f, frame.right, r.top + 5.75f * u + stroke_px * 0.5f), 0, color);
}
// A type filter chip's icon (HTML fchip): none for 全部文件, a picture for image types,
// a document otherwise; Folder mode's only chip is 只看文件夹.
StoreIcon FilterChipIcon(const FolderPickerChrome& chrome, const PickerOptionChip& filter) {
    if (chrome.mode == PickerMode::Folder) return StoreIcon::Folder;
    static constexpr std::wstring_view kImages[] = {L"png", L"jpg", L"jpeg", L"gif", L"bmp", L"webp", L"tif", L"tiff",
        L"psd", L"heic", L"heif", L"ico", L"svg", L"avif", L"raw", L"dng", L"jfif"};
    bool any = false, images = true;
    size_t start = 0;
    const std::wstring& pattern = filter.label;
    while (start <= pattern.size()) {
        size_t end = pattern.find(L';', start);
        if (end == std::wstring::npos) end = pattern.size();
        std::wstring part = pattern.substr(start, end - start);
        start = end + 1;
        while (!part.empty() && part.front() == L' ') part.erase(part.begin());
        while (!part.empty() && part.back() == L' ') part.pop_back();
        if (part.empty()) continue;
        if (part == L"*" || part == L"*.*") return StoreIcon::None;
        any = true;
        const auto dot = part.rfind(L'.');
        std::wstring ext = dot == std::wstring::npos ? L"" : part.substr(dot + 1);
        for (auto& ch : ext) ch = static_cast<wchar_t>(towlower(ch));
        if (std::find(std::begin(kImages), std::end(kImages), ext) == std::end(kImages)) images = false;
    }
    return !any ? StoreIcon::None : images ? StoreIcon::Image : StoreIcon::File;
}
fluent::InfoBarSpec AutoJumpInfo(const FolderPickerChrome& c, const D2D1_RECT_F& bounds) {
    fluent::InfoBarSpec spec;
    spec.bounds = bounds; spec.title = c.auto_jump_notice; spec.show_close = false;
    spec.actions[0] = c.undo_jump_text;
    spec.hovered_action = c.hover == kPickUndoJump ? 0 : -1;
    return spec;
}

} // namespace

float PickerFooterDip(PickerMode, bool overwrite_pending, bool has_options, bool, bool has_suggestions) {
    return 100.0f + (has_suggestions ? 36.0f : 0.0f) + (overwrite_pending ? 44.0f : 0.0f) + (has_options ? 38.0f : 0.0f);
}

FolderPickerChromeLayout LayoutPickerChrome(float width, float height,
                                            const FolderPickerChrome& chrome,
                                            const fluent::Painter& painter, float scale) {
    FolderPickerChromeLayout l;
    l.scale = scale;
    l.width = width;
    l.height = height;
    l.title = D2D1::RectF(0, 0, width, Dip(scale, kPickerTitleDip));
    l.close = D2D1::RectF(width - Dip(scale, 46.0f), 0, width, l.title.bottom);
    const bool format_row = chrome.mode == PickerMode::Save && !chrome.formats.empty();
    const bool suggestion_row = !chrome.save_suggestions.empty() || !chrome.format_note.empty();
    const float format_height = 0.0f; // Save formats share the filename row.
    l.footer = D2D1::RectF(0, height - Dip(scale, PickerFooterDip(chrome.mode, chrome.overwrite_pending || chrome.live_conflict, !chrome.options.empty(), format_row, suggestion_row)), width, height);

    const float left = Dip(scale, kSidePad);
    const float right = width - Dip(scale, kSidePad);
    const float btn_h = painter.MeasureButtonHeight();
    // The last row: what will be picked on the left, the buttons on the right.
    const float button_band = Dip(scale, 56.0f);
    const float by = height - button_band + (button_band - btn_h) * 0.5f;
    const float primary_w = std::min(std::max(painter.MeasureButtonWidth(chrome.primary_text) + (chrome.primary_drop_down ? Dip(scale,28.0f) : 0.0f),
                                     Dip(scale, kButtonMinW)),std::max(0.0f,(right-left-Dip(scale,8.0f))*0.5f));
    const float cancel_w = std::min(std::max(painter.MeasureButtonWidth(chrome.cancel_text),
                                    Dip(scale, kButtonMinW)), primary_w);
    l.primary = D2D1::RectF(right - primary_w, by, right, by + btn_h);
    l.cancel = D2D1::RectF(l.primary.left - Dip(scale, 8.0f) - cancel_w, by,
                           l.primary.left - Dip(scale, 8.0f), by + btn_h);
    l.summary = D2D1::RectF(left, by, std::max(left, l.cancel.left - Dip(scale, 16.0f)), by + btn_h);
    if (chrome.footer_tool.id != 0) {
        const float tool_w = painter.MeasureButtonWidth(chrome.footer_tool.text, {}, true);
        if (l.summary.right - l.summary.left >= tool_w + Dip(scale, 128.0f)) {
            l.footer_tool = D2D1::RectF(l.cancel.left - Dip(scale, 8.0f) - tool_w, by,
                l.cancel.left - Dip(scale, 8.0f), by + btn_h);
            l.summary.right = l.footer_tool.left - Dip(scale, 16.0f);
        }
    }
    if (chrome.show_tray_remove && !chrome.tray_remove_text.empty()) {
        const float anchor_right = l.footer_tool.right > l.footer_tool.left
            ? l.footer_tool.left - Dip(scale, 10.0f) : l.cancel.left - Dip(scale, 12.0f);
        const float cb_w = std::min(std::max(0.0f, anchor_right - left),
            Dip(scale, 24.0f) + CaptionWidth(painter, scale, chrome.tray_remove_text) + Dip(scale, 6.0f));
        const float min_summary = chrome.summary.empty() && chrome.selection_summary.empty() ? Dip(scale, 0.0f) : Dip(scale, 96.0f);
        if (anchor_right - left >= cb_w + min_summary && cb_w >= Dip(scale, 60.0f)) {
            l.tray_remove = D2D1::RectF(anchor_right - cb_w, by, anchor_right, by + btn_h);
            l.summary.right = std::max(l.summary.left, l.tray_remove.left - Dip(scale, 12.0f));
        }
    }
    if (!chrome.save_space_text.empty() && chrome.summary_warning.empty() && chrome.notice.empty() &&
        chrome.save_format_notice.empty()) {
        const float space_width = CaptionWidth(painter, scale, chrome.save_space_text) + Dip(scale, 4.0f);
        // Keep the destination readable; narrow windows retain space in the tooltip.
        if (l.summary.right - l.summary.left >= space_width + Dip(scale, 320.0f))
            l.summary_space = D2D1::RectF(l.summary.right - space_width, l.summary.top, l.summary.right, l.summary.bottom);
    }
    if (chrome.overwrite_pending || chrome.live_conflict) {
        // HTML 同名冲突: a banner on the summary row says what exists and offers
        // 返回修改 / 改名保存; the button row keeps 取消 and 替换 (the primary).
        l.conflict = D2D1::RectF(left, l.footer.top + Dip(scale, 52.0f + (suggestion_row ? 36.0f : 0.0f) + format_height + (chrome.options.empty() ? 0.0f : 38.0f)), right, l.footer.top + Dip(scale, 86.0f + (suggestion_row ? 36.0f : 0.0f) + format_height + (chrome.options.empty() ? 0.0f : 38.0f)));
        const float bh = std::min(btn_h, Dip(scale, 28.0f));
        const float bt = (l.conflict.top + l.conflict.bottom - bh) * 0.5f;
        const float cap = (right - left) * 0.3f;
        const float rename_w = std::min(painter.MeasureButtonWidth(chrome.rename_text), cap);
        const float back_w = std::min(painter.MeasureButtonWidth(chrome.overwrite_back_text), cap);
        l.rename = D2D1::RectF(l.conflict.right - Dip(scale, 4.0f) - rename_w, bt, l.conflict.right - Dip(scale, 4.0f), bt + bh);
        l.overwrite_back = D2D1::RectF(l.rename.left - Dip(scale, 6.0f) - back_w, bt, l.rename.left - Dip(scale, 6.0f), bt + bh);
        l.conflict_text = D2D1::RectF(l.conflict.left + Dip(scale, 34.0f), l.conflict.top, l.overwrite_back.left - Dip(scale, 8.0f), l.conflict.bottom);
        if (!chrome.overwrite_replace_text.empty()) {
            const float replace_w = std::min(painter.MeasureButtonWidth(chrome.overwrite_replace_text), cap);
            l.overwrite_replace = D2D1::RectF(l.conflict.right - Dip(scale, 4.0f) - replace_w, bt,
                l.conflict.right - Dip(scale, 4.0f), bt + bh);
            const float candidate_w = std::min(painter.MeasureButtonWidth(chrome.rename_text), (right - left) * 0.45f);
            l.rename = D2D1::RectF(l.overwrite_replace.left - Dip(scale, 6.0f) - candidate_w, bt,
                l.overwrite_replace.left - Dip(scale, 6.0f), bt + bh);
            l.overwrite_back = {};
            l.conflict_text.right = std::max(l.conflict_text.left, l.rename.left - Dip(scale, 8.0f));
        }
    }
    if (chrome.live_conflict && !chrome.overwrite_pending) {
        const float y = l.conflict.top + Dip(scale, 3.0f), h = Dip(scale, 28.0f);
        const float save_w = painter.MeasureButtonWidth(chrome.live_save_text) + Dip(scale, 8.0f);
        const float rename_w = std::min(painter.MeasureButtonWidth(chrome.live_rename_text) + Dip(scale, 8.0f), (right - left) * 0.38f);
        l.live_save = D2D1::RectF(right - save_w, y, right, y + h);
        l.live_rename = D2D1::RectF(l.live_save.left - Dip(scale, 6.0f) - rename_w, y, l.live_save.left - Dip(scale, 6.0f), y + h);
        l.conflict_text.right = std::max(l.conflict_text.left, l.live_rename.left - Dip(scale, 8.0f));
        l.rename = l.overwrite_back = l.overwrite_replace = {};
    }

    if (chrome.strip_top > 0.0f) {
        const float top = Dip(scale, chrome.strip_top);
        const float strip_right = chrome.strip_right > 0 ? std::min(width, Dip(scale, chrome.strip_right)) : width;
        l.strip = D2D1::RectF(Dip(scale, chrome.strip_left), top, strip_right, top + Dip(scale, kPickerStripDip));
        const float strip_x = l.strip.left + Dip(scale, chrome.strip_left > 0.0f ? 12.0f : kSidePad);
        const float ch = Dip(scale, kChipH);
        const float cy = top + (Dip(scale, kPickerStripDip) - ch) * 0.5f;
        // Folders come first: when the 新建文件夹 keycap would hide one, the keycap goes.
        const auto place_buttons = [&](bool keycap) {
            float end = std::min(right, strip_right - Dip(scale, kSidePad));
            l.new_folder = l.undo_jump = {};
            if (chrome.context_top > 0) return end;
            l.new_folder_keycap = keycap && !chrome.new_folder_key.empty();
            if (!chrome.new_folder_text.empty()) {
                const float w = CompactNewFolderWidth(painter, scale, chrome.new_folder_text,
                    l.new_folder_keycap ? chrome.new_folder_key : std::wstring());
                l.new_folder = D2D1::RectF(end - w, cy + Dip(scale, 1.0f), end, cy + ch - Dip(scale, 1.0f));
                end = l.new_folder.left - Dip(scale, 4.0f);
            }
            if (!chrome.undo_jump_text.empty()) {
                const float w = Dip(scale, 8.0f) + Dip(scale, 14.0f) + Dip(scale, 6.0f) + CaptionWidth(painter, scale, chrome.undo_jump_text) + Dip(scale, 8.0f);
                l.undo_jump = D2D1::RectF(end - w, cy + Dip(scale, 1.0f), end, cy + ch - Dip(scale, 1.0f));
                end = l.undo_jump.left - Dip(scale, 8.0f);
            }
            return end;
        };
        const float first_x = strip_x + CaptionWidth(painter, scale, chrome.strip_caption) + Dip(scale, 10.0f);
        constexpr size_t kMaxOpenStripChips = 96;
        const size_t shown = std::min<size_t>(chrome.open_folders.size(), kMaxOpenStripChips);
        float end = place_buttons(true);
        {
            float need = first_x;
            for (size_t i = 0; i < shown; ++i) {
                PickerOpenChip bare = chrome.open_folders[i];
                bare.source.clear();
                need += ChipWidth(painter, scale, bare, i < 9 ? static_cast<int>(i) : -1) + Dip(scale, 6.0f);
            }
            if (need > end) end = place_buttons(false);
        }
        const int last_key_index = !chrome.last_location.label.empty() && shown < 9 ? static_cast<int>(shown) : -1;
        const float last_chip_w = chrome.last_location.label.empty() ? 0.0f : ChipWidth(painter, scale, chrome.last_location, last_key_index);
        const float origin_chip_w = chrome.save_original_location.label.empty() ? 0.0f : ChipWidth(painter, scale, chrome.save_original_location, -1);
        const float repo_chip_w = chrome.save_repo_root.label.empty() ? 0.0f : ChipWidth(painter, scale, chrome.save_repo_root, -1);
        // Source labels give way before scrolling kicks in: the "Pulse" ones first, then
        // Explorer ones, so the chips and the last location fit without scrolling when possible.
        std::vector<bool> keep_source(shown);
        const auto row_width = [&] {
            float total = first_x;
            bool has_any = false;
            for (size_t i = 0; i < shown; ++i) {
                PickerOpenChip chip = chrome.open_folders[i];
                if (!keep_source[i]) chip.source.clear();
                if (has_any) total += Dip(scale, 6.0f);
                total += ChipWidth(painter, scale, chip, i < 9 ? static_cast<int>(i) : -1);
                has_any = true;
            }
            if (last_chip_w > 0.0f) {
                if (has_any) total += Dip(scale, 17.0f);
                total += last_chip_w;
                has_any = true;
            }
            if (origin_chip_w > 0.0f || repo_chip_w > 0.0f) {
                if (has_any) total += Dip(scale, 17.0f);
                if (origin_chip_w > 0.0f) { total += origin_chip_w; if (repo_chip_w > 0.0f) total += Dip(scale, 6.0f); }
                total += repo_chip_w;
            }
            return total;
        };
        for (size_t i = 0; i < shown; ++i) keep_source[i] = !chrome.open_folders[i].source.empty();
        if (row_width() > end) for (size_t i = 0; i < shown; ++i) if (!chrome.open_folders[i].window) keep_source[i] = false;
        if (row_width() > end) for (size_t i = 0; i < shown; ++i) keep_source[i] = false;

        std::vector<float> chip_widths(shown);
        float content_w = 0.0f;
        bool has_prev = false;
        for (size_t i = 0; i < shown; ++i) {
            PickerOpenChip chip = chrome.open_folders[i];
            if (!keep_source[i]) chip.source.clear();
            chip_widths[i] = ChipWidth(painter, scale, chip, i < 9 ? static_cast<int>(i) : -1);
            if (has_prev) content_w += Dip(scale, 6.0f);
            content_w += chip_widths[i];
            has_prev = true;
        }
        if (last_chip_w > 0.0f) {
            if (has_prev) content_w += Dip(scale, 17.0f);
            content_w += last_chip_w;
            has_prev = true;
        }
        if (origin_chip_w > 0.0f || repo_chip_w > 0.0f) {
            if (has_prev) content_w += Dip(scale, 17.0f);
            if (origin_chip_w > 0.0f) {
                content_w += origin_chip_w;
                if (repo_chip_w > 0.0f) content_w += Dip(scale, 6.0f);
            }
            content_w += repo_chip_w;
        }
        const float raw_avail = std::max(0.0f, end - first_x);
        if (content_w > raw_avail + 0.5f && raw_avail > Dip(scale, 76.0f)) {
            const float btn_w = Dip(scale, 22.0f);
            const float btn_gap = Dip(scale, 4.0f);
            l.open_scroll_left = D2D1::RectF(first_x, cy, first_x + btn_w, cy + ch);
            l.open_scroll_right = D2D1::RectF(end - btn_w, cy, end, cy + ch);
            l.open_viewport = D2D1::RectF(l.open_scroll_left.right + btn_gap, l.strip.top,
                                          std::max(l.open_scroll_left.right + btn_gap, l.open_scroll_right.left - btn_gap), l.strip.bottom);
        } else {
            l.open_viewport = D2D1::RectF(first_x, l.strip.top, std::max(first_x, end), l.strip.bottom);
        }
        const float vp_w = std::max(0.0f, l.open_viewport.right - l.open_viewport.left);
        l.open_scroll_max = scale > 0.0f ? std::max(0.0f, (content_w - vp_w) / scale) : 0.0f;
        const float scroll_px = Dip(scale, std::clamp(chrome.open_scroll, 0.0f, l.open_scroll_max));
        float x = l.open_viewport.left - scroll_px;
        bool placed_any = false;
        for (size_t i = 0; i < shown; ++i) {
            const float w = chip_widths[i];
            l.open_chips.push_back(D2D1::RectF(x, cy, x + w, cy + ch));
            if (i < 9) {
                const float kw = KeyWidth(painter, scale, static_cast<int>(i));
                l.open_keycaps.push_back(D2D1::RectF(x + w - Dip(scale, 6.0f) - kw, cy + Dip(scale, 5.0f),
                    x + w - Dip(scale, 6.0f), cy + Dip(scale, 23.0f)));
            }
            l.open_chip_source.push_back(keep_source[i]);
            x += w + Dip(scale, 6.0f);
            placed_any = true;
        }
        if (last_chip_w > 0.0f) {
            if (placed_any) x += Dip(scale, 11.0f);
            l.last_location = D2D1::RectF(x, cy, x + last_chip_w, cy + ch);
            if (last_key_index >= 0) {
                const float kw = KeyWidth(painter, scale, last_key_index);
                l.last_location_keycap = D2D1::RectF(l.last_location.right - Dip(scale, 6.0f) - kw, cy + Dip(scale, 5.0f),
                    l.last_location.right - Dip(scale, 6.0f), cy + Dip(scale, 23.0f));
            }
            x += last_chip_w + Dip(scale, 6.0f);
            placed_any = true;
        }
        if (origin_chip_w > 0.0f || repo_chip_w > 0.0f) {
            if (placed_any) x += Dip(scale, 11.0f);
            if (origin_chip_w > 0.0f) {
                l.save_origin = D2D1::RectF(x, cy, x + origin_chip_w, cy + ch);
                x += origin_chip_w + Dip(scale, 6.0f);
            }
            if (repo_chip_w > 0.0f) {
                l.save_repo_root = D2D1::RectF(x, cy, x + repo_chip_w, cy + ch);
            }
        }
    }

    if (chrome.context_top > 0) {
        float x = Dip(scale, chrome.context_left + 10.0f);
        float end = Dip(scale, chrome.context_right - 6.0f);
        const float y = Dip(scale, chrome.context_top + 6.0f), h = Dip(scale, 28.0f);
        if (chrome.filter_tools_visible) {
            const float tool_w = Dip(scale, 28.0f);
            if (chrome.preview_toggle_visible && end - tool_w >= x + Dip(scale, 36.0f)) {
                l.preview_toggle = D2D1::RectF(end - tool_w, y, end, y + h);
                end = l.preview_toggle.left - Dip(scale, 4.0f);
            }
            if (end - tool_w >= x + Dip(scale, 36.0f)) {
                l.sort_sync = D2D1::RectF(end - tool_w, y, end, y + h);
                end = l.sort_sync.left - Dip(scale, 8.0f);
            }
        }
        const auto place = [&](const std::wstring& text, const wchar_t*, D2D1_RECT_F& rect) {
            if (text.empty()) return;
            const float w = Dip(scale, 8.0f) + Dip(scale, 14.0f) + Dip(scale, 6.0f) + CaptionWidth(painter, scale, text) + Dip(scale, 8.0f);
            if (x + w > end) return;
            rect = D2D1::RectF(x, y + Dip(scale, 1.0f), x + w, y + h - Dip(scale, 1.0f)); x += w + Dip(scale, 8.0f);
        };
        l.new_folder_keycap = false;
        const auto chip_width = [&](const PickerOptionChip& filter) {
            const bool icon = FilterChipIcon(chrome, filter) != StoreIcon::None;
            float w = CaptionWidth(painter, scale, filter.more ? filter.text + L" 99" : filter.text) + Dip(scale, icon ? 39.0f : 20.0f);
            if (filter.count >= 0) w += CaptionWidth(painter, scale, std::to_wstring(filter.count)) + Dip(scale, 5.0f);
            if (filter.drop_down) w += Dip(scale, 13.0f);
            return std::min(Dip(scale, 190.0f), w);
        };
        // The keycap only when the chips that never fold still have room after it.
        // (更多 counts too: it shows as soon as one type chip folds.)
        float chips_need = 0.0f, chips_all = 0.0f;
        const bool may_fold = std::any_of(chrome.context_filters.begin(), chrome.context_filters.end(),
                                          [](const PickerOptionChip& filter) { return filter.hideable; });
        for (const auto& filter : chrome.context_filters) {
            const float w = chip_width(filter) + Dip(scale, filter.gap_before ? 14.0f : 6.0f);
            if (!filter.more) chips_all += w;
            if (!filter.hideable && !filter.fold_late && (!filter.more || may_fold)) chips_need += w;
        }
        const float nf_top = y + Dip(scale, 1.0f), nf_bot = y + h - Dip(scale, 1.0f);
        const float create_w = CompactNewFolderWidth(painter, scale, chrome.new_folder_text, chrome.new_folder_key);
        const float create_bare_w = CompactNewFolderWidth(painter, scale, chrome.new_folder_text, L"");
        const float trailing_need = std::max(Dip(scale, 140.0f), chips_need);
        if (!chrome.new_folder_text.empty() && !chrome.new_folder_key.empty() && x + create_w + trailing_need <= end) {
            l.new_folder_keycap = true;
            l.new_folder = D2D1::RectF(x, nf_top, x + create_w, nf_bot); x += create_w + Dip(scale, 8.0f);
        } else if (!chrome.new_folder_text.empty() &&
                   x + create_bare_w + Dip(scale, 12.0f) + chips_all > end) {
            // Its label goes before any chip folds (the … menu and Ctrl+Shift+N still create).
            const float w = Dip(scale, 30.0f);
            l.new_folder_icon_only = true;
            l.new_folder = D2D1::RectF(x, nf_top, x + w, nf_bot); x += w + Dip(scale, 8.0f);
        } else if (!chrome.new_folder_text.empty()) {
            const float w = create_bare_w;
            if (x + w <= end) { l.new_folder = D2D1::RectF(x, nf_top, x + w, nf_bot); x += w + Dip(scale, 8.0f); }
        }
        if (chrome.auto_jump_notice.empty()) place(chrome.undo_jump_text, L"\xE7A7", l.undo_jump);
        // HTML fchip: 26 DIP pill, 12px text. Type chips fold into 更多 when the row is short.
        const auto& chips = chrome.context_filters;
        const size_t n = chips.size();
        std::vector<float> widths(n), gaps(n);
        int hideable_total = 0;
        for (size_t i = 0; i < n; ++i) {
            const auto& filter = chips[i];
            widths[i] = chip_width(filter);
            gaps[i] = Dip(scale, filter.gap_before && i > 0 ? 14.0f : 6.0f);
            if (filter.hideable) ++hideable_total;
        }
        std::vector<bool> shown(n);
        for (size_t i = 0; i < n; ++i) shown[i] = !chips[i].more;
        const auto need = [&] {
            float total = 0.0f; bool first = true;
            for (size_t i = 0; i < n; ++i) if (shown[i]) { total += (first ? 0.0f : gaps[i]) + widths[i]; first = false; }
            return total;
        };
        if (x + need() > end && hideable_total > 0) {
            for (size_t i = 0; i < n; ++i) if (chips[i].more) shown[i] = true;
            for (size_t i = n; i-- > 0 && x + need() > end;)
                if (chips[i].hideable && shown[i]) { shown[i] = false; ++l.context_folded; }
        }
        // Still short: fold an unused time chip first, then give the widest
        // remaining chip an ellipsis so primary chips stay readable.
        for (size_t i = n; i-- > 0 && x + need() > end;)
            if (chips[i].fold_late && !chips[i].checked && shown[i]) shown[i] = false;
        if (const float over = x + need() - end; over > 0.0f) {
            size_t widest = n;
            for (size_t i = 0; i < n; ++i)
                if (shown[i] && !chips[i].more && (widest == n || widths[i] > widths[widest])) widest = i;
            if (widest < n) widths[widest] = std::max(Dip(scale, 80.0f), widths[widest] - over);
        }
        bool first = true;
        for (size_t i = 0; i < n; ++i) {
            const float start = x + (first ? 0.0f : gaps[i]);
            if (!shown[i] || start + widths[i] > end) {
                if (shown[i] && chips[i].hideable) ++l.context_folded;
                l.context_filters.push_back(D2D1::RectF());
                continue;
            }
            l.context_filters.push_back(D2D1::RectF(start, y + Dip(scale, 1.0f), start + widths[i], y + h - Dip(scale, 1.0f)));
            x = start + widths[i];
            first = false;
        }
    }
    if (!chrome.pulse_selection.empty() && l.strip.bottom > l.strip.top) {
        l.pulse_band = D2D1::RectF(l.strip.left, l.strip.bottom, l.strip.right,
            l.strip.bottom + Dip(scale, kPickerPulseSelectionDip));
        const float strip_x = l.pulse_band.left + Dip(scale, chrome.strip_left > 0.0f ? 12.0f : kSidePad);
        const float card_top = l.pulse_band.top + Dip(scale, 2.0f);
        const float card_bottom = card_top + Dip(scale, 34.0f);
        const float card_mid = (card_top + card_bottom) * 0.5f;
        const float logo_sz = Dip(scale, 18.0f);
        l.pulse_logo = D2D1::RectF(strip_x, card_mid - logo_sz * 0.5f, strip_x + logo_sz, card_mid + logo_sz * 0.5f);

        const float card_left = l.pulse_logo.right + Dip(scale, 8.0f);
        const float card_max_right = l.pulse_band.right - Dip(scale, 12.0f);
        const float inner_y = card_top + Dip(scale, 4.0f);
        const float inner_h = Dip(scale, 26.0f);

        // Compact .btn.s action button: 26 DIP tall, 12px caption label + compact Alt 0 keycap.
        const float keycap_w = CaptionWidth(painter, scale, L"Alt 0") + Dip(scale, 10.0f);
        const float action_w = Dip(scale, 9.0f) + CaptionWidth(painter, scale, chrome.pulse_use_text) +
            Dip(scale, 6.0f) + keycap_w + Dip(scale, 9.0f);
        const float use_gap = Dip(scale, 6.0f);
        const float card_pad_r = Dip(scale, 4.0f);
        const float content_limit = std::max(card_left + Dip(scale, 10.0f), card_max_right - card_pad_r - action_w - use_gap);

        const auto pulse_chip_w = [&](const std::wstring& name) {
            const float label_w = (CaptionWidth(painter, scale, name) + BodyWidth(painter, scale, name)) * 0.5f + Dip(scale, 2.0f);
            return std::min(Dip(scale, 196.0f), Dip(scale, 4.0f) + Dip(scale, 18.0f) + Dip(scale, 6.0f) + label_w + Dip(scale, 8.0f));
        };
        const auto pulse_more_w = [&](size_t hidden_count) {
            const auto text = l10n::Get(l10n::StringId::More) + L" " + std::to_wstring(hidden_count);
            return CaptionWidth(painter, scale, text) + Dip(scale, 28.0f);
        };

        float x = card_left + Dip(scale, 10.0f);
        const float base_cap_w = CaptionWidth(painter, scale, chrome.pulse_caption);
        const std::wstring folder_suffix = chrome.pulse_folder.empty() ? L"" : (L" \x00B7 " + chrome.pulse_folder);
        const float folder_w = folder_suffix.empty() ? 0.0f : CaptionWidth(painter, scale, folder_suffix);
        const float first_w = pulse_chip_w(chrome.pulse_selection[0].text);
        const float first_need = first_w + (chrome.pulse_selection.size() > 1 ? Dip(scale, 4.0f) + pulse_more_w(chrome.pulse_selection.size() - 1) : 0.0f);
        const float avail_inner = std::max(0.0f, content_limit - x);
        const bool show_folder = !folder_suffix.empty() &&
            (base_cap_w + folder_w + Dip(scale, 8.0f) + first_need <= avail_inner);
        l.pulse_caption_show_folder = show_folder;
        const float all_more_w = pulse_more_w(chrome.pulse_selection.size());
        const float caption_max = std::max(0.0f, std::min(avail_inner - all_more_w - Dip(scale, 8.0f), avail_inner * 0.62f));
        const float caption_w = show_folder
            ? (base_cap_w + folder_w)
            : std::min(base_cap_w, caption_max);
        l.pulse_caption = D2D1::RectF(x, inner_y, x + caption_w, inner_y + inner_h);
        x = l.pulse_caption.right + (caption_w > 0.0f ? Dip(scale, 8.0f) : 0.0f);

        l.pulse_items.resize(chrome.pulse_selection.size());
        size_t shown = 0;
        for (; shown < chrome.pulse_selection.size(); ++shown) {
            const float reserve = shown + 1 < chrome.pulse_selection.size()
                ? Dip(scale, 4.0f) + pulse_more_w(chrome.pulse_selection.size() - shown - 1) : 0.0f;
            const float w = pulse_chip_w(chrome.pulse_selection[shown].text);
            if (x + w + reserve > content_limit) break;
            l.pulse_items[shown] = D2D1::RectF(x, inner_y, x + w, inner_y + inner_h);
            x += w + Dip(scale, 4.0f);
        }
        float content_end = shown > 0 ? l.pulse_items[shown - 1].right : l.pulse_caption.right;
        if (shown < chrome.pulse_selection.size()) {
            const size_t hidden = chrome.pulse_selection.size() - shown;
            l.pulse_more_text = l10n::Get(l10n::StringId::More) + L" " + std::to_wstring(hidden);
            const float w = pulse_more_w(hidden);
            l.pulse_more = D2D1::RectF(x, inner_y, std::min(content_limit, x + w), inner_y + inner_h);
            content_end = l.pulse_more.right;
        }
        const float use_left = std::min(card_max_right - card_pad_r - action_w,
            std::max(card_left + Dip(scale, 4.0f), content_end + use_gap));
        l.pulse_use = D2D1::RectF(use_left, inner_y, use_left + action_w, inner_y + inner_h);
        l.pulse_card = D2D1::RectF(card_left, card_top,
            std::min(card_max_right, l.pulse_use.right + card_pad_r), card_bottom);
    }
    if (!chrome.auto_jump_notice.empty() && chrome.band_top > 0) {
        const float notice_right = chrome.strip_right > 0 ? std::min(width, Dip(scale, chrome.strip_right)) : width;
        l.auto_jump_notice = D2D1::RectF(Dip(scale, chrome.band_left + 14.0f), Dip(scale, chrome.band_top),
            notice_right - Dip(scale, 16.0f), Dip(scale, chrome.band_top + kPickerAutoJumpNoticeDip));
        l.undo_jump = painter.LayoutInfoBar(AutoJumpInfo(chrome, l.auto_jump_notice)).actions[0];
    }
    {
        const float fy = l.footer.top + Dip(scale, 10.0f);
        const float fh = Dip(scale, kFieldH);
        const float label_w = std::min(MediumWidth(painter, scale, chrome.filename_label) + Dip(scale, 4.0f), std::max(0.0f, (right - left) * 0.2f));
        l.filename_label = D2D1::RectF(left, fy, left + std::max(0.0f, label_w), fy + fh);
        // The type list keeps a readable width; the name takes the rest.
        const float field_left = l.filename_label.right + Dip(scale, 8.0f);
        const float field_space = std::max(0.0f, right - field_left - Dip(scale, 8.0f));
        const float filter_w = std::min(Dip(scale, 264.0f), field_space * 0.45f);
        if (format_row) {
            // Keep the caller's order and full labels. Remove lower-priority segments
            // until More fits; never squeeze the name field or leave invisible tab stops.
            std::vector<float> widths(chrome.formats.size());
            std::vector<size_t> visible;
            size_t supplied = 0;
            float total = 0.0f;
            for (size_t i = 0; i < chrome.formats.size(); ++i) {
                if (chrome.formats[i].filter < 0) continue;
                ++supplied;
                widths[i] = BodyWidth(painter, scale, chrome.filter_static ? chrome.format_static_text : chrome.formats[i].text) + Dip(scale, 24.0f);
                visible.push_back(i);
                total += widths[i];
            }
            const size_t count = std::max(supplied, chrome.format_count);
            const float name_min = std::min(Dip(scale, 160.0f), field_space * 0.55f);
            const float available = std::max(0.0f, std::min(field_space * 0.5f, field_space - name_min));
            float more_width = 0.0f;
            for (;;) {
                const size_t hidden = count - visible.size();
                l.format_more_text = hidden ? l10n::Get(l10n::StringId::More) + L" " + std::to_wstring(hidden) : L"";
                more_width = hidden ? painter.MeasureButtonWidth(l.format_more_text, {}, true) : 0.0f;
                if (total + more_width <= available || visible.empty()) break;
                auto remove = visible.end();
                for (auto it = visible.end(); it != visible.begin();) {
                    --it;
                    if (static_cast<int>(*it) != chrome.format_selected && !chrome.formats[*it].any) { remove = it; break; }
                }
                if (remove == visible.end()) {
                    for (auto it = visible.end(); it != visible.begin();) {
                        --it;
                        if (static_cast<int>(*it) != chrome.format_selected) { remove = it; break; }
                    }
                }
                if (remove == visible.end()) remove = std::prev(visible.end());
                total -= widths[*remove];
                visible.erase(remove);
            }
            // When the selected segment plus More would crowd the filename,
            // use one existing dropdown showing the current format, not an extra row.
            if (visible.empty() || (chrome.format_selected >= 0 &&
                std::find(visible.begin(), visible.end(), static_cast<size_t>(chrome.format_selected)) == visible.end())) {
                l.format_compact = true;
                visible.clear(); total = 0.0f;
                const size_t selected = chrome.format_selected >= 0 ? static_cast<size_t>(chrome.format_selected) : 0;
                l.format_more_text = chrome.filter_static ? chrome.format_static_text : selected < chrome.formats.size() ? chrome.formats[selected].text : chrome.filter_text;
                more_width = std::min(available, painter.MeasureButtonWidth(l.format_more_text, {}, !chrome.filter_static));
            }
            const float y = fy + Dip(scale, 1.0f);
            const float h = Dip(scale, 32.0f);
            l.format_segments.resize(chrome.formats.size());
            const float formats_left = right - total - more_width;
            float x = formats_left;
            const auto place_more = [&] {
                if (more_width <= 0.0f || l.format_more.right > l.format_more.left) return;
                l.format_more = D2D1::RectF(x, y, std::min(right, x + more_width), y + h);
                x = l.format_more.right;
            };
            for (const auto i : visible) {
                if (chrome.formats[i].any) place_more();
                l.format_segments[i] = D2D1::RectF(x, y, x + widths[i], y + h);
                x += widths[i];
            }
            place_more();
            l.formats = D2D1::RectF(formats_left, y, x, y + h);
        }
        if (chrome.mode != PickerMode::Folder && !format_row) l.filter = D2D1::RectF(right - filter_w, fy, right, fy + fh);
        const float types_left = l.filter.left;
        l.filename = D2D1::RectF(l.filename_label.right + Dip(scale, 8.0f), fy,
                                 chrome.mode == PickerMode::Folder ? right : format_row ? l.formats.left - Dip(scale, 8.0f) : types_left - Dip(scale, 8.0f), fy + fh);
    }
    if (!chrome.options.empty()) {
        const float y = l.filename.bottom + Dip(scale, 8.0f + format_height);
        l.options = D2D1::RectF(l.filename.left, y, right, y + Dip(scale, 30.0f));
        const float more_w = std::min(right - l.options.left, painter.MeasureButtonWidth(chrome.more_options_text, {}, true) + painter.MeasureBadgeWidth(std::to_wstring(chrome.options.size())) + Dip(scale, 6.0f));
        float x = l.options.left;
        for (size_t i = 0; i < chrome.options.size() && i < 4; ++i) {
            const auto& option = chrome.options[i];
            if (option.property) {  // label segment | value segment, one field
                const float label_w = CaptionWidth(painter, scale, option.label) + Dip(scale, 16.0f);
                const float value_w = std::min(Dip(scale, 200.0f), BodyWidth(painter, scale, option.text)) +
                    Dip(scale, option.add ? 34.0f : 18.0f);
                const float reserve = i + 1 < chrome.options.size() ? more_w + Dip(scale, 8.0f) : 0.0f;
                if (x + label_w + value_w > right - reserve) break;
                l.option_chips.push_back(D2D1::RectF(x, y, x + label_w + value_w, y + Dip(scale, 28.0f)));
                l.option_labels.push_back(D2D1::RectF(x, y, x + label_w, y + Dip(scale, 28.0f)));
                l.option_buttons.push_back(D2D1::RectF(x + label_w, y, x + label_w + value_w, y + Dip(scale, 28.0f)));
                x += label_w + value_w + Dip(scale, 8.0f);
                continue;
            }
            const bool separate_label = option.drop_down && !option.checkable && !option.label.empty();
            const float label_w = separate_label ? BodyWidth(painter, scale, option.label) + Dip(scale, 8.0f) : 0.0f;
            const auto text = separate_label || option.label.empty() ? option.text : option.label + L" " + option.text;
            float button_w = painter.MeasureButtonWidth(text, {}, option.drop_down);
            if (separate_label) button_w = std::max(button_w, Dip(scale, 132.0f));  // HTML 编码 box: min 132
            const float w = label_w + button_w + Dip(scale, option.checkable ? 28.0f : 0.0f);
            const float reserve = i + 1 < chrome.options.size() ? more_w + Dip(scale, 8.0f) : 0.0f;
            if (x + w > right - reserve) break;
            l.option_chips.push_back(D2D1::RectF(x, y, x + w, y + Dip(scale, 28.0f)));
            l.option_labels.push_back(D2D1::RectF(x, y, x + std::max(0.0f, label_w - Dip(scale, 8.0f)), y + Dip(scale, 28.0f)));
            l.option_buttons.push_back(D2D1::RectF(x + label_w, y, x + w, y + Dip(scale, 28.0f)));
            x += w + Dip(scale, 8.0f);
        }
        if (l.option_chips.size() < chrome.options.size()) {
            l.more_options = D2D1::RectF(x, y, std::min(right, x + more_w), y + Dip(scale, 28.0f));
            const float badge_w = painter.MeasureBadgeWidth(std::to_wstring(chrome.options.size() - l.option_chips.size()));
            const float badge_right = l.more_options.right - Dip(scale, 26.0f);
            l.more_options_badge = D2D1::RectF(badge_right - badge_w, y + Dip(scale, 5.0f), badge_right, y + Dip(scale, 23.0f));
            l.more_options_label = D2D1::RectF(x + Dip(scale, 8.0f), y,
                std::max(x + Dip(scale, 8.0f), l.more_options_badge.left - Dip(scale, 6.0f)), y + Dip(scale, 28.0f));
        }
    }
    if (!chrome.selected_names.empty()) {
        const float clear_width = painter.MeasureButtonWidth(l10n::Get(l10n::StringId::ClearAll));
        l.selection_clear = D2D1::RectF(std::max(l.filename.left, l.filename.right - clear_width),
            l.filename.top, l.filename.right, l.filename.bottom);
        float x = l.filename.left + Dip(scale, 4.0f);
        const float end = l.selection_clear.left - Dip(scale, 6.0f);
        const float more_width = painter.MeasureButtonWidth(l10n::Get(l10n::StringId::More) + L" 1024", {}, true);
        for (size_t i = 0; i < chrome.selected_names.size(); ++i) {
            const float reserve = i + 1 < chrome.selected_names.size() ? more_width + Dip(scale, 4.0f) : 0;
            const float available = end - x - reserve;
            if (available < Dip(scale, 90.0f)) break;
            const auto source = i < chrome.selected_sources.size() ? chrome.selected_sources[i] : std::wstring();
            const bool tray = i < chrome.selected_from_tray.size() && chrome.selected_from_tray[i];
            const float desired = painter.MeasureButtonWidth(chrome.selected_names[i]) +
                std::min(Dip(scale, 100.0f), BodyWidth(painter, scale, source) + Dip(scale, tray ? 22.0f : 0.0f)) + Dip(scale, 52.0f);
            const float tag_width = std::min(available, std::clamp(desired, Dip(scale, 90.0f), Dip(scale, 320.0f)));
            l.selection_tags.push_back(D2D1::RectF(x, l.filename.top + Dip(scale, 4.0f),
                x + tag_width, l.filename.bottom - Dip(scale, 4.0f)));
            x += tag_width + Dip(scale, 4.0f);
        }
        if (l.selection_tags.size() < chrome.selected_names.size()) {
            l.selection_more_text = l10n::Get(l10n::StringId::More) + L" " +
                std::to_wstring(chrome.selected_names.size() - l.selection_tags.size());
            l.selection_more = D2D1::RectF(x, l.filename.top, std::max(x, std::min(end,
                x + painter.MeasureButtonWidth(l.selection_more_text, {}, true))), l.filename.bottom);
        }
    }
    if (suggestion_row) {
        const float y = l.footer.top + Dip(scale, 52.0f + format_height + (chrome.options.empty() ? 0.0f : 38.0f));
        if (!chrome.save_suggestions.empty()) l.suggestions_caption = D2D1::RectF(left, y, l.filename.left - Dip(scale, 8.0f), y + Dip(scale, 30.0f));
        if (!chrome.save_suggestions.empty()) l.suggestions = D2D1::RectF(l.filename.left, y, right, y + Dip(scale, 30.0f));
        l.suggestion_items.resize(chrome.save_suggestions.size());
        const float more_w = painter.MeasureButtonWidth(l10n::Get(l10n::StringId::More), {}, true);
        const float suggestions_right = right;
        float x = l.filename.left;
        for (size_t i = 0; i < chrome.save_suggestions.size(); ++i) {
            const float suggestion_width = std::min(Dip(scale, 240.0f), painter.MeasureButtonWidth(chrome.save_suggestions[i].text) + Dip(scale, 12.0f));
            const float reserve = i + 1 < chrome.save_suggestions.size() ? more_w + Dip(scale, 6.0f) : 0;
            if (x + suggestion_width + reserve > suggestions_right) {
                l.suggestions_more = D2D1::RectF(x, y, std::min(suggestions_right, x + more_w), l.suggestions.bottom); break;
            }
            l.suggestion_items[i] = D2D1::RectF(x, y, x + suggestion_width, l.suggestions.bottom);
            x += suggestion_width + Dip(scale, 6.0f);
        }
        // Use spare space, not a reserved quota: adding a quiet explanation must
        // not hide naming actions that already fit. The type tooltip keeps it complete.
        const float note_left = chrome.save_suggestions.empty() ? l.filename.left :
            (l.suggestions_more.right > l.suggestions_more.left ? l.suggestions_more.right : x) + Dip(scale, 12.0f);
        const float note_width = right - note_left;
        if (!chrome.format_note.empty() && note_width >= Dip(scale, 120.0f))
            l.format_note = D2D1::RectF(std::max(note_left, right - Dip(scale, 360.0f)), y, right, y + Dip(scale, 30.0f));
    }
    return l;
}

int HitTestPickerChrome(const FolderPickerChromeLayout& l, const FolderPickerChrome& c,
                        float x, float y) {
    if ((l.format_note.right > l.format_note.left && ContainsRect(l.format_note, x, y)) ||
        (c.filter_static && l.formats.right > l.formats.left && ContainsRect(l.formats, x, y))) return kPickFormatNote;
    if (c.filter_tools_visible && l.sort_sync.right > l.sort_sync.left && ContainsRect(l.sort_sync, x, y)) return kPickSortSync;
    if (c.filter_tools_visible && c.preview_toggle_visible && l.preview_toggle.right > l.preview_toggle.left && ContainsRect(l.preview_toggle, x, y)) return kPickPreviewToggle;
    for (size_t i = 0; i < l.context_filters.size() && i < c.context_filters.size(); ++i)
        if (l.context_filters[i].right > l.context_filters[i].left && ContainsRect(l.context_filters[i], x, y)) return c.context_filters[i].id;
    if (c.overwrite_pending && ContainsRect(l.overwrite_replace, x, y)) return kPickOverwriteReplace;
    if (c.overwrite_pending && ContainsRect(l.overwrite_back, x, y)) return kPickOverwriteBack;
    if (c.overwrite_pending && ContainsRect(l.rename, x, y)) return kPickRename;
    if (c.save_actions_enabled) {
        if (ContainsRect(l.live_rename, x, y)) return kPickLiveRename;
        if (ContainsRect(l.live_save, x, y)) return kPickLiveSave;
        if (ContainsRect(l.suggestions_more, x, y)) return kPickSaveSuggestionsMore;
        for (size_t i = 0; i < l.suggestion_items.size(); ++i)
            if (ContainsRect(l.suggestion_items[i], x, y)) return kPickSaveSuggestionBase + static_cast<int>(i);
    }
    if (ContainsRect(l.close, x, y)) return kPickClose;
    if (c.pulse_selection_enabled && ContainsRect(l.pulse_use, x, y)) return kPickPulseUse;
    if (c.pulse_selection_enabled && ContainsRect(l.pulse_more, x, y)) return kPickPulseMore;
    for (size_t i = 0; i < l.pulse_items.size(); ++i)
        if (c.pulse_selection_enabled && c.pulse_selection[i].enabled && ContainsRect(l.pulse_items[i], x, y))
            return kPickPulseFileBase + static_cast<int>(i);
    if (ContainsRect(l.more_options, x, y)) return kPickMoreOptions;
    for (size_t i = 0; i < l.option_chips.size(); ++i)
        if (ContainsRect(l.option_chips[i], x, y)) return c.options[i].enabled ? c.options[i].id : kPickNone;
    if (ContainsRect(l.open_scroll_left, x, y)) return c.open_scroll > 0.5f ? kPickOpenScrollLeft : kPickNone;
    if (ContainsRect(l.open_scroll_right, x, y)) return c.open_scroll < l.open_scroll_max - 0.5f ? kPickOpenScrollRight : kPickNone;
    if (ContainsRect(l.open_viewport, x, y)) {
        for (size_t i = 0; i < l.open_chips.size(); ++i)
            if (ContainsRect(l.open_chips[i], x, y)) return kPickOpenFolderBase + static_cast<int>(i);
        if (ContainsRect(l.last_location, x, y)) return kPickLastLocation;
        if (ContainsRect(l.save_origin, x, y)) return kPickSaveOrigin;
        if (ContainsRect(l.save_repo_root, x, y)) return kPickSaveRepoRoot;
    }
    if (!c.undo_jump_text.empty() && ContainsRect(l.undo_jump, x, y)) return kPickUndoJump;
    if (!c.new_folder_text.empty() && ContainsRect(l.new_folder, x, y)) return kPickNewFolder;
    if (c.show_tray_remove && l.tray_remove.right > l.tray_remove.left && ContainsRect(l.tray_remove, x, y)) return kPickTrayRemoveAfterOpen;
    if (c.footer_tool.id != 0 && ContainsRect(l.footer_tool, x, y)) return c.footer_tool.enabled ? c.footer_tool.id : kPickNone;
    if (ContainsRect(l.primary, x, y)) return c.primary_enabled ? (c.primary_drop_down && x >= l.primary.right - 28 * l.scale ? kPickPrimaryDropDown : kPickPrimary) : kPickNone;
    if (ContainsRect(l.cancel, x, y)) return kPickCancel;
    if (!c.selected_names.empty() && ContainsRect(l.selection_clear, x, y)) return kPickClearSelection;
    if (!l.selection_more_text.empty() && ContainsRect(l.selection_more, x, y)) return kPickSelectionMore;
    for (size_t i = 0; i < l.selection_tags.size(); ++i) {
        auto remove = l.selection_tags[i];
        remove.left = remove.right - Dip(l.scale, 24.0f);
        if (ContainsRect(remove, x, y)) return kPickRemoveSelectionBase + static_cast<int>(i);
    }
    if (ContainsRect(l.filename, x, y) || ContainsRect(l.filename_label, x, y)) return kPickFilename;
    if (!c.filter_static && ContainsRect(l.filter, x, y)) return kPickFilter;
    if (!c.filter_static && ContainsRect(l.format_more, x, y)) return kPickFilter;
    for (size_t i = 0; i < l.format_segments.size(); ++i)
        if (!c.filter_static && ContainsRect(l.format_segments[i], x, y)) return kPickFormatBase + static_cast<int>(i);
    return kPickNone;
}

void DrawPickerChrome(Compositor& compositor, fluent::Painter& painter, const Theme& theme,
                      const FolderPickerChrome& c, const FolderPickerChromeLayout& l,
                      bool high_contrast) {
    const float scale = l.scale;
    const float radius = Dip(scale, theme.radius_control);

    if (high_contrast || (c.frame_title_alpha >= 1.0f && c.frame_sheet_alpha >= 1.0f)) {
        painter.FillRoundedRect(l.title, 0, theme.surface_title);
        painter.FillRoundedRect(l.footer, 0, theme.surface_sheet);
    } else {
        // Translucent frame: the title layer over everything, the sheet below the title.
        const auto right_of = [&](D2D1_RECT_F r) { r.left = std::max(r.left, c.frame_left); return r; };
        const auto title = right_of(l.title), footer = right_of(l.footer);
        if (title.right > title.left) painter.FillRoundedRect(title, 0, WithAlpha(theme.surface_title, c.frame_title_alpha));
        if (footer.right > footer.left) {
            painter.FillRoundedRect(footer, 0, WithAlpha(theme.surface_title, c.frame_title_alpha));
            painter.FillRoundedRect(footer, 0, WithAlpha(theme.surface_sheet, c.frame_sheet_alpha));
        }
    }

    // Title strip: the program that asked (HTML 标题栏), then what it asked for.
    auto* dc = compositor.Dc();
    const float mid = l.title.bottom * 0.5f;
    if (!c.app_name.empty()) {
        const auto badge = D2D1::RectF(Dip(scale, 16.0f), mid - Dip(scale, 10.0f), Dip(scale, 36.0f), mid + Dip(scale, 10.0f));
        if (c.app_icon) {
            // Whole pixels: the worker extracted the icon at this size, so the
            // usual case copies 1:1; other DPIs resample with a cubic filter.
            const float edge = std::round(Dip(scale, 20.0f));
            const float x = std::round(badge.left), y = std::round((badge.top + badge.bottom - edge) * 0.5f);
            const auto target = D2D1::RectF(x, y, x + edge, y + edge);
            dc->DrawBitmap(c.app_icon, &target, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        }
        else {
            painter.FillRoundedRect(badge, Dip(scale, 5.0f), WithAlpha(theme.text, 0.10f));
            DrawStoreIcon(dc, StoreIcon::Square, D2D1::RectF(badge.left + Dip(scale, 3.0f), badge.top + Dip(scale, 3.0f),
                          badge.right - Dip(scale, 3.0f), badge.bottom - Dip(scale, 3.0f)), theme.text_secondary, 1.3f);
        }
        const float name_max = (l.close.left - Dip(scale, 60.0f)) * 0.6f;
        const std::wstring name = FitEnd(compositor, compositor.TabActiveFormat(), c.app_name, name_max);
        const float name_w = std::min(name_max, typography::MeasureLine(&compositor, compositor.TabActiveFormat(), name));
        const float name_x = Dip(scale, 45.0f);
        painter.DrawText(name, D2D1::RectF(name_x, 0, name_x + name_w + Dip(scale, 2.0f), l.title.bottom),
                         compositor.TabActiveFormat(), theme.text);
        const float div_x = name_x + name_w + Dip(scale, 10.0f);
        painter.FillRoundedRect(D2D1::RectF(div_x, mid - Dip(scale, 7.0f), div_x + std::max(1.0f, Dip(scale, 1.0f)), mid + Dip(scale, 7.0f)),
                                0, theme.stroke_divider);
        const float verb_x = div_x + Dip(scale, 11.0f);
        painter.DrawText(FitEnd(compositor, compositor.FileNameFormat(), c.title, std::max(0.0f, l.close.left - Dip(scale, 8.0f) - verb_x)),
                         D2D1::RectF(verb_x, 0, l.close.left - Dip(scale, 8.0f), l.title.bottom), compositor.FileNameFormat(), theme.text_secondary);
    } else {
        const StoreIcon icon = c.mode == PickerMode::Image ? StoreIcon::Image
                             : c.mode == PickerMode::File || c.mode == PickerMode::Save ? StoreIcon::File : StoreIcon::Folder;
        DrawStoreIcon(dc, icon, D2D1::RectF(Dip(scale, 17.0f), mid - Dip(scale, 8.0f), Dip(scale, 33.0f), mid + Dip(scale, 8.0f)),
                      high_contrast ? theme.text : theme.accent, 1.4f);
        painter.DrawText(FitEnd(compositor, compositor.TabActiveFormat(), c.title,
                                l.close.left - Dip(scale, 54.0f)),
                         D2D1::RectF(Dip(scale, 46.0f), 0, l.close.left - Dip(scale, 8.0f), l.title.bottom),
                         compositor.TabActiveFormat(), theme.text);
    }
    fluent::ControlState close_state{};
    close_state.hovered = c.hover == kPickClose;
    close_state.pressed = c.pressed == kPickClose;
    painter.DrawTitleBarButton(l.close, fluent::TitleBarButtonRole::Close, {}, close_state);

    // 已打开的文件夹: folders open in Pulse tabs and Explorer windows, Alt+1..9 on the right (HTML 跳转条).
    if (l.strip.bottom > l.strip.top) {
        const float left = l.strip.left + Dip(scale, c.strip_left > 0.0f ? 12.0f : kSidePad);
        const float mid_y = (l.strip.top + l.strip.bottom) * 0.5f;
        const D2D1_COLOR_F muted = theme.text_secondary;
        const D2D1_COLOR_F faint = WithAlpha(theme.text, 0.45f);
        const float cap_w = CaptionWidth(painter, scale, c.strip_caption);
        painter.DrawText(c.strip_caption, D2D1::RectF(left, l.strip.top, left + cap_w + Dip(scale, 4.0f), l.strip.bottom),
                         compositor.SmallFormat(), muted);
        auto chip = [&](const D2D1_RECT_F& r, const PickerOpenChip& item, int key_index, int id, int icon_kind, bool show_source) {
            const bool on = item.active;
            const bool hot = c.hover == id;
            const float chip_pill = (r.bottom - r.top) * 0.5f;
            painter.FillRoundedRect(r, chip_pill, on ? WithAlpha(theme.accent, high_contrast ? 0.0f : 0.10f)
                                                  : hot ? WithAlpha(theme.text, 0.06f) : theme.surface_card);
            painter.StrokeRoundedRect(r, chip_pill, on ? WithAlpha(theme.accent, 0.42f) : theme.stroke_card, std::max(1.0f, Dip(scale, 1.0f)));
            const D2D1_COLOR_F ink = on && !high_contrast ? theme.accent : theme.text;
            float x = r.left + Dip(scale, 10.0f);
            const auto icon_box = D2D1::RectF(x, mid_y - Dip(scale, 6.5f), x + Dip(scale, 13.0f), mid_y + Dip(scale, 6.5f));
            if (icon_kind == 1) DrawCommandShape(dc, command_icons::Icon::History, icon_box, on ? ink : muted, 1.3f);
            else if (icon_kind == 2) painter.DrawGlyph(L"\xE8B7", icon_box, on ? ink : (high_contrast ? theme.text : theme.accent));
            else if (icon_kind == 3) painter.DrawGlyph(L"\xE943", icon_box, on ? ink : muted);
            else if (item.window) DrawWindowShape(painter, icon_box, on ? ink : muted, 1.3f);
            else DrawStoreIcon(dc, StoreIcon::Folder, icon_box, on ? ink : muted, 1.3f);
            x += Dip(scale, kChipIcon + 5.0f);
            float right_limit = r.right - Dip(scale, 6.0f);
            if (key_index >= 0) {
                const std::wstring key = OpenFolderKey(key_index);
                const float kw = KeyWidth(painter, scale, key_index);
                const auto kr = D2D1::RectF(right_limit - kw, mid_y - Dip(scale, 9.0f), right_limit, mid_y + Dip(scale, 9.0f));
                DrawPickerKeycap(compositor, dc, painter, theme, scale, kr, key, high_contrast);
                right_limit = kr.left - Dip(scale, 7.0f);
            } else {
                right_limit -= Dip(scale, 4.0f);
            }
            const float source_w = item.source.empty() || !show_source ? 0.0f : CaptionWidth(painter, scale, item.source);
            const float label_w = std::max(0.0f, right_limit - (source_w > 0 ? source_w + Dip(scale, 7.0f) : 0.0f) - x);
            painter.DrawText(FitEnd(compositor, compositor.FileNameFormat(), item.label, label_w), D2D1::RectF(x, r.top, x + label_w + Dip(scale, 2.0f), r.bottom),
                             compositor.FileNameFormat(), ink);
            if (source_w > 0)
                painter.DrawText(item.source, D2D1::RectF(right_limit - source_w, r.top, right_limit + Dip(scale, 2.0f), r.bottom),
                                 compositor.SmallFormat(), faint);
        };
        auto draw_scroll_btn = [&](const D2D1_RECT_F& r, int id, StoreIcon icon, bool enabled) {
            if (r.right <= r.left || r.bottom <= r.top) return;
            auto st = StateFor(c, id, enabled);
            if (enabled && (st.hovered || st.pressed)) {
                painter.FillRoundedRect(r, Dip(scale, 6.0f), WithAlpha(theme.text, st.pressed ? 0.11f : 0.06f));
            }
            const float cx = (r.left + r.right) * 0.5f;
            const float cy_b = (r.top + r.bottom) * 0.5f;
            const float half = Dip(scale, 6.0f);
            const D2D1_COLOR_F ink = !enabled ? WithAlpha(theme.text, 0.24f)
                : (st.hovered || st.pressed ? theme.text : theme.text_secondary);
            DrawStoreIcon(dc, icon, D2D1::RectF(cx - half, cy_b - half, cx + half, cy_b + half), ink, 1.4f);
        };
        draw_scroll_btn(l.open_scroll_left, kPickOpenScrollLeft, StoreIcon::ChevronLeft, c.open_scroll > 0.5f);
        const bool has_vp = l.open_viewport.right > l.open_viewport.left && l.open_viewport.bottom > l.open_viewport.top;
        if (has_vp) {
            dc->PushAxisAlignedClip(l.open_viewport, D2D1_ANTIALIAS_MODE_ALIASED);
            for (size_t i = 0; i < l.open_chips.size() && i < c.open_folders.size(); ++i) {
                const auto& r = l.open_chips[i];
                if (r.right <= l.open_viewport.left || r.left >= l.open_viewport.right) continue;
                chip(r, c.open_folders[i], i < 9 ? static_cast<int>(i) : -1,
                     kPickOpenFolderBase + static_cast<int>(i), 0,
                     i < l.open_chip_source.size() && l.open_chip_source[i]);
            }
            const int last_key = l.last_location_keycap.right > l.last_location_keycap.left ? static_cast<int>(l.open_chips.size()) : -1;
            if (l.last_location.right > l.last_location.left &&
                l.last_location.right > l.open_viewport.left && l.last_location.left < l.open_viewport.right) {
                if (!l.open_chips.empty()) {
                    const float vx = l.last_location.left - Dip(scale, 6.0f);
                    painter.FillRoundedRect(D2D1::RectF(vx, mid_y - Dip(scale, 9.0f), vx + std::max(1.0f, Dip(scale, 1.0f)), mid_y + Dip(scale, 9.0f)), 0, theme.stroke_divider);
                }
                chip(l.last_location, c.last_location, last_key, kPickLastLocation, 1, true);
            }
            const bool has_p2 = l.save_origin.right > l.save_origin.left || l.save_repo_root.right > l.save_repo_root.left;
            if (has_p2 && (!l.open_chips.empty() || l.last_location.right > l.last_location.left)) {
                const float first_p2_left = l.save_origin.right > l.save_origin.left ? l.save_origin.left : l.save_repo_root.left;
                if (first_p2_left > l.open_viewport.left && first_p2_left < l.open_viewport.right) {
                    const float vx = first_p2_left - Dip(scale, 6.0f);
                    painter.FillRoundedRect(D2D1::RectF(vx, mid_y - Dip(scale, 9.0f), vx + std::max(1.0f, Dip(scale, 1.0f)), mid_y + Dip(scale, 9.0f)), 0, theme.stroke_divider);
                }
            }
            if (l.save_origin.right > l.save_origin.left &&
                l.save_origin.right > l.open_viewport.left && l.save_origin.left < l.open_viewport.right) {
                chip(l.save_origin, c.save_original_location, -1, kPickSaveOrigin, 2, true);
            }
            if (l.save_repo_root.right > l.save_repo_root.left &&
                l.save_repo_root.right > l.open_viewport.left && l.save_repo_root.left < l.open_viewport.right) {
                chip(l.save_repo_root, c.save_repo_root, -1, kPickSaveRepoRoot, 3, true);
            }
            dc->PopAxisAlignedClip();
        }
        draw_scroll_btn(l.open_scroll_right, kPickOpenScrollRight, StoreIcon::ChevronRight, c.open_scroll < l.open_scroll_max - 0.5f);
        if (!c.undo_jump_text.empty() && l.undo_jump.right > l.undo_jump.left && l.auto_jump_notice.right <= l.auto_jump_notice.left) {
            const auto ust = StateFor(c, kPickUndoJump);
            if (ust.enabled && (ust.hovered || ust.pressed))
                painter.FillRoundedRect(l.undo_jump, Dip(scale, 6.0f), ust.pressed ? theme.fill_pressed : theme.fill_hover);
            if (ust.keyboard_focus) painter.DrawFocusRing(l.undo_jump, Dip(scale, 6.0f));
            const float u_cy = (l.undo_jump.top + l.undo_jump.bottom) * 0.5f;
            const auto u_icon = D2D1::RectF(l.undo_jump.left + Dip(scale, 8.0f), u_cy - Dip(scale, 6.5f),
                                            l.undo_jump.left + Dip(scale, 21.0f), u_cy + Dip(scale, 6.5f));
            DrawStoreIcon(dc, StoreIcon::Undo, u_icon, theme.text_secondary, 1.3f);
            painter.DrawText(FitEnd(compositor, compositor.SmallFormat(), c.undo_jump_text,
                    std::max(0.0f, l.undo_jump.right - Dip(scale, 8.0f) - (u_icon.right + Dip(scale, 5.0f)))),
                D2D1::RectF(u_icon.right + Dip(scale, 5.0f), l.undo_jump.top, l.undo_jump.right - Dip(scale, 8.0f), l.undo_jump.bottom),
                compositor.SmallFormat(), theme.text_secondary);
        }
        if (!c.new_folder_text.empty() && l.new_folder.right > l.new_folder.left) {
            const auto nst = StateFor(c, kPickNewFolder);
            const float nr = Dip(scale, 6.0f);
            const D2D1_COLOR_F nbg = high_contrast ? theme.bg
                : nst.pressed ? theme.fill_pressed : nst.hovered ? theme.fill_hover : theme.surface_card;
            painter.FillRoundedRect(l.new_folder, nr, nbg);
            painter.StrokeRoundedRect(l.new_folder, nr, high_contrast ? theme.text : theme.stroke_card, std::max(1.0f, Dip(scale, 1.0f)));
            if (nst.keyboard_focus) painter.DrawFocusRing(l.new_folder, nr);
            const float n_cy = (l.new_folder.top + l.new_folder.bottom) * 0.5f;
            const float icon_sz = Dip(scale, 14.0f);
            if (l.new_folder_icon_only) {
                const float cx = (l.new_folder.left + l.new_folder.right) * 0.5f;
                DrawStoreIcon(dc, StoreIcon::NewFolder,
                    D2D1::RectF(cx - icon_sz * 0.5f, n_cy - icon_sz * 0.5f, cx + icon_sz * 0.5f, n_cy + icon_sz * 0.5f),
                    high_contrast ? theme.text : theme.accent, 1.3f);
            } else {
                const float ix = l.new_folder.left + Dip(scale, 8.0f);
                DrawStoreIcon(dc, StoreIcon::NewFolder,
                    D2D1::RectF(ix, n_cy - icon_sz * 0.5f, ix + icon_sz, n_cy + icon_sz * 0.5f),
                    high_contrast ? theme.text : theme.accent, 1.3f);
                float text_r = l.new_folder.right - Dip(scale, 8.0f);
                if (l.new_folder_keycap && !c.new_folder_key.empty()) {
                    const float kw = KeycapWidth(painter, scale, c.new_folder_key);
                    const auto kr = D2D1::RectF(text_r - kw, n_cy - Dip(scale, 9.0f), text_r, n_cy + Dip(scale, 9.0f));
                    DrawPickerKeycap(compositor, dc, painter, theme, scale, kr, c.new_folder_key, high_contrast);
                    text_r = kr.left - Dip(scale, 6.0f);
                }
                const float tx = ix + icon_sz + Dip(scale, 6.0f);
                painter.DrawText(FitEnd(compositor, compositor.SmallFormat(), c.new_folder_text, std::max(0.0f, text_r - tx)),
                    D2D1::RectF(tx, l.new_folder.top, text_r, l.new_folder.bottom),
                    compositor.SmallFormat(), theme.text);
            }
        }
        if (l.pulse_band.right <= l.pulse_band.left) {
            painter.FillRoundedRect(D2D1::RectF(l.strip.left, l.strip.bottom - 1.0f, l.width, l.strip.bottom), 0, theme.stroke_divider);
        }
    }

    if (l.pulse_band.right > l.pulse_band.left) {
        if (high_contrast || (c.frame_title_alpha >= 1.0f && c.frame_sheet_alpha >= 1.0f)) {
            painter.FillRoundedRect(l.pulse_band, 0, theme.surface_sheet);
        }
        // Real Pulse logo bitmap (.pband .logo, 18x18 DIP) outside .pgrp on the far left.
        if (c.pulse_logo && dc && l.pulse_logo.right > l.pulse_logo.left) {
            const float edge = std::round(l.pulse_logo.right - l.pulse_logo.left);
            const float lx = std::round(l.pulse_logo.left);
            const float ly = std::round((l.pulse_logo.top + l.pulse_logo.bottom - edge) * 0.5f);
            const auto target = D2D1::RectF(lx, ly, lx + edge, ly + edge);
            dc->DrawBitmap(c.pulse_logo, &target, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        }
        // Compact content-hugging .pgrp card framing caption + file chips + action button.
        if (l.pulse_card.right > l.pulse_card.left && l.pulse_card.bottom > l.pulse_card.top) {
            painter.FillRoundedRect(l.pulse_card, Dip(scale, 8.0f), high_contrast ? theme.bg : theme.surface_card);
            painter.StrokeRoundedRect(l.pulse_card, Dip(scale, 8.0f), high_contrast ? theme.text : theme.stroke_card, std::max(1.0f, Dip(scale, 1.0f)));
        }
        // Caption (.pgrp .gt): 12px secondary text + optional muted folder suffix (.gt .q).
        if (l.pulse_caption.right > l.pulse_caption.left) {
            if (l.pulse_caption_show_folder && !c.pulse_folder.empty()) {
                const float main_w = typography::MeasureLine(&compositor, compositor.SmallFormat(), c.pulse_caption);
                painter.DrawText(c.pulse_caption,
                    D2D1::RectF(l.pulse_caption.left, l.pulse_caption.top,
                                std::min(l.pulse_caption.right, l.pulse_caption.left + main_w + Dip(scale, 2.0f)),
                                l.pulse_caption.bottom),
                    compositor.SmallFormat(), theme.text_secondary);
                const float suf_x = l.pulse_caption.left + main_w;
                if (suf_x < l.pulse_caption.right) {
                    const std::wstring suffix = L" \x00B7 " + c.pulse_folder;
                    painter.DrawText(FitEnd(compositor, compositor.SmallFormat(), suffix, l.pulse_caption.right - suf_x),
                        D2D1::RectF(suf_x, l.pulse_caption.top, l.pulse_caption.right, l.pulse_caption.bottom),
                        compositor.SmallFormat(), high_contrast ? theme.text_secondary : WithAlpha(theme.text, 0.48f));
                }
            } else {
                painter.DrawText(FitEnd(compositor, compositor.SmallFormat(), c.pulse_caption,
                    std::max(0.0f, l.pulse_caption.right - l.pulse_caption.left)),
                    l.pulse_caption, compositor.SmallFormat(), theme.text_secondary);
            }
        }
        const auto pulse_ext_color = [](const std::wstring& ext) -> uint32_t {
            if (ext == L"PSD") return 0x1473E6;
            if (ext == L"PNG") return 0x2E9E6A;
            if (ext == L"JPG" || ext == L"JPEG") return 0xD9822B;
            if (ext == L"WEBP") return 0x7A5AF8;
            if (ext == L"SVG") return 0xE0A100;
            if (ext == L"PDF") return 0xD64545;
            if (ext == L"ZIP" || ext == L"RAR" || ext == L"7Z") return 0x8B55DB;
            if (ext == L"AI") return 0xF08A24;
            if (ext == L"DOC" || ext == L"DOCX") return 0x6A78A8;
            if (ext == L"URL") return 0x4F8A99;
            if (ext == L"XLS" || ext == L"XLSX" || ext == L"CSV") return 0x1F8A5B;
            if (ext == L"PPT" || ext == L"PPTX") return 0xD95B38;
            if (ext == L"MP4" || ext == L"MOV" || ext == L"MKV" || ext == L"AVI" || ext == L"WEBM") return 0xC73A7E;
            if (ext == L"MP3" || ext == L"WAV" || ext == L"FLAC") return 0x2E7BB8;
            return 0x6E7C91;
        };
        const auto draw_tiny_text = [&](std::wstring_view text, const D2D1_RECT_F& box, float dip_size,
                                        DWRITE_FONT_WEIGHT weight, const D2D1_COLOR_F& color,
                                        DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_CENTER) {
            DrawTinyText(compositor, dc, scale, text, box, dip_size, weight, color, align);
        };
        for (size_t i = 0; i < l.pulse_items.size(); ++i) {
            const auto& r = l.pulse_items[i];
            if (r.right <= r.left) continue;
            const bool item_en = c.pulse_selection_enabled && c.pulse_selection[i].enabled;
            const bool item_sel = item_en && c.pulse_selection[i].selected;
            const auto st = StateFor(c, kPickPulseFileBase + static_cast<int>(i), item_en);
            const float chip_r = Dip(scale, 6.0f);
            if (item_sel) {
                painter.FillRoundedRect(r, chip_r, high_contrast ? theme.fill_selected : WithAlpha(theme.accent, 0.10f));
                painter.StrokeRoundedRect(r, chip_r, high_contrast ? theme.accent : WithAlpha(theme.accent, 0.42f), std::max(1.0f, Dip(scale, 1.0f)));
            } else if (item_en && (st.hovered || st.pressed)) {
                painter.FillRoundedRect(r, chip_r, st.pressed ? theme.fill_pressed : theme.fill_hover);
            }
            if (st.keyboard_focus) painter.DrawFocusRing(r, chip_r);

            const std::wstring& fname = c.pulse_selection[i].text;
            const auto dot = fname.find_last_of(L'.');
            std::wstring ext = (dot != std::wstring::npos && dot + 1 < fname.size()) ? fname.substr(dot + 1) : L"";
            if (ext.size() > 4) ext.resize(4);
            for (auto& ch : ext) ch = static_cast<wchar_t>(std::towupper(ch));

            // 18x18 DIP solid colored file-type tile (.pf .ft).
            const float tile_sz = Dip(scale, 18.0f);
            const float cy_i = (r.top + r.bottom) * 0.5f;
            const auto tile_rc = D2D1::RectF(r.left + Dip(scale, 4.0f), cy_i - tile_sz * 0.5f,
                                             r.left + Dip(scale, 4.0f) + tile_sz, cy_i + tile_sz * 0.5f);
            const D2D1_COLOR_F tile_bg = high_contrast ? theme.text : WithAlpha(HexColor(pulse_ext_color(ext)), item_en ? 1.0f : 0.42f);
            const D2D1_COLOR_F tile_fg = high_contrast ? theme.bg : WithAlpha(HexColor(0xFFFFFF), item_en ? 1.0f : 0.78f);
            painter.FillRoundedRect(tile_rc, Dip(scale, 4.0f), tile_bg);
            if (!ext.empty()) {
                const float ext_dip = ext.size() >= 4 ? 6.0f : 7.0f;
                draw_tiny_text(ext, tile_rc, ext_dip, DWRITE_FONT_WEIGHT_BOLD, tile_fg);
            } else {
                const float ins = Dip(scale, 3.5f);
                DrawStoreIcon(dc, StoreIcon::File, D2D1::RectF(tile_rc.left + ins, tile_rc.top + ins,
                                                               tile_rc.right - ins, tile_rc.bottom - ins), tile_fg, 1.2f);
            }

            const float tx = tile_rc.right + Dip(scale, 6.0f);
            const float text_max = std::max(0.0f, r.right - Dip(scale, 8.0f) - tx);
            const D2D1_COLOR_F name_ink = !item_en ? (high_contrast ? theme.text_disabled : WithAlpha(theme.text, 0.42f))
                : item_sel && !high_contrast ? theme.accent : theme.text;
            const auto text = FitEnd(compositor, compositor.FileNameFormat(), fname, text_max);
            painter.DrawText(text, D2D1::RectF(tx, r.top, r.right - Dip(scale, 8.0f), r.bottom),
                             compositor.FileNameFormat(), name_ink);
        }
        if (l.pulse_more.right > l.pulse_more.left) {
            const auto mst = StateFor(c, kPickPulseMore, c.pulse_selection_enabled);
            const float mr = Dip(scale, 6.0f);
            if (mst.enabled && (mst.hovered || mst.pressed)) {
                painter.FillRoundedRect(l.pulse_more, mr, mst.pressed ? theme.fill_pressed : theme.fill_hover);
            }
            if (mst.keyboard_focus) painter.DrawFocusRing(l.pulse_more, mr);
            const D2D1_COLOR_F m_ink = mst.enabled ? theme.text_secondary : theme.text_disabled;
            const float chev_sz = Dip(scale, 10.0f);
            const float m_cy = (l.pulse_more.top + l.pulse_more.bottom) * 0.5f;
            const float chev_l = l.pulse_more.right - Dip(scale, 6.0f) - chev_sz;
            painter.DrawText(FitEnd(compositor, compositor.SmallFormat(), l.pulse_more_text,
                    std::max(0.0f, chev_l - Dip(scale, 4.0f) - (l.pulse_more.left + Dip(scale, 8.0f)))),
                D2D1::RectF(l.pulse_more.left + Dip(scale, 8.0f), l.pulse_more.top, chev_l - Dip(scale, 4.0f), l.pulse_more.bottom),
                compositor.SmallFormat(), m_ink);
            DrawStoreIcon(dc, StoreIcon::ChevronDown, D2D1::RectF(chev_l, m_cy - chev_sz * 0.5f, chev_l + chev_sz, m_cy + chev_sz * 0.5f), m_ink, 1.3f);
        }
        // Compact .btn.s action button (26 DIP tall): primary in single-select, standard in multi-select.
        if (l.pulse_use.right > l.pulse_use.left) {
            const auto ust = StateFor(c, kPickPulseUse, c.pulse_selection_enabled);
            const bool pri = c.pulse_use_primary;
            const float br = Dip(scale, 6.0f);
            D2D1_COLOR_F bg{}, fg{}, border{};
            bool draw_border = !pri;
            if (high_contrast) {
                bg = !ust.enabled ? theme.fill_input : pri ? theme.accent : (ust.hovered || ust.pressed ? theme.fill_hover : theme.bg);
                fg = !ust.enabled ? theme.text_disabled : pri ? theme.accent_text : theme.text;
                border = theme.text;
                draw_border = true;
            } else if (pri) {
                const AccentShades shades = DeriveAccentShades(theme.accent);
                const bool dark_theme = 0.2126f * theme.bg.r + 0.7152f * theme.bg.g + 0.0722f * theme.bg.b < 0.5f;
                bg = !ust.enabled ? WithAlpha(theme.accent, 0.45f)
                    : ust.pressed ? (dark_theme ? shades.dark2 : shades.light3)
                    : ust.hovered ? (dark_theme ? shades.dark1 : shades.light1)
                    : theme.accent;
                fg = !ust.enabled ? WithAlpha(theme.accent_text, 0.72f)
                    : ust.pressed ? WithAlpha(theme.accent_text, 0.72f)
                    : theme.accent_text;
                border = bg;
            } else {
                bg = !ust.enabled ? WithAlpha(theme.surface_card, 0.55f)
                    : ust.pressed ? theme.fill_pressed
                    : ust.hovered ? theme.fill_hover
                    : theme.surface_card;
                fg = !ust.enabled ? theme.text_disabled : theme.text;
                border = theme.stroke_card;
                draw_border = true;
            }
            painter.FillRoundedRect(l.pulse_use, br, bg);
            if (draw_border) painter.StrokeRoundedRect(l.pulse_use, br, border, std::max(1.0f, Dip(scale, 1.0f)));
            if (ust.keyboard_focus) painter.DrawFocusRing(l.pulse_use, br);

            const float kw = CaptionWidth(painter, scale, L"Alt 0") + Dip(scale, 10.0f);
            const float kh = Dip(scale, 18.0f);
            const float u_cy = (l.pulse_use.top + l.pulse_use.bottom) * 0.5f;
            const float label_l = l.pulse_use.left + Dip(scale, 9.0f);
            const float key_r = l.pulse_use.right - Dip(scale, 9.0f);
            const float key_l = std::max(label_l, key_r - kw);
            const float label_r = std::max(label_l, key_l - Dip(scale, 6.0f));
            if (pri) {
                draw_tiny_text(c.pulse_use_text, D2D1::RectF(label_l, l.pulse_use.top, label_r, l.pulse_use.bottom),
                               12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, fg, DWRITE_TEXT_ALIGNMENT_LEADING);
            } else {
                painter.DrawText(c.pulse_use_text, D2D1::RectF(label_l, l.pulse_use.top, label_r, l.pulse_use.bottom),
                                 compositor.SmallFormat(), fg);
            }
            const auto kr = D2D1::RectF(key_l, u_cy - kh * 0.5f, key_r, u_cy + kh * 0.5f);
            DrawPickerKeycap(compositor, dc, painter, theme, scale, kr, L"Alt 0", high_contrast, ust.enabled);
        }
        painter.FillRoundedRect(D2D1::RectF(l.pulse_band.left, l.pulse_band.bottom - 1.0f,
            l.pulse_band.right, l.pulse_band.bottom), 0, theme.stroke_divider);
    }
    if (l.auto_jump_notice.right > l.auto_jump_notice.left) {
        auto notice = AutoJumpInfo(c, l.auto_jump_notice);
        const auto parts = painter.LayoutInfoBar(notice);
        notice.title = {};
        painter.DrawInfoBar(notice);
        painter.DrawText(FitEnd(compositor, compositor.SmallFormat(), c.auto_jump_notice,
            parts.title.right - parts.title.left), parts.title, compositor.SmallFormat(), theme.text);
    }
    for (size_t i = 0; i < l.context_filters.size() && i < c.context_filters.size(); ++i) {
        const auto& filter = c.context_filters[i];
        auto state = StateFor(c, filter.id, filter.enabled); state.checked = state.selected = filter.checked;
        const auto rect = l.context_filters[i];
        if (rect.right <= rect.left) continue;
        // HTML .fchip: a full pill; on = selection tint, accent outline and semibold accent text.
        const float pill = (rect.bottom - rect.top) * 0.5f;
        const float line = std::max(1.0f, Dip(scale, 1.0f));
        const bool on = filter.checked;
        D2D1_COLOR_F fill = high_contrast ? theme.bg : on ? WithAlpha(theme.accent, 0.12f) : theme.surface_card;
        painter.FillRoundedRect(rect, pill, fill);
        if (!on && (state.hovered || state.pressed) && filter.enabled)
            painter.FillRoundedRect(rect, pill, state.pressed ? theme.fill_selected : theme.fill_hover);
        painter.StrokeRoundedRect(rect, pill, high_contrast ? (on ? theme.accent : theme.text)
            : on ? WithAlpha(theme.accent, 0.45f) : theme.stroke_card, line);
        const D2D1_COLOR_F ink = on ? theme.accent : theme.text_secondary;
        float tx = rect.left + Dip(scale, 10.0f);
        if (const StoreIcon icon = FilterChipIcon(c, filter); icon != StoreIcon::None) {
            const float cy = (rect.top + rect.bottom) * 0.5f;
            DrawStoreIcon(dc, icon, D2D1::RectF(tx, cy - Dip(scale, 6.5f), tx + Dip(scale, 13.0f), cy + Dip(scale, 6.5f)), ink, 1.2f);
            tx += Dip(scale, 19.0f);
        }
        auto text_rect = D2D1::RectF(tx, rect.top, rect.right - Dip(scale, 8.0f), rect.bottom);
        if (filter.drop_down) {
            const float cy = (rect.top + rect.bottom) * 0.5f;
            const float cx = text_rect.right - Dip(scale, 4.0f);
            ID2D1SolidColorBrush* cb = nullptr;
            if (dc && SUCCEEDED(dc->CreateSolidColorBrush(ink, &cb))) {
                dc->DrawLine({cx - Dip(scale, 3.0f), cy - Dip(scale, 1.5f)}, {cx, cy + Dip(scale, 1.8f)}, cb, std::max(1.0f, Dip(scale, 1.2f)));
                dc->DrawLine({cx, cy + Dip(scale, 1.8f)}, {cx + Dip(scale, 3.0f), cy - Dip(scale, 1.5f)}, cb, std::max(1.0f, Dip(scale, 1.2f)));
                cb->Release();
            }
            text_rect.right -= Dip(scale, 11.0f);
        }
        const std::wstring text = filter.more ? filter.text + L" " + std::to_wstring(l.context_folded) : filter.text;
        if (filter.count >= 0) {  // HTML .c: the count, quieter, after the label
            const std::wstring count = std::to_wstring(filter.count);
            const float count_w = CaptionWidth(painter, scale, count);
            const auto count_rect = D2D1::RectF(text_rect.right - count_w, rect.top, text_rect.right, rect.bottom);
            painter.DrawText(count, count_rect, compositor.SmallFormat(), WithAlpha(ink, on ? 0.75f : 0.6f));
            text_rect.right = count_rect.left - Dip(scale, 5.0f);
        }
        painter.DrawText(FitEnd(compositor, compositor.SmallFormat(), text, std::max(0.0f, text_rect.right - text_rect.left)),
                         text_rect, compositor.SmallFormat(), ink);
        if (state.keyboard_focus)
            painter.StrokeRoundedRect(D2D1::RectF(rect.left - Dip(scale, 2.0f), rect.top - Dip(scale, 2.0f), rect.right + Dip(scale, 2.0f), rect.bottom + Dip(scale, 2.0f)),
                pill + Dip(scale, 2.0f), theme.accent, Dip(scale, 2.0f));
    }
    if (c.filter_tools_visible && l.sort_sync.right > l.sort_sync.left) {
        const auto st = StateFor(c, kPickSortSync);
        if (st.hovered || st.pressed)
            painter.FillRoundedRect(l.sort_sync, Dip(scale, 5.0f), st.pressed ? theme.fill_selected : theme.fill_hover);
        painter.DrawGlyph(L"\xE8CB", l.sort_sync, (st.hovered || c.sort_synced) && !high_contrast ? theme.text : theme.text_secondary);
        if (c.sort_synced && dc) {
            ID2D1SolidColorBrush* ab = nullptr;
            if (SUCCEEDED(dc->CreateSolidColorBrush(high_contrast ? theme.text : theme.accent, &ab))) {
                dc->FillEllipse(D2D1::Ellipse({l.sort_sync.right - Dip(scale, 5.0f), l.sort_sync.top + Dip(scale, 5.0f)},
                                              Dip(scale, 2.6f), Dip(scale, 2.6f)), ab);
                ab->Release();
            }
        }
    }
    if (c.filter_tools_visible && c.preview_toggle_visible && l.preview_toggle.right > l.preview_toggle.left) {
        const auto st = StateFor(c, kPickPreviewToggle);
        if (c.preview_toggle_on && !high_contrast) {
            painter.FillRoundedRect(l.preview_toggle, Dip(scale, 5.0f), WithAlpha(theme.accent, st.hovered ? 0.24f : 0.14f));
        } else if (st.hovered || st.pressed) {
            painter.FillRoundedRect(l.preview_toggle, Dip(scale, 5.0f), st.pressed ? theme.fill_selected : theme.fill_hover);
        }
        const D2D1_COLOR_F ink = c.preview_toggle_on && !high_contrast ? theme.accent : (st.hovered ? theme.text : theme.text_secondary);
        const float cx = (l.preview_toggle.left + l.preview_toggle.right) * 0.5f;
        const float cy = (l.preview_toggle.top + l.preview_toggle.bottom) * 0.5f;
        const auto box = D2D1::RectF(cx - Dip(scale, 7.0f), cy - Dip(scale, 5.5f), cx + Dip(scale, 7.0f), cy + Dip(scale, 5.5f));
        const float split_x = box.left + (box.right - box.left) * 0.62f;
        if (c.preview_toggle_on && !high_contrast)
            painter.FillRoundedRect({split_x, box.top + Dip(scale, 1.0f), box.right - Dip(scale, 1.0f), box.bottom - Dip(scale, 1.0f)}, Dip(scale, 1.5f), WithAlpha(ink, 0.35f));
        painter.StrokeRoundedRect(box, Dip(scale, 2.0f), ink, std::max(1.0f, Dip(scale, 1.25f)));
        painter.FillRoundedRect({split_x, box.top, split_x + std::max(1.0f, Dip(scale, 1.2f)), box.bottom}, 0, ink);
    }

    for (size_t i = 0; i < l.option_chips.size(); ++i) {
        const auto& option = c.options[i];
        auto state = StateFor(c, option.id, option.enabled);
        state.checked = option.checked;
        if (option.property) {  // HTML .prop: quiet label segment, value segment, one outline
            const auto& r = l.option_chips[i];
            const auto& lab = l.option_labels[i];
            const auto& val = l.option_buttons[i];
            const float prop_radius = Dip(scale, 6.0f);
            const float line = std::max(1.0f, Dip(scale, 1.0f));
            painter.FillRoundedRect(r, prop_radius, high_contrast ? theme.bg : theme.surface_card);
            if (!high_contrast) {
                // One translucent fill running past the divider, then the opaque card takes the
                // overrun back: square right corners without a doubled-alpha seam.
                painter.FillRoundedRect(D2D1::RectF(lab.left, lab.top, lab.right + prop_radius, lab.bottom), prop_radius, WithAlpha(theme.text, 0.05f));
                painter.FillRoundedRect(D2D1::RectF(lab.right, lab.top + line, lab.right + prop_radius, lab.bottom - line), 0, theme.surface_card);
            }
            if (state.hovered || state.pressed) painter.FillRoundedRect(r, prop_radius, state.pressed ? theme.fill_selected : theme.fill_hover);
            painter.FillRoundedRect(D2D1::RectF(lab.right, r.top + line, lab.right + line, r.bottom - line), 0, theme.stroke_divider);
            painter.StrokeRoundedRect(r, prop_radius, high_contrast ? theme.text : theme.stroke_card, line);
            painter.DrawText(FitEnd(compositor, compositor.SmallFormat(), option.label, lab.right - lab.left - Dip(scale, 12.0f)),
                D2D1::RectF(lab.left + Dip(scale, 8.0f), lab.top, lab.right - Dip(scale, 4.0f), lab.bottom),
                compositor.SmallFormat(), theme.text_secondary);
            const D2D1_COLOR_F ink = !option.enabled ? theme.text_disabled : option.add && !high_contrast ? theme.accent : theme.text;
            float vx = val.left + Dip(scale, 9.0f);
            if (option.add) {
                const float cy = (val.top + val.bottom) * 0.5f;
                DrawStoreIcon(dc, StoreIcon::Plus, D2D1::RectF(vx, cy - Dip(scale, 6.0f), vx + Dip(scale, 12.0f), cy + Dip(scale, 6.0f)), ink, 1.4f);
                vx += Dip(scale, 17.0f);
            }
            painter.DrawText(FitEnd(compositor, compositor.TextFormat(), option.text, val.right - vx - Dip(scale, 8.0f)),
                D2D1::RectF(vx, val.top, val.right - Dip(scale, 6.0f), val.bottom), compositor.TextFormat(), ink);
            if (state.keyboard_focus)
                painter.StrokeRoundedRect(D2D1::RectF(r.left - Dip(scale, 2.0f), r.top - Dip(scale, 2.0f), r.right + Dip(scale, 2.0f), r.bottom + Dip(scale, 2.0f)),
                    prop_radius + Dip(scale, 2.0f), theme.accent, Dip(scale, 2.0f));
            continue;
        }
        const bool separate_label = option.drop_down && !option.checkable && !option.label.empty();
        const auto text = separate_label || option.label.empty() ? option.text : option.label + L" " + option.text;
        if (separate_label) painter.DrawText(option.label, l.option_labels[i], compositor.TextFormat(),
            option.enabled ? theme.text_secondary : theme.text_disabled);
        if (option.checkable) painter.DrawCheckBox(l.option_chips[i], text, state);
        else {
            fluent::ButtonSpec spec{l.option_buttons[i], text, {}, fluent::ButtonKind::Standard, state};
            spec.drop_down = option.drop_down;
            painter.DrawButton(spec);
        }
    }
    if (l.more_options.right > l.more_options.left) {
        const auto count = std::to_wstring(c.options.size() - l.option_chips.size());
        fluent::ButtonSpec spec{l.more_options, L"", {}, fluent::ButtonKind::Transparent, StateFor(c, kPickMoreOptions)};
        spec.drop_down = true;
        painter.DrawButton(spec);
        painter.DrawText(FitEnd(compositor, compositor.TextFormat(), c.more_options_text,
            l.more_options_label.right - l.more_options_label.left), l.more_options_label, compositor.TextFormat(),
            high_contrast ? theme.text : theme.accent);  // HTML .morex: accent text
        painter.DrawBadge({l.more_options_badge, count, fluent::BadgeKind::Accent});
    }

    // The shared sheet runs to the bottom edge; a hairline separates the footer
    // from the panes like the main window's status bar.
    painter.FillRoundedRect(D2D1::RectF(l.footer.left, l.footer.top, l.footer.right,
                                        l.footer.top + 1.0f),
                            0, theme.stroke_divider);

    // File name and file type.
    if (l.filename.right > l.filename.left) {
        painter.DrawText(FitEnd(compositor, compositor.FileNameFormat(), c.filename_label, l.filename_label.right - l.filename_label.left),
                         l.filename_label, compositor.FileNameFormat(), theme.text_secondary);
        fluent::ControlState field = StateFor(c, kPickFilename);
        field.focused = c.filename_focused;
        if (c.selected_names.empty()) painter.DrawTextFieldFrame(l.filename, field, c.hosted_edit);
        if (!c.hosted_edit && l.selection_tags.empty() && !c.filename_text.empty()) {
            const D2D1_RECT_F inner = D2D1::RectF(l.filename.left + Dip(scale, 10.0f), l.filename.top,
                                                  l.filename.right - Dip(scale, 10.0f), l.filename.bottom);
            painter.DrawText(FitEnd(compositor, compositor.FileNameFormat(), c.filename_text,
                                    inner.right - inner.left),
                             inner, compositor.FileNameFormat(), theme.text);
        }
        for (size_t i = 0; i < l.selection_tags.size() && i < c.selected_names.size(); ++i) {
            const auto tag = l.selection_tags[i];
            painter.FillRoundedRect(tag, Dip(scale, 6.0f), theme.surface_card);
            painter.StrokeRoundedRect(tag, Dip(scale, 6.0f), high_contrast ? theme.text : theme.stroke_divider, 1.0f);
            const float tag_mid = (tag.top + tag.bottom) * 0.5f;
            const auto ext_at = c.selected_names[i].find_last_of(L'.');
            std::wstring ext = ext_at == std::wstring::npos ? L"" : c.selected_names[i].substr(ext_at);
            CharLowerBuffW(ext.data(), static_cast<DWORD>(ext.size()));
            const bool picture = ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".gif" || ext == L".webp";
            const uint32_t file_color = picture ? 0x7B61C9 : ext == L".pdf" ? 0xD2493D :
                ext == L".xlsx" || ext == L".xls" ? 0x1F7A45 : ext == L".pptx" || ext == L".ppt" ? 0xC9542F : 0x2B6CC4;
            DrawStoreIcon(dc, picture ? StoreIcon::Image : StoreIcon::File,
                D2D1::RectF(tag.left + Dip(scale, 6.0f), tag_mid - Dip(scale, 7.0f), tag.left + Dip(scale, 20.0f), tag_mid + Dip(scale, 7.0f)),
                high_contrast ? theme.text : HexColor(file_color), 1.2f);
            const std::wstring source = i < c.selected_sources.size() ? c.selected_sources[i] : std::wstring();
            const bool tray = i < c.selected_from_tray.size() && c.selected_from_tray[i];
            float source_width = source.empty() ? 0 : std::min({Dip(scale, 100.0f),
                std::max(0.0f, tag.right - tag.left - Dip(scale, 124.0f)),
                BodyWidth(painter, scale, source) + Dip(scale, tray ? 22.0f : 10.0f)});
            // Keep the filename readable in narrow chips. A tray icon is enough
            // when its full source label does not fit; More/tooltip keep detail.
            const bool source_icon_only = tray && source_width < BodyWidth(painter, scale, source) + Dip(scale, 22.0f);
            if (source_icon_only) source_width = source_width >= Dip(scale, 18.0f) ? Dip(scale, 18.0f) : 0;
            else if (source_width < Dip(scale, 20.0f)) source_width = 0;
            const auto source_rect = D2D1::RectF(tag.right - Dip(scale, 24.0f) - source_width, tag.top,
                tag.right - Dip(scale, 24.0f), tag.bottom);
            if (source_width > 0) {
                auto label_rect = source_rect;
                if (tray) {
                    painter.DrawText(L"\xE7B8", D2D1::RectF(source_rect.left, source_rect.top,
                        source_rect.left + Dip(scale, 16.0f), source_rect.bottom), compositor.IconFormat(), theme.text_secondary);
                    label_rect.left += Dip(scale, 18.0f);
                }
                if (!source_icon_only) painter.DrawText(FitEnd(compositor, compositor.SmallFormat(), source,
                    std::max(0.0f, label_rect.right - label_rect.left)), label_rect, compositor.SmallFormat(), theme.text_secondary);
            }
            const auto text_rect = D2D1::RectF(tag.left + Dip(scale, 26.0f), tag.top,
                std::max(tag.left + Dip(scale, 26.0f), source_rect.left - Dip(scale, 4.0f)), tag.bottom);
            painter.DrawText(FitEnd(compositor, compositor.SmallFormat(), c.selected_names[i],
                std::max(0.0f, text_rect.right - text_rect.left)), text_rect, compositor.SmallFormat(), theme.text);
            const auto remove = D2D1::RectF(tag.right - Dip(scale, 24.0f), tag.top, tag.right, tag.bottom);
            painter.DrawButton({remove, L"\x00D7", {}, fluent::ButtonKind::Transparent,
                StateFor(c, kPickRemoveSelectionBase + static_cast<int>(i))});
        }
        if (!c.selected_names.empty()) {
            painter.DrawButton({l.selection_clear, l10n::Get(l10n::StringId::ClearAll), {},
                fluent::ButtonKind::Transparent, StateFor(c, kPickClearSelection)});
            if (!l.selection_more_text.empty()) {
                fluent::ButtonSpec more{l.selection_more, l.selection_more_text, {},
                    fluent::ButtonKind::Transparent, StateFor(c, kPickSelectionMore)};
                more.drop_down = true; painter.DrawButton(more);
            }
        }
        if (l.filter.right > l.filter.left && c.filter_static) {
            // A single type is a fact, not a choice (HTML 只有一种格式).
            painter.StrokeRoundedRect(l.filter, Dip(scale, 4.0f), theme.stroke_card, std::max(1.0f, Dip(scale, 1.0f)));
            const auto text_rect = D2D1::RectF(l.filter.left + Dip(scale, 11.0f), l.filter.top,
                                               l.filter.right - Dip(scale, 11.0f), l.filter.bottom);
            painter.DrawText(FitEnd(compositor, compositor.FileNameFormat(), c.filter_text, text_rect.right - text_rect.left),
                             text_rect, compositor.FileNameFormat(), theme.text_secondary);
        } else if (l.filter.right > l.filter.left) {
            fluent::ButtonSpec filter{l.filter, L"", {}, fluent::ButtonKind::Standard, StateFor(c, kPickFilter)};
            filter.drop_down = true;
            filter.chevron_turn = c.filter_turn;
            painter.DrawButton(filter);
            const auto text_rect = D2D1::RectF(l.filter.left + Dip(scale, 10.0f), l.filter.top,
                                               l.filter.right - Dip(scale, 28.0f), l.filter.bottom);
            painter.DrawText(FitEnd(compositor, compositor.FileNameFormat(), c.filter_text, std::max(0.0f, text_rect.right - text_rect.left)),
                             text_rect, compositor.FileNameFormat(), theme.text);
        }
        if (c.filter_static && !c.format_static_text.empty()) {
            painter.DrawText(FitEnd(compositor, compositor.FileNameFormat(), c.format_static_text,
                std::max(0.0f, l.formats.right - l.formats.left - Dip(scale, 12.0f))),
                D2D1::RectF(l.formats.left + Dip(scale, 6.0f), l.formats.top, l.formats.right, l.formats.bottom),
                compositor.FileNameFormat(), theme.text_secondary);
        } else if (!l.format_segments.empty() && l.format_segments.size() == c.formats.size()) {
            painter.DrawSegmentedTrack(l.formats);
            const size_t count = static_cast<size_t>(std::count_if(l.format_segments.begin(), l.format_segments.end(),
                [](const auto& r) { return r.right > r.left; }));
            size_t drawn = 0;
            for (size_t i = 0; i < l.format_segments.size(); ++i) {
                const auto& r = l.format_segments[i];
                if (r.right <= r.left) continue;
                const auto& format = c.formats[i];
                if (format.any && drawn > 0) {  // *.* sits apart from the specific formats
                    const float cx = std::round(r.left);
                    painter.FillRoundedRect(D2D1::RectF(cx, r.top + Dip(scale, 8.0f), cx + std::max(1.0f, Dip(scale, 1.0f)),
                                                        r.bottom - Dip(scale, 8.0f)), 0, theme.stroke_divider);
                }
                fluent::SegmentedItemSpec seg;
                seg.bounds = r;
                seg.shared_track = true;
                seg.position = count == 1 ? fluent::SegmentPosition::Single : drawn == 0 ? fluent::SegmentPosition::First
                    : drawn + 1 == count ? fluent::SegmentPosition::Last : fluent::SegmentPosition::Middle;
                seg.state = StateFor(c, kPickFormatBase + static_cast<int>(i));
                seg.state.selected = !c.filter_static && static_cast<int>(i) == c.format_selected;
                const std::wstring text = FitEnd(compositor, compositor.TextFormat(), format.text,
                                                 std::max(0.0f, r.right - r.left - Dip(scale, 12.0f)));
                seg.text = text;
                painter.DrawSegmentedItem(seg);
                ++drawn;
            }
        }
        if (!c.filter_static && l.format_more.right > l.format_more.left) {
            // The button measures and elides with its own body font; pre-eliding
            // with the compositor font can cut a correctly measured More label.
            fluent::ButtonSpec more{l.format_more, l.format_more_text, {}, fluent::ButtonKind::Standard, StateFor(c, kPickFilter)};
            more.drop_down = !c.filter_static;
            painter.DrawButton(more);
        }
    }

    if (l.format_note.right > l.format_note.left) {
        painter.DrawText(FitEnd(compositor, compositor.SmallFormat(), c.format_note,
            l.format_note.right - l.format_note.left), l.format_note, compositor.SmallFormat(), theme.text_secondary);
    }
    if (l.suggestions_caption.right > l.suggestions_caption.left) {
        painter.DrawText(FitEnd(compositor, compositor.SmallFormat(), l10n::Get(l10n::StringId::SearchSuggestions),
            l.suggestions_caption.right - l.suggestions_caption.left), l.suggestions_caption, compositor.SmallFormat(), theme.text_secondary);
    }
    for (size_t i = 0; i < l.suggestion_items.size(); ++i) {
        const auto r = l.suggestion_items[i];
        if (r.right <= r.left) continue;
        const auto text = FitEnd(compositor, compositor.TextFormat(), c.save_suggestions[i].text, std::max(0.0f, r.right - r.left - Dip(scale, 24.0f)));
        painter.DrawButton({r, text, {}, fluent::ButtonKind::Standard,
            StateFor(c, kPickSaveSuggestionBase + static_cast<int>(i), c.save_actions_enabled)});
    }
    if (l.suggestions_more.right > l.suggestions_more.left) {
        fluent::ButtonSpec more{l.suggestions_more, l10n::Get(l10n::StringId::More), {}, fluent::ButtonKind::Transparent,
            StateFor(c, kPickSaveSuggestionsMore, c.save_actions_enabled)};
        more.drop_down = true; painter.DrawButton(more);
    }
    // What will be picked (or why not), then the buttons.
    const bool notice = !c.notice.empty();
    const std::wstring& line = notice ? c.notice : c.summary;
    if ((c.overwrite_pending || c.live_conflict) && l.conflict.right > l.conflict.left) {
        painter.FillRoundedRect(l.conflict, Dip(scale, 7.0f), high_contrast ? theme.bg : WithAlpha(theme.danger, 0.08f));
        if (high_contrast) painter.StrokeRoundedRect(l.conflict, Dip(scale, 7.0f), theme.danger, std::max(1.0f, Dip(scale, 1.0f)));
        const float cy = (l.conflict.top + l.conflict.bottom) * 0.5f;
        DrawStoreIcon(dc, StoreIcon::Warn, D2D1::RectF(l.conflict.left + Dip(scale, 10.0f), cy - Dip(scale, 8.0f), l.conflict.left + Dip(scale, 26.0f), cy + Dip(scale, 8.0f)),
                      theme.danger, 1.4f);
    }
    if (c.live_conflict && !c.overwrite_pending) {
        painter.DrawButton({l.live_rename, FitEnd(compositor, compositor.TextFormat(), c.live_rename_text,
            std::max(0.0f, l.live_rename.right - l.live_rename.left - Dip(scale, 24.0f))), {}, fluent::ButtonKind::Standard,
            StateFor(c, kPickLiveRename, c.save_actions_enabled)});
        painter.DrawButton({l.live_save, c.live_save_text, {}, fluent::ButtonKind::Standard,
            StateFor(c, kPickLiveSave, c.save_actions_enabled)});
    }
    if (c.live_conflict && !c.overwrite_pending)
        painter.DrawText(FitEnd(compositor, compositor.TextFormat(), c.live_conflict_text,
            l.conflict_text.right - l.conflict_text.left), l.conflict_text, compositor.TextFormat(), theme.text);
    if (c.overwrite_pending && notice)  // the question sits in the banner; the target stays on its row
        painter.DrawText(FitEnd(compositor, compositor.TextFormat(), c.notice, l.conflict_text.right - l.conflict_text.left),
                         l.conflict_text, compositor.TextFormat(), theme.text);
    if (!notice && !c.overwrite_pending && !c.live_conflict && !c.summary_warning.empty()) {
        const bool dark_bg = theme.bg.r + theme.bg.g + theme.bg.b < 1.5f;
        const D2D1_COLOR_F ink = high_contrast ? theme.text : c.summary_warning_error ? theme.danger
            : HexColor(dark_bg ? 0xF2B85B : 0x9A5B00);
        const float mid_y = (l.summary.top + l.summary.bottom) * 0.5f;
        DrawStoreIcon(dc, StoreIcon::Warn, D2D1::RectF(l.summary.left + Dip(scale, 1.0f), mid_y - Dip(scale, 7.0f),
                      l.summary.left + Dip(scale, 15.0f), mid_y + Dip(scale, 7.0f)), ink, 1.4f);
        const auto text = D2D1::RectF(l.summary.left + Dip(scale, 22.0f), l.summary.top, l.summary.right, l.summary.bottom);
        painter.DrawText(FitEnd(compositor, compositor.SmallFormat(), c.summary_warning, text.right - text.left),
                         text, compositor.SmallFormat(), ink);
    } else if (!notice && !c.overwrite_pending && !c.live_conflict && !c.save_format_notice.empty()) {
        painter.DrawText(FitEnd(compositor, compositor.SmallFormat(), c.save_format_notice,
            l.summary.right - l.summary.left), l.summary, compositor.SmallFormat(), theme.text_secondary);
    } else if ((!notice || c.overwrite_pending) && !c.summary_folder.empty()) {
        // 将保存到：D:\工作\季度报告\ name.docx — the folder reads first, the leaf quieter.
        const bool show_space = l.summary_space.right > l.summary_space.left;
        const float summary_right = show_space ? l.summary_space.left - Dip(scale, 12.0f) : l.summary.right;
        const float avail = summary_right - l.summary.left - Dip(scale, 22.0f);
        const float x0 = l.summary.left + Dip(scale, 22.0f);
        const float mid_y = (l.summary.top + l.summary.bottom) * 0.5f;
        DrawStoreIcon(dc, StoreIcon::Folder, D2D1::RectF(l.summary.left + Dip(scale, 1.0f), mid_y - Dip(scale, 7.0f), l.summary.left + Dip(scale, 15.0f), mid_y + Dip(scale, 7.0f)),
                      high_contrast ? theme.text : HexColor(0xD8A43E), 1.3f);
        const float lead_w = std::min(avail, typography::MeasureLine(&compositor, compositor.SmallFormat(), c.summary_lead));
        painter.DrawText(c.summary_lead, D2D1::RectF(x0, l.summary.top, x0 + lead_w + 1.0f, l.summary.bottom), compositor.SmallFormat(), theme.text_secondary);
        const float rest = std::max(0.0f, avail - lead_w);
        const float leaf_full = typography::MeasureLine(&compositor, compositor.SmallFormat(), c.summary_leaf);
        const float folder_full = typography::MeasureLine(&compositor, compositor.SmallFormat(), c.summary_folder);
        const float folder_w = std::min(folder_full, std::max(rest * 0.5f, rest - leaf_full));
        const float fx = x0 + lead_w;
        painter.DrawText(FitEnd(compositor, compositor.SmallFormat(), c.summary_folder, folder_w), D2D1::RectF(fx, l.summary.top, fx + folder_w + 1.0f, l.summary.bottom),
                         compositor.SmallFormat(), theme.text);
        const float lx = fx + folder_w;
        painter.DrawText(FitEnd(compositor, compositor.SmallFormat(), c.summary_leaf, std::max(0.0f, rest - folder_w)),
                         D2D1::RectF(lx, l.summary.top, summary_right, l.summary.bottom), compositor.SmallFormat(), theme.text_secondary);
        if (show_space) painter.DrawText(c.save_space_text, l.summary_space, compositor.SmallFormat(), theme.text_secondary);
    } else if (!notice && !c.selected_names.empty() && !c.selection_summary.empty()) {
        // HTML: ✓ 3 个文件 · 9.4 MB, the check in the accent where the folder icon sits otherwise.
        const float mid_y = (l.summary.top + l.summary.bottom) * 0.5f;
        DrawStoreIcon(dc, StoreIcon::Check, D2D1::RectF(l.summary.left + Dip(scale, 1.0f), mid_y - Dip(scale, 7.0f), l.summary.left + Dip(scale, 15.0f), mid_y + Dip(scale, 7.0f)),
                      high_contrast ? theme.text : theme.accent, 1.6f);
        const float x0 = l.summary.left + Dip(scale, 22.0f);
        const float total = std::max(0.0f, l.summary.right - x0);
        const float summary_w = std::min(total, CaptionWidth(painter, scale, c.selection_summary));
        const auto main = D2D1::RectF(x0, l.summary.top, x0 + summary_w, l.summary.bottom);
        painter.DrawText(FitEnd(compositor, compositor.SmallFormat(), c.selection_summary, summary_w), main, compositor.SmallFormat(), theme.text);
        const auto hint = D2D1::RectF(main.right + Dip(scale, 8.0f), l.summary.top, l.summary.right, l.summary.bottom);
        if (!c.selection_hint.empty() && hint.right - hint.left >= Dip(scale, 80.0f))
            painter.DrawText(FitEnd(compositor, compositor.SmallFormat(), c.selection_hint, hint.right - hint.left), hint, compositor.SmallFormat(), theme.text_secondary);
    } else if (!line.empty() && !c.overwrite_pending) {
        painter.DrawText(FitEnd(compositor, compositor.SmallFormat(), line,
                                l.summary.right - l.summary.left),
                         l.summary, compositor.SmallFormat(),
                         c.overwrite_pending ? theme.text : notice ? theme.danger : theme.text_secondary);
    }
    if (c.overwrite_pending) {
        const auto fit = [&](const std::wstring& text, const D2D1_RECT_F& r) {
            return FitEnd(compositor, compositor.TextFormat(), text, std::max(0.0f, r.right - r.left - Dip(scale, 16.0f)));
        };
        if (l.overwrite_back.right > l.overwrite_back.left)
            painter.DrawButton({l.overwrite_back, fit(c.overwrite_back_text, l.overwrite_back), {}, fluent::ButtonKind::Transparent, StateFor(c, kPickOverwriteBack)});
        if (l.overwrite_replace.right > l.overwrite_replace.left)
            painter.DrawButton({l.overwrite_replace, fit(c.overwrite_replace_text, l.overwrite_replace), {}, fluent::ButtonKind::Danger, StateFor(c, kPickOverwriteReplace)});
        painter.DrawButton({l.rename, fit(c.rename_text, l.rename), {}, fluent::ButtonKind::Standard, StateFor(c, kPickRename)});
    }
    if (c.show_tray_remove && l.tray_remove.right > l.tray_remove.left) {
        auto st = StateFor(c, kPickTrayRemoveAfterOpen);
        st.checked = c.tray_remove_checked;
        const float cb_sz = Dip(scale, 16.0f);
        const float cb_cy = (l.tray_remove.top + l.tray_remove.bottom) * 0.5f;
        const auto box = D2D1::RectF(l.tray_remove.left, cb_cy - cb_sz * 0.5f, l.tray_remove.left + cb_sz, cb_cy + cb_sz * 0.5f);
        if (st.checked) {
            painter.FillRoundedRect(box, Dip(scale, 4.0f), high_contrast ? theme.text : theme.accent);
            DrawStoreIcon(dc, StoreIcon::Check, D2D1::RectF(box.left + Dip(scale, 2.5f), box.top + Dip(scale, 2.5f),
                                                            box.right - Dip(scale, 2.5f), box.bottom - Dip(scale, 2.5f)),
                          high_contrast ? theme.bg : theme.accent_text, 1.5f);
        } else {
            painter.FillRoundedRect(box, Dip(scale, 4.0f), st.hovered ? theme.fill_hover : theme.surface_card);
            painter.StrokeRoundedRect(box, Dip(scale, 4.0f), high_contrast ? theme.text : theme.stroke_card, std::max(1.0f, Dip(scale, 1.0f)));
        }
        if (st.keyboard_focus) painter.DrawFocusRing(l.tray_remove, Dip(scale, 4.0f));
        const float lx = box.right + Dip(scale, 7.0f);
        painter.DrawText(FitEnd(compositor, compositor.SmallFormat(), c.tray_remove_text, std::max(0.0f, l.tray_remove.right - lx)),
                         D2D1::RectF(lx, l.tray_remove.top, l.tray_remove.right, l.tray_remove.bottom),
                         compositor.SmallFormat(), st.hovered ? theme.text : theme.text_secondary);
    }
    if (l.footer_tool.right > l.footer_tool.left) {
        fluent::ButtonSpec tool{l.footer_tool, c.footer_tool.text, {}, fluent::ButtonKind::Standard,
            StateFor(c, c.footer_tool.id, c.footer_tool.enabled)};
        tool.drop_down = true;
        painter.DrawButton(tool);
    }
    {
        const auto cst = StateFor(c, kPickCancel);
        painter.DrawButton({l.cancel, L"", {}, fluent::ButtonKind::Standard, cst});
        const auto ctext = FitEnd(compositor, compositor.FileNameFormat(), c.cancel_text, std::max(0.0f, l.cancel.right - l.cancel.left - Dip(scale, 16.0f)));
        painter.DrawText(ctext, l.cancel, compositor.FileNameFormat(), cst.pressed ? WithAlpha(theme.text, 0.72f) : theme.text,
                         fluent::HorizontalAlignment::Center);
    }
    const fluent::ControlState primary = StateFor(c, kPickPrimary, c.primary_enabled);
    auto primary_rect = l.primary;
    if (c.primary_drop_down) primary_rect.right -= Dip(scale, 28.0f);
    painter.DrawButton({primary_rect, L"", {}, fluent::ButtonKind::Primary, primary});
    {
        const bool dark_theme = 0.2126f * theme.bg.r + 0.7152f * theme.bg.g + 0.0722f * theme.bg.b < 0.5f;
        const D2D1_COLOR_F pfg = high_contrast ? (primary.enabled ? theme.accent_text : theme.text_disabled)
            : !primary.enabled ? (dark_theme ? WithAlpha(HexColor(0xFFFFFF), 0.43f) : WithAlpha(HexColor(0xFFFFFF), 0.90f))
            : primary.pressed ? WithAlpha(theme.accent_text, 0.63f) : theme.accent_text;
        const auto ptext = FitEnd(compositor, compositor.TabActiveFormat(), c.primary_text, std::max(0.0f, primary_rect.right - primary_rect.left - Dip(scale, 16.0f)));
        painter.DrawText(ptext, primary_rect, compositor.TabActiveFormat(), pfg, fluent::HorizontalAlignment::Center);
    }
    if (c.primary_drop_down) {
        fluent::ButtonSpec arrow{D2D1::RectF(primary_rect.right,l.primary.top,l.primary.right,l.primary.bottom),L"",{},fluent::ButtonKind::Primary,StateFor(c,kPickPrimaryDropDown,c.primary_enabled)};
        arrow.drop_down=true;
        painter.DrawButton(arrow);
    }
    // The shared accent ring vanishes against a filled button; add the Fluent
    // outer ring in the text colour.
    if (primary.keyboard_focus) {
        const float gap = Dip(scale, 3.0f);
        painter.StrokeRoundedRect(D2D1::RectF(l.primary.left - gap, l.primary.top - gap,
                                              l.primary.right + gap, l.primary.bottom + gap),
                                  radius + gap, theme.text, Dip(scale, 1.5f));
    }
}

// ---- jump panel (HTML 输入跳转) ------------------------------------------------

namespace {
constexpr float kJumpHeaderH = 24.0f, kJumpRowH = 46.0f, kJumpFootH = 36.0f, kJumpEmptyH = 40.0f;
}

PickerJumpLayout LayoutPickerJump(const PickerJumpPanel& p, D2D1_RECT_F anchor, float width, float bottom, float scale) {
    PickerJumpLayout l;
    if (!p.open) return l;
    const float w = std::min(std::max(anchor.right - anchor.left, Dip(scale, 600.0f)), width - anchor.left - Dip(scale, 12.0f));
    const float top = anchor.bottom + Dip(scale, 4.0f);
    if (w <= 0 || bottom - top < Dip(scale, kJumpFootH + 12.0f)) return l;
    const bool any = std::any_of(p.rows.begin(), p.rows.end(), [](const auto& row) { return row.group != kJumpCreate; });
    const float empty_h = !any && bottom - top >= Dip(scale, kJumpEmptyH + kJumpRowH + kJumpFootH + 16.0f)
        ? Dip(scale, kJumpEmptyH) : 0.0f;
    const float notice_h = !p.index_notice.empty() && bottom - top >= Dip(scale, 160.0f) ? Dip(scale, 84.0f) : 0.0f;
    if (notice_h > 0) {
        l.index_notice = D2D1::RectF(anchor.left + Dip(scale, 6.0f), top + Dip(scale, 6.0f), anchor.left + w - Dip(scale, 6.0f), top + notice_h);
        if (!p.index_action_text.empty())
            l.index_action = D2D1::RectF(l.index_notice.right - std::min(Dip(scale, 160.0f), w * 0.4f),
                l.index_notice.top + Dip(scale, 2.0f), l.index_notice.right - Dip(scale, 4.0f), l.index_notice.top + Dip(scale, 30.0f));
    }
    float y = top;
    const auto arrange = [&](size_t start) {
        l.rows.assign(start, {}); // Preserve model indices for hit testing and activation.
        l.headers.clear();
        y = top + Dip(scale, 6.0f) + empty_h + notice_h;
        int group = -1;
        for (size_t i = start; i < p.rows.size(); ++i) {
            const auto& row = p.rows[i];
            const float header = row.group == group ? 0.0f : Dip(scale, row.group == kJumpCreate ? 4.0f : kJumpHeaderH);
            if (y + header + Dip(scale, kJumpRowH + kJumpFootH + 6.0f) > bottom) break;
            if (row.group != group && row.group != kJumpCreate)
                l.headers.push_back({row.group, D2D1::RectF(anchor.left + Dip(scale, 16.0f), y,
                    anchor.left + w - Dip(scale, 16.0f), y + header)});
            group = row.group;
            y += header;
            l.rows.push_back(D2D1::RectF(anchor.left + Dip(scale, 6.0f), y,
                anchor.left + w - Dip(scale, 6.0f), y + Dip(scale, kJumpRowH)));
            y += Dip(scale, kJumpRowH);
        }
    };
    arrange(0);
    if (p.selected >= 0 && static_cast<size_t>(p.selected) < p.rows.size() && static_cast<size_t>(p.selected) >= l.rows.size())
        arrange(static_cast<size_t>(p.selected));
    if (empty_h > 0)
        l.empty = D2D1::RectF(anchor.left + Dip(scale, 16.0f), top + Dip(scale, 6.0f) + notice_h,
            anchor.left + w - Dip(scale, 16.0f), top + Dip(scale, 6.0f) + notice_h + empty_h);
    y += Dip(scale, 6.0f);
    l.foot = D2D1::RectF(anchor.left, y, anchor.left + w, y + Dip(scale, kJumpFootH));
    l.panel = D2D1::RectF(anchor.left, top, anchor.left + w, l.foot.bottom);
    return l;
}

int HitTestPickerJump(const PickerJumpLayout& l, float x, float y) {
    if (!ContainsRect(l.panel, x, y)) return -2;
    if (l.index_action.right > l.index_action.left && ContainsRect(l.index_action, x, y)) return kPickerJumpIndexInfoHit;
    for (size_t i = 0; i < l.rows.size(); ++i) if (ContainsRect(l.rows[i], x, y)) return static_cast<int>(i);
    return -1;
}

void DrawPickerJump(Compositor& compositor, fluent::Painter& painter, const Theme& theme,
                    const PickerJumpPanel& p, const PickerJumpLayout& l, bool high_contrast) {
    if (!p.open || l.panel.right <= l.panel.left) return;
    auto* dc = compositor.Dc();
    const float scale = (l.panel.right - l.panel.left) > 0 ? painter.Scale() : 1.0f;
    const float radius = Dip(scale, 10.0f);
    // Flyout card: a soft shadow, the card, a hairline.
    if (!high_contrast) {
        for (int i = 3; i >= 1; --i) {
            const float spread = Dip(scale, 3.0f * static_cast<float>(i));
            painter.FillRoundedRect(D2D1::RectF(l.panel.left - spread, l.panel.top - spread * 0.3f, l.panel.right + spread, l.panel.bottom + spread * 1.4f),
                                    radius + spread, D2D1::ColorF(0, 0, 0, 0.035f));
        }
    }
    painter.FillRoundedRect(l.panel, radius, high_contrast ? theme.bg : theme.surface_card);
    painter.StrokeRoundedRect(l.panel, radius, high_contrast ? theme.text : theme.stroke_card, std::max(1.0f, Dip(scale, 1.0f)));
    const D2D1_COLOR_F faint = WithAlpha(theme.text, 0.45f);
    const StoreIcon group_icons[kJumpGroupCount] = {StoreIcon::Tabs, StoreIcon::Undo, StoreIcon::Star, StoreIcon::Pc, StoreIcon::NewFolder};
    for (const auto& [group, r] : l.headers) {
        const float cy = (r.top + r.bottom) * 0.5f;
        const auto group_box = D2D1::RectF(r.left, cy - Dip(scale, 6.0f), r.left + Dip(scale, 12.0f), cy + Dip(scale, 6.0f));
        if (group == kJumpOpen) DrawCommandShape(dc, command_icons::Icon::Bolt, group_box, faint, 1.2f);
        else if (group == kJumpRecent) DrawCommandShape(dc, command_icons::Icon::History, group_box, faint, 1.2f);
        else DrawStoreIcon(dc, group_icons[group], group_box, faint, 1.2f);
        const float detail_w = p.group_details[group].empty() ? 0.0f :
            std::min((r.right - r.left) * 0.55f, painter.MeasureBadgeWidth(p.group_details[group]));
        const auto title = D2D1::RectF(r.left + Dip(scale, 18.0f), r.top, r.right - detail_w - Dip(scale, detail_w > 0 ? 8.0f : 0.0f), r.bottom);
        painter.DrawText(FitEnd(compositor, compositor.SmallFormat(), p.titles[group], title.right - title.left), title, compositor.SmallFormat(), faint);
        if (detail_w > 0) {
            const auto text = FitEnd(compositor, compositor.SmallFormat(), p.group_details[group], detail_w - Dip(scale, 12.0f));
            painter.DrawBadge({D2D1::RectF(r.right - detail_w, cy - Dip(scale, 9.0f), r.right, cy + Dip(scale, 9.0f)), text, fluent::BadgeKind::Neutral});
        }
    }
    if (l.index_notice.right > l.index_notice.left) {
        fluent::InfoBarSpec info;
        info.bounds = l.index_notice; info.message = p.index_notice; info.show_close = false;
        info.trailing_width = (l.index_action.right - l.index_action.left) / scale;
        painter.DrawInfoBar(info);
        if (l.index_action.right > l.index_action.left) {
            const auto text = FitEnd(compositor, compositor.TextFormat(), p.index_action_text,
                l.index_action.right - l.index_action.left - Dip(scale, 16.0f));
            painter.DrawButton({l.index_action, text, {}, fluent::ButtonKind::Transparent, {}});
        }
    }
    if (l.empty.right > l.empty.left)
        painter.DrawText(p.empty_text, l.empty, compositor.TextFormat(), theme.text_secondary);
    for (size_t i = 0; i < l.rows.size() && i < p.rows.size(); ++i) {
        const auto& row = p.rows[i];
        const auto& r = l.rows[i];
        if (r.bottom <= r.top) continue;
        const bool on = static_cast<int>(i) == p.selected, hot = static_cast<int>(i) == p.hover;
        if (row.group == kJumpCreate)
            painter.FillRoundedRect(D2D1::RectF(l.panel.left + 1.0f, r.top - Dip(scale, 3.0f), l.panel.right - 1.0f, r.top - Dip(scale, 2.0f)), 0, theme.stroke_divider);
        if (on || hot)
            painter.FillRoundedRect(r, Dip(scale, 7.0f), on ? WithAlpha(theme.accent, high_contrast ? 0.35f : 0.13f) : WithAlpha(theme.text, 0.05f));
        const float cy = (r.top + r.bottom) * 0.5f;
        const auto icon = D2D1::RectF(r.left + Dip(scale, 12.0f), cy - Dip(scale, 8.0f), r.left + Dip(scale, 28.0f), cy + Dip(scale, 8.0f));
        const StoreIcon kind = row.group == kJumpCreate ? StoreIcon::NewFolder : row.file ? StoreIcon::File : StoreIcon::Folder;
        const D2D1_COLOR_F kind_color = row.group == kJumpCreate || on ? theme.accent : high_contrast ? theme.text : HexColor(0xD8A43E);
        if (row.window && row.group != kJumpCreate) DrawWindowShape(painter, icon, kind_color, 1.4f);
        else DrawStoreIcon(dc, kind, icon, kind_color, 1.4f);
        // Right side: the quiet tag and the keycap.
        float right = r.right - Dip(scale, 12.0f);
        if (!row.key.empty()) {
            const float kw = typography::MeasureLine(&compositor, compositor.SmallFormat(), row.key) + Dip(scale, 12.0f);
            const auto kr = D2D1::RectF(right - kw, cy - Dip(scale, 10.0f), right, cy + Dip(scale, 10.0f));
            painter.StrokeRoundedRect(kr, Dip(scale, 4.0f), theme.stroke_divider, std::max(1.0f, Dip(scale, 1.0f)));
            painter.DrawText(row.key, kr, compositor.SmallFormat(), theme.text_secondary, fluent::HorizontalAlignment::Center);
            right = kr.left - Dip(scale, 8.0f);
        }
        if (!row.note.empty()) {
            const float nw = typography::MeasureLine(&compositor, compositor.SmallFormat(), row.note) + Dip(scale, 14.0f);
            const auto nr = D2D1::RectF(right - nw, cy - Dip(scale, 10.0f), right, cy + Dip(scale, 10.0f));
            painter.FillRoundedRect(nr, Dip(scale, 4.0f), WithAlpha(theme.text, 0.06f));
            painter.DrawText(row.note, nr, compositor.SmallFormat(), theme.text_secondary, fluent::HorizontalAlignment::Center);
            right = nr.left - Dip(scale, 10.0f);
        }
        const float tx = icon.right + Dip(scale, 12.0f);
        const float tw = std::max(0.0f, right - tx);
        const auto name_r = D2D1::RectF(tx, r.top + Dip(scale, 4.0f), tx + tw, r.top + Dip(scale, 25.0f));
        const std::wstring name = FitEnd(compositor, compositor.TextFormat(), row.name, tw);
        // Mark the typed part of the name (case-insensitive) like the mockup's highlight.
        if (!p.query.empty() && row.group != kJumpCreate) {
            std::wstring lower_name = name, lower_query = p.query;
            CharLowerBuffW(lower_name.data(), static_cast<DWORD>(lower_name.size()));
            CharLowerBuffW(lower_query.data(), static_cast<DWORD>(lower_query.size()));
            const auto at = lower_name.find(lower_query);
            if (at != std::wstring::npos) {
                const float x0 = tx + typography::MeasureLine(&compositor, compositor.TextFormat(), std::wstring_view(name).substr(0, at));
                const float x1 = tx + typography::MeasureLine(&compositor, compositor.TextFormat(), std::wstring_view(name).substr(0, at + lower_query.size()));
                painter.FillRoundedRect(D2D1::RectF(x0 - 1.0f, name_r.top + Dip(scale, 2.0f), x1 + 1.0f, name_r.bottom - Dip(scale, 1.0f)), Dip(scale, 3.0f),
                                        WithAlpha(theme.accent, high_contrast ? 0.4f : 0.2f));
            }
        }
        painter.DrawText(name, name_r, compositor.TextFormat(), theme.text);
        painter.DrawText(FitEnd(compositor, compositor.SmallFormat(), row.path, tw), D2D1::RectF(tx, r.top + Dip(scale, 24.0f), tx + tw, r.bottom - Dip(scale, 4.0f)),
                         compositor.SmallFormat(), theme.text_secondary);
    }
    // Foot: the keys, then the matching note on the right.
    painter.FillRoundedRect(D2D1::RectF(l.foot.left + 1.0f, l.foot.top, l.foot.right - 1.0f, l.foot.top + 1.0f), 0, theme.stroke_divider);
    const wchar_t* keys[5] = {L"\x2191\x2193", L"Enter", L"Tab", L"Ctrl+Enter", L"Esc"};
    float x = l.foot.left + Dip(scale, 14.0f);
    const float cy = (l.foot.top + l.foot.bottom) * 0.5f;
    for (int i = 0; i < 5; ++i) {
        if (p.hints[i].empty()) continue;
        const float kw = typography::MeasureLine(&compositor, compositor.SmallFormat(), keys[i]) + Dip(scale, 10.0f);
        const float hw = typography::MeasureLine(&compositor, compositor.SmallFormat(), p.hints[i]);
        if (x + kw + Dip(scale, 5.0f) + hw > l.foot.right - Dip(scale, 14.0f)) break;
        const auto kr = D2D1::RectF(x, cy - Dip(scale, 9.0f), x + kw, cy + Dip(scale, 9.0f));
        painter.StrokeRoundedRect(kr, Dip(scale, 4.0f), theme.stroke_divider, std::max(1.0f, Dip(scale, 1.0f)));
        painter.DrawText(keys[i], kr, compositor.SmallFormat(), theme.text_secondary, fluent::HorizontalAlignment::Center);
        painter.DrawText(p.hints[i], D2D1::RectF(kr.right + Dip(scale, 5.0f), l.foot.top, kr.right + Dip(scale, 5.0f) + hw + 2.0f, l.foot.bottom),
                         compositor.SmallFormat(), faint);
        x = kr.right + Dip(scale, 5.0f) + hw + Dip(scale, 14.0f);
    }
    const float nw = typography::MeasureLine(&compositor, compositor.SmallFormat(), p.foot_note);
    if (!p.foot_note.empty() && x + nw < l.foot.right - Dip(scale, 14.0f))
        painter.DrawText(p.foot_note, D2D1::RectF(l.foot.right - Dip(scale, 14.0f) - nw - 2.0f, l.foot.top, l.foot.right - Dip(scale, 14.0f), l.foot.bottom),
                         compositor.SmallFormat(), faint);
}

} // namespace pulse::ui

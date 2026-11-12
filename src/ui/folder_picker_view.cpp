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
constexpr float kChipH = 28.0f, kChipIcon = 14.0f, kChipLabelMax = 150.0f;
std::wstring OpenFolderKey(int index) { return L"Alt " + std::to_wstring(index + 1); }
float KeyWidth(const fluent::Painter& p, float scale, int index) {
    return std::max(Dip(scale, 18.0f), p.MeasureBadgeWidth(OpenFolderKey(index)));
}
float ChipWidth(const fluent::Painter& p, float scale, const PickerOpenChip& chip, int key_index) {
    const float key = key_index >= 0 ? KeyWidth(p, scale, key_index) + Dip(scale, 7.0f) : 0.0f;
    const float label = std::min(BodyWidth(p, scale, chip.label), Dip(scale, kChipLabelMax));
    const float source = chip.source.empty() ? 0.0f : CaptionWidth(p, scale, chip.source) + Dip(scale, 7.0f);
    return Dip(scale, 6.0f) + key + Dip(scale, kChipIcon + 7.0f) + label + source + Dip(scale, 10.0f);
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
// The labelled strip buttons keep a drawn icon in the button's glyph slot.
D2D1_RECT_F GlyphSlot(const D2D1_RECT_F& bounds, float scale) {
    const float cy = (bounds.top + bounds.bottom) * 0.5f;
    return D2D1::RectF(bounds.left + Dip(scale, 11.0f), cy - Dip(scale, 7.0f), bounds.left + Dip(scale, 25.0f), cy + Dip(scale, 7.0f));
}

fluent::InfoBarSpec AutoJumpInfo(const FolderPickerChrome& c, const D2D1_RECT_F& bounds) {
    fluent::InfoBarSpec spec;
    spec.bounds = bounds; spec.title = c.auto_jump_notice; spec.show_close = false;
    spec.actions[0] = c.undo_jump_text;
    spec.hovered_action = c.hover == kPickUndoJump ? 0 : -1;
    return spec;
}

} // namespace

float PickerFooterDip(PickerMode, bool overwrite_pending, bool has_options) {
    return 100.0f + (overwrite_pending ? 44.0f : 0.0f) + (has_options ? 38.0f : 0.0f);
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
    l.footer = D2D1::RectF(0, height - Dip(scale, PickerFooterDip(chrome.mode, chrome.overwrite_pending, !chrome.options.empty())), width, height);

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
    if (chrome.overwrite_pending) {
        // HTML 同名冲突: a banner on the summary row says what exists and offers
        // 返回修改 / 改名保存; the button row keeps 取消 and 替换 (the primary).
        l.conflict = D2D1::RectF(left, l.footer.top + Dip(scale, 52.0f + (chrome.options.empty() ? 0.0f : 38.0f)), right, l.footer.top + Dip(scale, 86.0f + (chrome.options.empty() ? 0.0f : 38.0f)));
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
                const float w = painter.MeasureButtonWidth(chrome.new_folder_text, L"\xE8F4", false,
                                                           l.new_folder_keycap ? std::wstring_view(chrome.new_folder_key) : std::wstring_view());
                l.new_folder = D2D1::RectF(end - w, cy, end, cy + ch);
                end = l.new_folder.left - Dip(scale, 4.0f);
            }
            if (!chrome.undo_jump_text.empty()) {
                const float w = painter.MeasureButtonWidth(chrome.undo_jump_text, L"\xE7A7");
                l.undo_jump = D2D1::RectF(end - w, cy, end, cy + ch);
                end = l.undo_jump.left - Dip(scale, 8.0f);
            }
            return end;
        };
        const float first_x = strip_x + Dip(scale, kChipIcon + 6.0f) + BodyWidth(painter, scale, chrome.strip_caption) + Dip(scale, 8.0f);
        float end = place_buttons(true);
        {
            float need = first_x;
            for (size_t i = 0; i < chrome.open_folders.size() && i < 9; ++i) {
                PickerOpenChip bare = chrome.open_folders[i];
                bare.source.clear();
                need += ChipWidth(painter, scale, bare, static_cast<int>(i)) + Dip(scale, 6.0f);
            }
            if (need > end) end = place_buttons(false);
        }
        float x = first_x;
        const float last_w = chrome.last_location.label.empty() ? 0.0f : ChipWidth(painter, scale, chrome.last_location, -1) + Dip(scale, 17.0f);
        // Source labels give way before any chip does: the "Pulse" ones first, then the
        // Explorer ones, so the chips and the last location stay on the row.
        const size_t shown = std::min<size_t>(chrome.open_folders.size(), 9);
        std::vector<bool> keep_source(shown);
        const auto row_width = [&] {
            float total = first_x + last_w;
            for (size_t i = 0; i < shown; ++i) {
                PickerOpenChip chip = chrome.open_folders[i];
                if (!keep_source[i]) chip.source.clear();
                total += ChipWidth(painter, scale, chip, static_cast<int>(i)) + Dip(scale, 6.0f);
            }
            return total;
        };
        for (size_t i = 0; i < shown; ++i) keep_source[i] = !chrome.open_folders[i].source.empty();
        if (row_width() > end) for (size_t i = 0; i < shown; ++i) if (!chrome.open_folders[i].window) keep_source[i] = false;
        if (row_width() > end) for (size_t i = 0; i < shown; ++i) keep_source[i] = false;
        for (size_t i = 0; i < shown; ++i) {
            PickerOpenChip chip = chrome.open_folders[i];
            const bool source = keep_source[i];
            if (!source) chip.source.clear();
            const float w = ChipWidth(painter, scale, chip, static_cast<int>(i));
            if (x + w > end) break;  // open folders first; the last location takes what is left
            l.open_chips.push_back(D2D1::RectF(x, cy, x + w, cy + ch));
            l.open_keycaps.push_back(D2D1::RectF(x + Dip(scale, 6.0f), cy + Dip(scale, 5.0f),
                x + Dip(scale, 6.0f) + KeyWidth(painter, scale, static_cast<int>(i)), cy + Dip(scale, 23.0f)));
            l.open_chip_source.push_back(source);
            x += w + Dip(scale, 6.0f);
        }
        if (last_w > 0.0f && x + last_w <= end) {
            x += Dip(scale, 11.0f);
            l.last_location = D2D1::RectF(x, cy, x + last_w - Dip(scale, 17.0f), cy + ch);
        }
    }

    if (chrome.context_top > 0) {
        float x = Dip(scale, chrome.context_left + 10.0f);
        const float end = Dip(scale, chrome.context_right - 6.0f);
        const float y = Dip(scale, chrome.context_top + 6.0f), h = Dip(scale, 28.0f);
        const auto place = [&](const std::wstring& text, const wchar_t* icon, D2D1_RECT_F& rect) {
            if (text.empty()) return;
            const float w = painter.MeasureButtonWidth(text, icon);
            if (x + w > end) return;
            rect = D2D1::RectF(x, y, x + w, y + h); x += w + Dip(scale, 8.0f);
        };
        l.new_folder_keycap = false;
        const float create_w = painter.MeasureButtonWidth(chrome.new_folder_text, L"\xE8F4", false, chrome.new_folder_key);
        const float trailing_need = Dip(scale, 140.0f);
        if (!chrome.new_folder_text.empty() && !chrome.new_folder_key.empty() && x + create_w + trailing_need <= end) {
            l.new_folder_keycap = true;
            l.new_folder = D2D1::RectF(x, y, x + create_w, y + h); x += create_w + Dip(scale, 8.0f);
        } else place(chrome.new_folder_text, L"\xE8F4", l.new_folder);
        if (chrome.auto_jump_notice.empty()) place(chrome.undo_jump_text, L"\xE7A7", l.undo_jump);
        for (const auto& filter : chrome.context_filters) {  // HTML fchip: 26 DIP pill, 12px text
            const bool icon = FilterChipIcon(chrome, filter) != StoreIcon::None;
            const float w = std::min(Dip(scale, 190.0f), CaptionWidth(painter, scale, filter.text) + Dip(scale, icon ? 39.0f : 20.0f));
            if (x + w > end) break;
            l.context_filters.push_back(D2D1::RectF(x, y + Dip(scale, 1.0f), x + w, y + h - Dip(scale, 1.0f))); x += w + Dip(scale, 6.0f);
        }
    }
    if (!chrome.auto_jump_notice.empty() && chrome.recent_top > 0) {
        const float notice_right = chrome.strip_right > 0 ? std::min(width, Dip(scale, chrome.strip_right)) : width;
        l.auto_jump_notice = D2D1::RectF(Dip(scale, chrome.recent_left + 14.0f), Dip(scale, chrome.recent_top),
            notice_right - Dip(scale, 16.0f), Dip(scale, chrome.recent_top + kPickerAutoJumpNoticeDip));
        l.undo_jump = painter.LayoutInfoBar(AutoJumpInfo(chrome, l.auto_jump_notice)).actions[0];
    }
    if (!chrome.recent_files.empty() && l.strip.bottom > l.strip.top) {
        const float recent_top = (chrome.recent_top > 0 ? Dip(scale, chrome.recent_top) : l.strip.bottom) +
            (!chrome.auto_jump_notice.empty() && chrome.recent_top > 0 ? Dip(scale, kPickerAutoJumpNoticeDip) : 0.0f);
        const float recent_right = chrome.strip_right > 0 ? std::min(width, Dip(scale, chrome.strip_right)) : width;
        l.recent = D2D1::RectF(Dip(scale, chrome.recent_left), recent_top, recent_right, recent_top + Dip(scale, kPickerRecentDip));
        const float x0 = l.recent.left + Dip(scale, chrome.recent_left > 0.0f ? 14.0f : kSidePad);
        const float card_right = std::min(right, recent_right - Dip(scale, kSidePad));
        l.recent_caption = D2D1::RectF(x0, l.recent.top + Dip(scale, 6.0f), card_right, l.recent.top + Dip(scale, 28.0f));
        const float card_w = Dip(scale, 132.0f), gap = Dip(scale, 10.0f);
        float x = x0;
        for (size_t i = 0; i < chrome.recent_files.size(); ++i) {
            if (x + card_w > card_right) break;
            l.recent_cards.push_back(D2D1::RectF(x, l.recent_caption.bottom + Dip(scale, 4.0f), x + card_w, l.recent.bottom - Dip(scale, 10.0f)));
            const auto& card = l.recent_cards.back();
            l.recent_thumbnails.push_back(D2D1::RectF(card.left + Dip(scale, 6.0f), card.top + Dip(scale, 6.0f),
                card.right - Dip(scale, 6.0f), card.top + Dip(scale, 80.0f)));
            l.recent_names.push_back(D2D1::RectF(card.left + Dip(scale, 7.0f), card.top + Dip(scale, 86.0f),
                card.right - Dip(scale, 7.0f), card.top + Dip(scale, 106.0f)));
            l.recent_details.push_back(D2D1::RectF(card.left + Dip(scale, 7.0f), card.top + Dip(scale, 106.0f),
                card.right - Dip(scale, 7.0f), card.top + Dip(scale, 124.0f)));
            x += card_w + gap;
        }
    }
    {
        const float fy = l.footer.top + Dip(scale, 10.0f);
        const float fh = Dip(scale, kFieldH);
        const float label_w = std::min(painter.MeasureButtonWidth(chrome.filename_label) - Dip(scale, 16.0f),std::max(0.0f,(right-left)*0.2f));
        l.filename_label = D2D1::RectF(left, fy, left + std::max(0.0f, label_w), fy + fh);
        // The type list keeps a readable width; the name takes the rest.
        const float field_left = l.filename_label.right + Dip(scale, 8.0f);
        const float field_space = std::max(0.0f, right - field_left - Dip(scale, 8.0f));
        const float filter_w = std::min(Dip(scale, 264.0f), field_space * 0.45f);
        if (chrome.mode != PickerMode::Folder) l.filter = D2D1::RectF(right - filter_w, fy, right, fy + fh);
        l.filename = D2D1::RectF(l.filename_label.right + Dip(scale, 8.0f), fy,
                                 chrome.mode == PickerMode::Folder ? right : l.filter.left - Dip(scale, 8.0f), fy + fh);
    }
    if (!chrome.options.empty()) {
        const float y = l.filename.bottom + Dip(scale, 8.0f);
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
    if (chrome.selected_names.size() > 1) {
        float x = l.filename.left + Dip(scale, 4.0f);
        const float end = l.filename.right - Dip(scale, 4.0f);
        for (const auto& name : chrome.selected_names) {
            const float available = end - x;
            if (available < Dip(scale, 60.0f)) break;
            const float desired = painter.MeasureButtonWidth(name) + Dip(scale, 44.0f);
            const float tag_width = std::min(available, std::clamp(desired, Dip(scale, 60.0f), Dip(scale, 260.0f)));  // HTML .tc: natural width
            l.selection_tags.push_back(D2D1::RectF(x, l.filename.top + Dip(scale, 4.0f),
                x + tag_width, l.filename.bottom - Dip(scale, 4.0f)));
            x += tag_width + Dip(scale, 4.0f);
        }
    }
    return l;
}

int HitTestPickerChrome(const FolderPickerChromeLayout& l, const FolderPickerChrome& c,
                        float x, float y) {
    for (size_t i = 0; i < l.context_filters.size(); ++i)
        if (ContainsRect(l.context_filters[i], x, y)) return c.context_filters[i].id;
    if (c.overwrite_pending && ContainsRect(l.overwrite_replace, x, y)) return kPickOverwriteReplace;
    if (c.overwrite_pending && ContainsRect(l.overwrite_back, x, y)) return kPickOverwriteBack;
    if (c.overwrite_pending && ContainsRect(l.rename, x, y)) return kPickRename;
    if (ContainsRect(l.close, x, y)) return kPickClose;
    if (ContainsRect(l.more_options, x, y)) return kPickMoreOptions;
    for (size_t i = 0; i < l.option_chips.size(); ++i)
        if (ContainsRect(l.option_chips[i], x, y)) return c.options[i].enabled ? c.options[i].id : kPickNone;
    for (size_t i = 0; i < l.open_chips.size(); ++i)
        if (ContainsRect(l.open_chips[i], x, y)) return kPickOpenFolderBase + static_cast<int>(i);
    if (ContainsRect(l.last_location, x, y)) return kPickLastLocation;
    for (size_t i = 0; i < l.recent_cards.size(); ++i)
        if (ContainsRect(l.recent_cards[i], x, y)) return kPickRecentBase + static_cast<int>(i);
    if (!c.undo_jump_text.empty() && ContainsRect(l.undo_jump, x, y)) return kPickUndoJump;
    if (!c.new_folder_text.empty() && ContainsRect(l.new_folder, x, y)) return kPickNewFolder;
    if (c.footer_tool.id != 0 && ContainsRect(l.footer_tool, x, y)) return c.footer_tool.enabled ? c.footer_tool.id : kPickNone;
    if (ContainsRect(l.primary, x, y)) return c.primary_enabled ? (c.primary_drop_down && x >= l.primary.right - 28 * l.scale ? kPickPrimaryDropDown : kPickPrimary) : kPickNone;
    if (ContainsRect(l.cancel, x, y)) return kPickCancel;
    for (size_t i = 0; i < l.selection_tags.size(); ++i) {
        auto remove = l.selection_tags[i];
        remove.left = remove.right - Dip(l.scale, 24.0f);
        if (ContainsRect(remove, x, y)) return kPickRemoveSelectionBase + static_cast<int>(i);
    }
    if (ContainsRect(l.filename, x, y) || ContainsRect(l.filename_label, x, y)) return kPickFilename;
    if (ContainsRect(l.filter, x, y)) return kPickFilter;
    return kPickNone;
}

void DrawPickerChrome(Compositor& compositor, fluent::Painter& painter, const Theme& theme,
                      const FolderPickerChrome& c, const FolderPickerChromeLayout& l,
                      bool high_contrast) {
    const float scale = l.scale;
    const float radius = Dip(scale, theme.radius_control);

    painter.FillRoundedRect(l.title, 0, theme.surface_title);
    painter.FillRoundedRect(l.footer, 0, theme.surface_sheet);

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
        const std::wstring name = FitEnd(compositor, compositor.HeaderFormat(), c.app_name, name_max);
        const float name_w = std::min(name_max, typography::MeasureLine(&compositor, compositor.HeaderFormat(), name));
        const float name_x = Dip(scale, 45.0f);
        painter.DrawText(name, D2D1::RectF(name_x, 0, name_x + name_w + Dip(scale, 2.0f), l.title.bottom),
                         compositor.HeaderFormat(), theme.text);
        const float div_x = name_x + name_w + Dip(scale, 10.0f);
        painter.FillRoundedRect(D2D1::RectF(div_x, mid - Dip(scale, 7.0f), div_x + std::max(1.0f, Dip(scale, 1.0f)), mid + Dip(scale, 7.0f)),
                                0, theme.stroke_divider);
        const float verb_x = div_x + Dip(scale, 11.0f);
        painter.DrawText(FitEnd(compositor, compositor.TextFormat(), c.title, std::max(0.0f, l.close.left - Dip(scale, 8.0f) - verb_x)),
                         D2D1::RectF(verb_x, 0, l.close.left - Dip(scale, 8.0f), l.title.bottom), compositor.TextFormat(), theme.text_secondary);
    } else {
        const StoreIcon icon = c.mode == PickerMode::Image ? StoreIcon::Image
                             : c.mode == PickerMode::File || c.mode == PickerMode::Save ? StoreIcon::File : StoreIcon::Folder;
        DrawStoreIcon(dc, icon, D2D1::RectF(Dip(scale, 17.0f), mid - Dip(scale, 8.0f), Dip(scale, 33.0f), mid + Dip(scale, 8.0f)),
                      high_contrast ? theme.text : theme.accent, 1.4f);
        painter.DrawText(FitEnd(compositor, compositor.HeaderFormat(), c.title,
                                l.close.left - Dip(scale, 54.0f)),
                         D2D1::RectF(Dip(scale, 46.0f), 0, l.close.left - Dip(scale, 8.0f), l.title.bottom),
                         compositor.HeaderFormat(), theme.text);
    }
    fluent::ControlState close_state{};
    close_state.hovered = c.hover == kPickClose;
    close_state.pressed = c.pressed == kPickClose;
    painter.DrawTitleBarButton(l.close, fluent::TitleBarButtonRole::Close, {}, close_state);

    // 已打开: folders open in Pulse tabs and Explorer windows, Alt+1..9 (HTML 跳转条).
    if (l.strip.bottom > l.strip.top) {
        const float left = l.strip.left + Dip(scale, c.strip_left > 0.0f ? 12.0f : kSidePad);
        const float mid_y = (l.strip.top + l.strip.bottom) * 0.5f;
        const D2D1_COLOR_F muted = theme.text_secondary;
        const D2D1_COLOR_F faint = WithAlpha(theme.text, 0.42f);
        DrawCommandShape(dc, command_icons::Icon::Bolt, D2D1::RectF(left, mid_y - Dip(scale, 7.0f), left + Dip(scale, 14.0f), mid_y + Dip(scale, 7.0f)), faint, 1.3f);
        const float cap_x = left + Dip(scale, kChipIcon + 6.0f);
        const float cap_w = BodyWidth(painter, scale, c.strip_caption);
        painter.DrawText(c.strip_caption, D2D1::RectF(cap_x, l.strip.top, cap_x + cap_w + Dip(scale, 4.0f), l.strip.bottom),
                         compositor.TextFormat(), faint);
        auto chip = [&](const D2D1_RECT_F& r, const PickerOpenChip& item, int key_index, int id, bool recent, bool show_source) {
            const bool on = item.active;
            const bool hot = c.hover == id;
            painter.FillRoundedRect(r, Dip(scale, 7.0f), on ? WithAlpha(theme.accent, high_contrast ? 0.0f : 0.12f)
                                                  : hot ? WithAlpha(theme.text, 0.06f) : WithAlpha(theme.text, 0.03f));
            painter.StrokeRoundedRect(r, Dip(scale, 7.0f), on ? WithAlpha(theme.accent, 0.45f) : theme.stroke_divider, std::max(1.0f, Dip(scale, 1.0f)));
            float x = r.left + Dip(scale, 6.0f);
            if (key_index >= 0) {
                const std::wstring key = OpenFolderKey(key_index);
                const float kw = KeyWidth(painter, scale, key_index);
                const auto kr = D2D1::RectF(x, mid_y - Dip(scale, 9.0f), x + kw, mid_y + Dip(scale, 9.0f));
                painter.DrawBadge({kr, key, fluent::BadgeKind::Keycap});
                x = kr.right + Dip(scale, 7.0f);
            }
            const D2D1_COLOR_F ink = on && !high_contrast ? theme.accent : theme.text;
            const auto icon_box = D2D1::RectF(x, mid_y - Dip(scale, 7.0f), x + Dip(scale, 14.0f), mid_y + Dip(scale, 7.0f));
            if (recent) DrawCommandShape(dc, command_icons::Icon::History, icon_box, on ? ink : muted, 1.3f);
            else if (item.window) DrawWindowShape(painter, icon_box, on ? ink : muted, 1.3f);
            else DrawStoreIcon(dc, StoreIcon::Folder, icon_box, on ? ink : muted, 1.3f);
            x += Dip(scale, kChipIcon + 7.0f);
            const float source_w = item.source.empty() || !show_source ? 0.0f : CaptionWidth(painter, scale, item.source);
            const float label_w = std::max(0.0f, r.right - Dip(scale, 10.0f) - (source_w > 0 ? source_w + Dip(scale, 7.0f) : 0.0f) - x);
            painter.DrawText(FitEnd(compositor, compositor.TextFormat(), item.label, label_w), D2D1::RectF(x, r.top, x + label_w + Dip(scale, 2.0f), r.bottom),
                             compositor.TextFormat(), ink);
            if (source_w > 0)
                painter.DrawText(item.source, D2D1::RectF(r.right - Dip(scale, 10.0f) - source_w, r.top, r.right - Dip(scale, 8.0f), r.bottom),
                                 compositor.SmallFormat(), faint);
        };
        for (size_t i = 0; i < l.open_chips.size() && i < c.open_folders.size(); ++i)
            chip(l.open_chips[i], c.open_folders[i], static_cast<int>(i), kPickOpenFolderBase + static_cast<int>(i), false, i < l.open_chip_source.size() && l.open_chip_source[i]);
        if (l.last_location.right > l.last_location.left) {
            const float vx = l.last_location.left - Dip(scale, 6.0f);
            painter.FillRoundedRect(D2D1::RectF(vx, mid_y - Dip(scale, 9.0f), vx + std::max(1.0f, Dip(scale, 1.0f)), mid_y + Dip(scale, 9.0f)), 0, theme.stroke_divider);
            chip(l.last_location, c.last_location, -1, kPickLastLocation, true, true);
        }
        if (!c.undo_jump_text.empty() && l.undo_jump.right > l.undo_jump.left && l.auto_jump_notice.right <= l.auto_jump_notice.left) {
            fluent::ButtonSpec undo{l.undo_jump, c.undo_jump_text, L"\xE7A7", fluent::ButtonKind::Transparent, StateFor(c, kPickUndoJump)};
            undo.skip_glyph = true;
            painter.DrawButton(undo);
            DrawStoreIcon(dc, StoreIcon::Undo, GlyphSlot(l.undo_jump, scale), theme.text_secondary, 1.3f);
        }
        if (!c.new_folder_text.empty() && l.new_folder.right > l.new_folder.left) {
            fluent::ButtonSpec create{l.new_folder, c.new_folder_text, L"\xE8F4", fluent::ButtonKind::Standard, StateFor(c, kPickNewFolder)};
            create.skip_glyph = true;
            if (l.new_folder_keycap) create.keycap = c.new_folder_key;
            painter.DrawButton(create);
            DrawStoreIcon(dc, StoreIcon::NewFolder, GlyphSlot(l.new_folder, scale), high_contrast ? theme.text : theme.accent, 1.3f);
        }
        painter.FillRoundedRect(D2D1::RectF(l.strip.left, l.strip.bottom - 1.0f, l.width, l.strip.bottom), 0, theme.stroke_divider);
    }

    if (l.auto_jump_notice.right > l.auto_jump_notice.left) {
        auto notice = AutoJumpInfo(c, l.auto_jump_notice);
        const auto parts = painter.LayoutInfoBar(notice);
        notice.title = {};
        painter.DrawInfoBar(notice);
        painter.DrawText(FitEnd(compositor, compositor.SmallFormat(), c.auto_jump_notice,
            parts.title.right - parts.title.left), parts.title, compositor.SmallFormat(), theme.text);
    }
    for (size_t i = 0; i < l.context_filters.size(); ++i) {
        const auto& filter = c.context_filters[i];
        auto state = StateFor(c, filter.id, filter.enabled); state.checked = state.selected = filter.checked;
        const auto rect = l.context_filters[i];
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
        const auto text_rect = D2D1::RectF(tx, rect.top, rect.right - Dip(scale, 8.0f), rect.bottom);
        painter.DrawText(FitEnd(compositor, compositor.SmallFormat(), filter.text, text_rect.right - text_rect.left),
                         text_rect, compositor.SmallFormat(), ink);
        if (state.keyboard_focus)
            painter.StrokeRoundedRect(D2D1::RectF(rect.left - Dip(scale, 2.0f), rect.top - Dip(scale, 2.0f), rect.right + Dip(scale, 2.0f), rect.bottom + Dip(scale, 2.0f)),
                pill + Dip(scale, 2.0f), theme.accent, Dip(scale, 2.0f));
    }
    // Recent files across folders (open modes).
    if (l.recent.bottom > l.recent.top) {
        const D2D1_COLOR_F faint = WithAlpha(theme.text, 0.45f);
        const float cy = (l.recent_caption.top + l.recent_caption.bottom) * 0.5f;
        DrawStoreIcon(dc, StoreIcon::Undo, D2D1::RectF(l.recent_caption.left, cy - Dip(scale, 6.0f), l.recent_caption.left + Dip(scale, 12.0f), cy + Dip(scale, 6.0f)), faint, 1.2f);
        painter.DrawText(c.recent_caption, D2D1::RectF(l.recent_caption.left + Dip(scale, 18.0f), l.recent_caption.top, l.recent_caption.right, l.recent_caption.bottom),
                         compositor.SmallFormat(), faint);
        for (size_t i = 0; i < l.recent_cards.size() && i < c.recent_files.size(); ++i) {
            const auto& card = c.recent_files[i];
            const auto& r = l.recent_cards[i];
            const bool on = static_cast<int>(i) == c.recent_selected, hot = c.hover == kPickRecentBase + static_cast<int>(i);
            painter.FillRoundedRect(r, Dip(scale, 8.0f), high_contrast ? theme.bg : theme.surface_card);
            painter.StrokeRoundedRect(r, Dip(scale, 8.0f), on ? theme.accent : hot ? WithAlpha(theme.accent, 0.5f) : theme.stroke_card,
                                      on ? Dip(scale, 2.0f) : std::max(1.0f, Dip(scale, 1.0f)));
            const auto thumb = l.recent_thumbnails[i];
            bool cache_drawn = false;
            if (!card.thumbnail && c.draw_recent_thumbnail) {
                dc->PushAxisAlignedClip(thumb, D2D1_ANTIALIAS_MODE_ALIASED);
                try { cache_drawn = c.draw_recent_thumbnail(i, thumb); } catch (...) { cache_drawn = false; }
                dc->PopAxisAlignedClip();
            }
            if (card.thumbnail) {
                const D2D1_SIZE_F size = card.thumbnail->GetSize();
                // Fill the frame (centre crop), like the list's thumbnails.
                const float tw = thumb.right - thumb.left, th = thumb.bottom - thumb.top;
                const float k = std::max(tw / std::max(1.0f, size.width), th / std::max(1.0f, size.height));
                const float dw = size.width * k, dh = size.height * k;
                dc->PushAxisAlignedClip(thumb, D2D1_ANTIALIAS_MODE_ALIASED);
                dc->DrawBitmap(card.thumbnail, D2D1::RectF(thumb.left + (tw - dw) * 0.5f, thumb.top + (th - dh) * 0.5f,
                               thumb.left + (tw + dw) * 0.5f, thumb.top + (th + dh) * 0.5f), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                dc->PopAxisAlignedClip();
            } else if (!cache_drawn) {
                // No thumbnail: a quiet card in the type's colour with the file mark.
                const auto dot = card.name.find_last_of(L'.');
                std::wstring ext = dot == std::wstring::npos ? L"" : card.name.substr(dot + 1);
                CharLowerBuffW(ext.data(), static_cast<DWORD>(ext.size()));
                uint32_t rgb = 0x6B7787;
                if (ext == L"png" || ext == L"jpg" || ext == L"jpeg" || ext == L"webp" || ext == L"gif" || ext == L"heic") rgb = 0x7B61C9;
                else if (ext == L"psd") rgb = 0x1D6FB8; else if (ext == L"pdf") rgb = 0xD2493D;
                else if (ext == L"docx" || ext == L"doc") rgb = 0x2B6CC4; else if (ext == L"xlsx" || ext == L"xls") rgb = 0x1F7A45;
                else if (ext == L"pptx" || ext == L"ppt") rgb = 0xC9542F;
                painter.FillRoundedRect(thumb, Dip(scale, 5.0f), high_contrast ? theme.bg : HexColor(rgb, 0.16f));
                const float mx = (thumb.left + thumb.right) * 0.5f, my = (thumb.top + thumb.bottom) * 0.5f;
                DrawStoreIcon(dc, StoreIcon::File, D2D1::RectF(mx - Dip(scale, 14.0f), my - Dip(scale, 14.0f), mx + Dip(scale, 14.0f), my + Dip(scale, 14.0f)),
                              high_contrast ? theme.text : HexColor(rgb), 1.4f);
                if (!ext.empty()) {
                    std::wstring tag = ext;
                    CharUpperBuffW(tag.data(), static_cast<DWORD>(tag.size()));
                    painter.DrawText(tag, D2D1::RectF(thumb.left, thumb.bottom - Dip(scale, 20.0f), thumb.right - Dip(scale, 6.0f), thumb.bottom - Dip(scale, 2.0f)),
                                     compositor.SmallFormat(), high_contrast ? theme.text : HexColor(rgb), fluent::HorizontalAlignment::Right);
                }
            }
            const float tw = r.right - r.left - Dip(scale, 14.0f);
            painter.DrawText(FitEnd(compositor, compositor.TextFormat(), card.name, tw),
                             l.recent_names[i],
                             compositor.TextFormat(), theme.text);
            painter.DrawText(FitEnd(compositor, compositor.SmallFormat(), card.detail, tw),
                             l.recent_details[i],
                             compositor.SmallFormat(), theme.text_secondary);
        }
        painter.FillRoundedRect(D2D1::RectF(l.recent.left, l.recent.bottom - 1.0f, l.width, l.recent.bottom), 0, theme.stroke_divider);
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
        painter.DrawText(FitEnd(compositor,compositor.TextFormat(),c.filename_label,l.filename_label.right-l.filename_label.left), l.filename_label, compositor.TextFormat(), theme.text);
        fluent::ControlState field = StateFor(c, kPickFilename);
        field.focused = c.filename_focused;
        if (c.selected_names.size() <= 1) painter.DrawTextFieldFrame(l.filename, field, c.hosted_edit);
        if (!c.hosted_edit && l.selection_tags.empty() && !c.filename_text.empty()) {
            const D2D1_RECT_F inner = D2D1::RectF(l.filename.left + Dip(scale, 10.0f), l.filename.top,
                                                  l.filename.right - Dip(scale, 10.0f), l.filename.bottom);
            painter.DrawText(FitEnd(compositor, compositor.TextFormat(), c.filename_text,
                                    inner.right - inner.left),
                             inner, compositor.TextFormat(), theme.text);
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
            const auto text_rect = D2D1::RectF(tag.left + Dip(scale, 26.0f), tag.top,
                tag.right - Dip(scale, 24.0f), tag.bottom);
            painter.DrawText(FitEnd(compositor, compositor.TextFormat(), c.selected_names[i],
                std::max(0.0f, text_rect.right - text_rect.left)), text_rect, compositor.TextFormat(), theme.text);
            const auto remove = D2D1::RectF(tag.right - Dip(scale, 24.0f), tag.top, tag.right, tag.bottom);
            painter.DrawButton({remove, L"\x00D7", {}, fluent::ButtonKind::Transparent,
                StateFor(c, kPickRemoveSelectionBase + static_cast<int>(i))});
        }
        fluent::ButtonSpec filter{l.filter, c.filter_text, {}, fluent::ButtonKind::Standard,
                                 StateFor(c, kPickFilter)};
        filter.drop_down = true;
        filter.chevron_turn = c.filter_turn;
        if (l.filter.right > l.filter.left) painter.DrawButton(filter);
    }

    // What will be picked (or why not), then the buttons.
    const bool notice = !c.notice.empty();
    const std::wstring& line = notice ? c.notice : c.summary;
    if (c.overwrite_pending && l.conflict.right > l.conflict.left) {
        painter.FillRoundedRect(l.conflict, Dip(scale, 7.0f), high_contrast ? theme.bg : WithAlpha(theme.danger, 0.08f));
        if (high_contrast) painter.StrokeRoundedRect(l.conflict, Dip(scale, 7.0f), theme.danger, std::max(1.0f, Dip(scale, 1.0f)));
        const float cy = (l.conflict.top + l.conflict.bottom) * 0.5f;
        DrawStoreIcon(dc, StoreIcon::Warn, D2D1::RectF(l.conflict.left + Dip(scale, 10.0f), cy - Dip(scale, 8.0f), l.conflict.left + Dip(scale, 26.0f), cy + Dip(scale, 8.0f)),
                      theme.danger, 1.4f);
    }
    if (c.overwrite_pending && notice)  // the question sits in the banner; the target stays on its row
        painter.DrawText(FitEnd(compositor, compositor.TextFormat(), c.notice, l.conflict_text.right - l.conflict_text.left),
                         l.conflict_text, compositor.TextFormat(), theme.text);
    if ((!notice || c.overwrite_pending) && !c.summary_folder.empty()) {
        // 将保存到：D:\工作\季度报告\ name.docx — the folder reads first, the leaf quieter.
        const float avail = l.summary.right - l.summary.left - Dip(scale, 22.0f);
        const float x0 = l.summary.left + Dip(scale, 22.0f);
        const float mid_y = (l.summary.top + l.summary.bottom) * 0.5f;
        DrawStoreIcon(dc, StoreIcon::Folder, D2D1::RectF(l.summary.left + Dip(scale, 1.0f), mid_y - Dip(scale, 7.0f), l.summary.left + Dip(scale, 15.0f), mid_y + Dip(scale, 7.0f)),
                      high_contrast ? theme.text : HexColor(0xD8A43E), 1.3f);
        const float lead_w = std::min(avail, typography::MeasureLine(&compositor, compositor.TextFormat(), c.summary_lead));
        painter.DrawText(c.summary_lead, D2D1::RectF(x0, l.summary.top, x0 + lead_w + 1.0f, l.summary.bottom), compositor.TextFormat(), theme.text_secondary);
        const float rest = std::max(0.0f, avail - lead_w);
        const float leaf_full = typography::MeasureLine(&compositor, compositor.TextFormat(), c.summary_leaf);
        const float folder_full = typography::MeasureLine(&compositor, compositor.TextFormat(), c.summary_folder);
        const float folder_w = std::min(folder_full, std::max(rest * 0.5f, rest - leaf_full));
        const float fx = x0 + lead_w;
        painter.DrawText(FitEnd(compositor, compositor.TextFormat(), c.summary_folder, folder_w), D2D1::RectF(fx, l.summary.top, fx + folder_w + 1.0f, l.summary.bottom),
                         compositor.TextFormat(), theme.text);
        const float lx = fx + folder_w;
        painter.DrawText(FitEnd(compositor, compositor.TextFormat(), c.summary_leaf, std::max(0.0f, rest - folder_w)),
                         D2D1::RectF(lx, l.summary.top, l.summary.right, l.summary.bottom), compositor.TextFormat(), theme.text_secondary);
    } else if (!notice && c.selected_names.size() > 1 && !c.selection_summary.empty()) {
        // HTML: ✓ 3 个文件 · 9.4 MB, the check in the accent where the folder icon sits otherwise.
        const float mid_y = (l.summary.top + l.summary.bottom) * 0.5f;
        DrawStoreIcon(dc, StoreIcon::Check, D2D1::RectF(l.summary.left + Dip(scale, 1.0f), mid_y - Dip(scale, 7.0f), l.summary.left + Dip(scale, 15.0f), mid_y + Dip(scale, 7.0f)),
                      high_contrast ? theme.text : theme.accent, 1.6f);
        const float x0 = l.summary.left + Dip(scale, 22.0f);
        const float total = std::max(0.0f, l.summary.right - x0);
        const float summary_w = std::min(total, BodyWidth(painter, scale, c.selection_summary));
        const auto main = D2D1::RectF(x0, l.summary.top, x0 + summary_w, l.summary.bottom);
        painter.DrawText(FitEnd(compositor, compositor.TextFormat(), c.selection_summary, summary_w), main, compositor.TextFormat(), theme.text);
        const auto hint = D2D1::RectF(main.right + Dip(scale, 8.0f), l.summary.top, l.summary.right, l.summary.bottom);
        if (!c.selection_hint.empty() && hint.right - hint.left >= Dip(scale, 80.0f))
            painter.DrawText(FitEnd(compositor, compositor.SmallFormat(), c.selection_hint, hint.right - hint.left), hint, compositor.SmallFormat(), theme.text_secondary);
    } else if (!line.empty() && !c.overwrite_pending) {
        painter.DrawText(FitEnd(compositor, compositor.TextFormat(), line,
                                l.summary.right - l.summary.left),
                         l.summary, compositor.TextFormat(),
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
    if (l.footer_tool.right > l.footer_tool.left) {
        fluent::ButtonSpec tool{l.footer_tool, c.footer_tool.text, {}, fluent::ButtonKind::Standard,
            StateFor(c, c.footer_tool.id, c.footer_tool.enabled)};
        tool.drop_down = true;
        painter.DrawButton(tool);
    }
    painter.DrawButton({l.cancel, FitEnd(compositor, compositor.TextFormat(), c.cancel_text, std::max(0.0f, l.cancel.right - l.cancel.left - Dip(scale, 24.0f))), {}, fluent::ButtonKind::Standard,
                        StateFor(c, kPickCancel)});
    const fluent::ControlState primary = StateFor(c, kPickPrimary, c.primary_enabled);
    auto primary_rect = l.primary;
    if (c.primary_drop_down) primary_rect.right -= Dip(scale, 28.0f);
    painter.DrawButton({primary_rect, FitEnd(compositor,compositor.TextFormat(),c.primary_text,std::max(0.0f,primary_rect.right-primary_rect.left-Dip(scale,24.0f))), {}, fluent::ButtonKind::Primary, primary});
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

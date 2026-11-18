#include "../common/windows_compat.h"
#include "folder_picker_view.h"

#include "../common/localization.h"
#include "typography.h"
#include "window_helpers.h"

#include <algorithm>
#include <cmath>

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

} // namespace

float PickerFooterDip(PickerMode) {
    return 140.0f;
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
    l.footer = D2D1::RectF(0, height - Dip(scale, PickerFooterDip(chrome.mode)), width, height);

    const float left = Dip(scale, kSidePad);
    const float right = width - Dip(scale, kSidePad);
    const float btn_h = painter.MeasureButtonHeight();
    // The button row is the last one; files get the name / type row above it.
    const float button_band = Dip(scale, 60.0f);
    const float by = height - button_band + (button_band - btn_h) * 0.5f;
    const float primary_w = std::min(std::max(painter.MeasureButtonWidth(chrome.primary_text) + (chrome.primary_drop_down ? Dip(scale,28.0f) : 0.0f),
                                     Dip(scale, kButtonMinW)),std::max(0.0f,(right-left-Dip(scale,8.0f))*0.5f));
    const float cancel_w = std::min(std::max(painter.MeasureButtonWidth(chrome.cancel_text),
                                    Dip(scale, kButtonMinW)), primary_w);
    l.primary = D2D1::RectF(right - primary_w, by, right, by + btn_h);
    l.cancel = D2D1::RectF(l.primary.left - Dip(scale, 8.0f) - cancel_w, by,
                           l.primary.left - Dip(scale, 8.0f), by + btn_h);
    l.summary = D2D1::RectF(left, l.footer.top + Dip(scale, 48.0f), right,
                               l.footer.top + Dip(scale, 72.0f));
    if (chrome.overwrite_pending) {
        const float gap = Dip(scale, 8.0f);
        const float cell = std::max(0.0f, (right - left - gap * 3.0f) / 4.0f);
        l.overwrite_back = D2D1::RectF(left, by, left + cell, by + btn_h);
        l.rename = D2D1::RectF(left + cell + gap, by, left + cell * 2.0f + gap, by + btn_h);
        l.cancel = D2D1::RectF(left + (cell + gap) * 2.0f, by, right - cell - gap, by + btn_h);
        l.primary = D2D1::RectF(right - cell, by, right, by + btn_h);
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
    if (chrome.selected_names.size() > 1) {
        float x = l.filename.left + Dip(scale, 4.0f);
        const float end = l.filename.right - Dip(scale, 4.0f);
        for (const auto& name : chrome.selected_names) {
            const float available = end - x;
            if (available < Dip(scale, 60.0f)) break;
            const float desired = painter.MeasureButtonWidth(name) + Dip(scale, 24.0f);
            const float tag_width = std::min(available, std::clamp(desired, Dip(scale, 60.0f), Dip(scale, 180.0f)));
            l.selection_tags.push_back(D2D1::RectF(x, l.filename.top + Dip(scale, 4.0f),
                x + tag_width, l.filename.bottom - Dip(scale, 4.0f)));
            x += tag_width + Dip(scale, 4.0f);
        }
    }
    return l;
}

int HitTestPickerChrome(const FolderPickerChromeLayout& l, const FolderPickerChrome& c,
                        float x, float y) {
    if (c.overwrite_pending && ContainsRect(l.overwrite_back, x, y)) return kPickOverwriteBack;
    if (c.overwrite_pending && ContainsRect(l.rename, x, y)) return kPickRename;
    if (ContainsRect(l.close, x, y)) return kPickClose;
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

    // Title strip: what the caller asked for, and the close button.
    const wchar_t* glyph = c.mode == PickerMode::Image ? L"\xEB9F"
                         : c.mode == PickerMode::File || c.mode == PickerMode::Save ? L"\xE8A5" : L"\xE8B7";
    painter.DrawGlyph(glyph, D2D1::RectF(Dip(scale, 14.0f), 0, Dip(scale, 38.0f), l.title.bottom),
                      high_contrast ? theme.text : theme.accent);
    painter.DrawText(FitEnd(compositor, compositor.HeaderFormat(), c.title,
                            l.close.left - Dip(scale, 54.0f)),
                     D2D1::RectF(Dip(scale, 46.0f), 0, l.close.left - Dip(scale, 8.0f), l.title.bottom),
                     compositor.HeaderFormat(), theme.text);
    fluent::ControlState close_state{};
    close_state.hovered = c.hover == kPickClose;
    close_state.pressed = c.pressed == kPickClose;
    painter.DrawTitleBarButton(l.close, fluent::TitleBarButtonRole::Close, {}, close_state);

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
        painter.DrawTextFieldFrame(l.filename, field, c.hosted_edit);
        if (!c.hosted_edit && l.selection_tags.empty() && !c.filename_text.empty()) {
            const D2D1_RECT_F inner = D2D1::RectF(l.filename.left + Dip(scale, 10.0f), l.filename.top,
                                                  l.filename.right - Dip(scale, 10.0f), l.filename.bottom);
            painter.DrawText(FitEnd(compositor, compositor.TextFormat(), c.filename_text,
                                    inner.right - inner.left),
                             inner, compositor.TextFormat(), theme.text);
        }
        for (size_t i = 0; i < l.selection_tags.size() && i < c.selected_names.size(); ++i) {
            const auto tag = l.selection_tags[i];
            painter.FillRoundedRect(tag, Dip(scale, 6.0f), theme.stroke_divider);
            const auto text_rect = D2D1::RectF(tag.left + Dip(scale, 8.0f), tag.top,
                tag.right - Dip(scale, 24.0f), tag.bottom);
            painter.DrawText(FitEnd(compositor, compositor.TextFormat(), c.selected_names[i],
                std::max(0.0f, text_rect.right - text_rect.left)), text_rect, compositor.TextFormat(), theme.text);
            const auto remove = D2D1::RectF(tag.right - Dip(scale, 24.0f), tag.top, tag.right, tag.bottom);
            painter.DrawButton({remove, L"\x00D7", {}, fluent::ButtonKind::Standard,
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
    if (!line.empty()) {
        painter.DrawText(FitEnd(compositor, compositor.TextFormat(), line,
                                l.summary.right - l.summary.left),
                         l.summary, compositor.TextFormat(),
                         c.overwrite_pending ? theme.text : notice ? theme.danger : theme.text_secondary);
    }
    if (c.overwrite_pending) {
        painter.DrawButton({l.overwrite_back, FitEnd(compositor, compositor.TextFormat(), c.overwrite_back_text, std::max(0.0f, l.overwrite_back.right - l.overwrite_back.left - Dip(scale, 24.0f))), {}, fluent::ButtonKind::Standard, StateFor(c, kPickOverwriteBack)});
        painter.DrawButton({l.rename, FitEnd(compositor, compositor.TextFormat(), c.rename_text, std::max(0.0f, l.rename.right - l.rename.left - Dip(scale, 24.0f))), {}, fluent::ButtonKind::Standard, StateFor(c, kPickRename)});
    }
    painter.DrawButton({l.cancel, c.cancel_text, {}, fluent::ButtonKind::Standard,
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

} // namespace pulse::ui

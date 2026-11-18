#include "../common/windows_compat.h"
#include "confirm_dialog_view.h"

#include "../common/display_path.h"
#include "../common/localization.h"
#include "typography.h"
#include "window_helpers.h"

#include <algorithm>
#include <cmath>
#include <cwchar>

namespace pulse::ui {
namespace {

constexpr float kMinWidth = 420.0f;
constexpr float kMaxWidth = 540.0f;
constexpr float kPad = 24.0f;
// The same modal as the file-operation
// dialogs. Headline at 18, the message under it, items in a bordered list of
// 34-DIP rows, then a 64-DIP footer band with the buttons on the right.
constexpr float kTitleBar = 40.0f;
constexpr float kRowH = 34.0f;
constexpr float kButtonMinW = 80.0f;
constexpr float kButtonGap = 8.0f;
constexpr float kFooterH = 64.0f;
// Prefer a wider dialog over a message that wraps into a tall column.
constexpr float kComfortableLines = 4.0f;

float Dip(float scale, float value) { return value * scale; }

float ButtonWidth(const fluent::Painter& painter, const std::wstring& text, float scale) {
    return std::max(painter.MeasureButtonWidth(text), Dip(scale, kButtonMinW));
}

// One group of the dialog rising in: drawn 6 DIP lower and faded until its
// spring reaches 1. A settled group (k == 1) costs nothing.
class RiseIn {
public:
    RiseIn(Compositor& compositor, float k, float rise) {
        if (k == 1.0f) return;
        dc_ = compositor.Dc();
        if (!dc_) return;
        dc_->GetTransform(&old_);
        dc_->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, (1.0f - k) * rise) * old_);
        const float opacity = std::clamp(k * 1.6f, 0.0f, 1.0f);
        if (opacity < 1.0f) {
            dc_->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr,
                                                  D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                  D2D1::IdentityMatrix(), opacity, nullptr,
                                                  D2D1_LAYER_OPTIONS1_NONE),
                           nullptr);
            layered_ = true;
        }
    }
    ~RiseIn() {
        if (!dc_) return;
        if (layered_) dc_->PopLayer();
        dc_->SetTransform(old_);
    }
    RiseIn(const RiseIn&) = delete;
    RiseIn& operator=(const RiseIn&) = delete;

private:
    ID2D1DeviceContext* dc_ = nullptr;
    D2D1_MATRIX_3X2_F old_{};
    bool layered_ = false;
};

// Scales (and turns) what is drawn while alive around `centre`.
class AroundCentre {
public:
    AroundCentre(Compositor& compositor, D2D1_POINT_2F centre, float scale, float degrees) {
        if (scale == 1.0f && degrees == 0.0f) return;
        dc_ = compositor.Dc();
        if (!dc_) return;
        dc_->GetTransform(&old_);
        dc_->SetTransform(D2D1::Matrix3x2F::Rotation(degrees, centre) *
                          D2D1::Matrix3x2F::Scale(scale, scale, centre) * old_);
    }
    ~AroundCentre() { if (dc_) dc_->SetTransform(old_); }
    AroundCentre(const AroundCentre&) = delete;
    AroundCentre& operator=(const AroundCentre&) = delete;

private:
    ID2D1DeviceContext* dc_ = nullptr;
    D2D1_MATRIX_3X2_F old_{};
};

D2D1_POINT_2F Centre(const D2D1_RECT_F& r) {
    return D2D1::Point2F((r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f);
}

float Wrapped(IDWriteFactory2* factory, IDWriteTextFormat* format, std::wstring_view text,
              float width) {
    if (!factory || !format || text.empty() || width <= 0.0f) return 0.0f;
    return std::ceil(typography::MeasureWrapped(factory, format, text, width));
}

} // namespace

bool ConfirmDialogFormats::Create(IDWriteFactory2* factory, float scale) {
    title.reset();
    body.reset();
    note.reset();
    if (!factory) return false;
    typography::CreateTextFormat(factory,
        {typography::FontRole::Display, 18.0f * scale, DWRITE_FONT_WEIGHT_SEMI_BOLD}, &title);
    typography::CreateTextFormat(factory, {typography::FontRole::Text, 13.0f * scale}, &body);
    typography::CreateTextFormat(factory, {typography::FontRole::Text, 12.0f * scale}, &note);
    for (IDWriteTextFormat* format : {title.get(), body.get(), note.get()}) {
        if (!format) return false;
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
    return true;
}

ConfirmDialogSpec NormalizeConfirmSpec(ConfirmDialogSpec spec) {
    spec.message = path::FriendlyPathText(spec.message);
    spec.note = path::FriendlyPathText(spec.note);
    for (std::wstring& item : spec.items) item = path::FriendlyPathText(item);
    if (spec.confirm_text.empty())
        spec.confirm_text = l10n::Get(l10n::StringId::ConfirmDefault);
    if (spec.cancel_text.empty() && !spec.single_button) spec.cancel_text = l10n::Get(l10n::StringId::Cancel);
    // Trailing blank lines from older prompt strings would pad the layout.
    while (!spec.message.empty() && (spec.message.back() == L'\n' || spec.message.back() == L' '))
        spec.message.pop_back();
    return spec;
}

ConfirmTone ResolveConfirmTone(const ConfirmDialogSpec& spec) {
    if (spec.tone != ConfirmTone::Auto) return spec.tone;
    return spec.danger ? ConfirmTone::Danger : ConfirmTone::Question;
}

size_t ConfirmItemCount(const ConfirmDialogSpec& spec) {
    return (std::max)(spec.items.size(), spec.items_total);
}

std::vector<std::wstring> ConfirmItemRows(const ConfirmDialogSpec& spec) {
    const size_t count = ConfirmItemCount(spec);
    if (count <= kConfirmMaxItemRows && count == spec.items.size()) return spec.items;
    const size_t listed = (std::min)(spec.items.size(), kConfirmMaxItemRows - 1);
    std::vector<std::wstring> rows(spec.items.begin(), spec.items.begin() + listed);
    wchar_t more[128]{};
    swprintf_s(more, l10n::Get(l10n::StringId::ConfirmMoreItemsFormat).c_str(), count - listed);
    rows.emplace_back(more);
    return rows;
}

std::wstring FitConfirmItemText(const std::wstring& text, float width,
                                const std::function<float(std::wstring_view)>& measure) {
    return FitPathMiddle(text, width, measure);
}

bool ConfirmCheckable(const ConfirmDialogSpec& spec) {
    return spec.item_checks && !spec.items.empty() &&
           spec.item_checks->size() == spec.items.size() &&
           spec.items.size() <= kConfirmMaxItemRows && spec.items_total <= spec.items.size();
}

bool ConfirmRowChecked(const ConfirmDialogSpec& spec, size_t row) {
    return ConfirmCheckable(spec) && row < spec.item_checks->size() && (*spec.item_checks)[row];
}

bool ConfirmAnyChecked(const ConfirmDialogSpec& spec) {
    if (!ConfirmCheckable(spec)) return true;
    for (const bool checked : *spec.item_checks)
        if (checked) return true;
    return false;
}

std::vector<int> ConfirmFocusOrder(const ConfirmDialogSpec& spec) {
    std::vector<int> order;
    if (ConfirmCheckable(spec)) {
        for (size_t i = 0; i < spec.items.size(); ++i)
            order.push_back(kConfirmRowBase + static_cast<int>(i));
    }
    if (spec.single_button) order.push_back(kConfirmPrimary);
    else if (spec.secondary_text.empty()) order.insert(order.end(), {kConfirmCancel, kConfirmPrimary});
    else order.insert(order.end(), {kConfirmCancel, kConfirmSecondary, kConfirmPrimary});
    return order;
}

int ConfirmControlFor(ConfirmChoice choice, bool has_secondary) {
    switch (choice) {
    case ConfirmChoice::Confirm: return kConfirmPrimary;
    case ConfirmChoice::Secondary: return has_secondary ? kConfirmSecondary : kConfirmCancel;
    default: return kConfirmCancel;
    }
}

std::wstring ConfirmClipboardText(const ConfirmDialogSpec& spec) {
    std::wstring text = spec.title;
    if (!spec.message.empty()) text += L"\r\n\r\n" + spec.message;
    const bool checkable = ConfirmCheckable(spec);
    for (size_t i = 0; i < spec.items.size(); ++i) {
        text += L"\r\n  ";
        if (checkable) text += ConfirmRowChecked(spec, i) ? L"[x] " : L"[ ] ";
        text += spec.items[i];
    }
    if (ConfirmItemCount(spec) > spec.items.size()) {
        wchar_t more[128]{};
        swprintf_s(more, l10n::Get(l10n::StringId::ConfirmMoreItemsFormat).c_str(),
                   ConfirmItemCount(spec) - spec.items.size());
        text += L"\r\n  " + std::wstring(more);
    }
    if (!spec.note.empty()) text += L"\r\n\r\n" + spec.note;
    return text;
}

ConfirmDialogLayout LayoutConfirmDialog(const ConfirmDialogSpec& spec,
                                        const fluent::Painter& painter,
                                        IDWriteFactory2* factory,
                                        const ConfirmDialogFormats& formats, float scale) {
    ConfirmDialogLayout l;
    const float pad = Dip(scale, kPad);
    const float gap = Dip(scale, kButtonGap);
    const float btn_h = painter.MeasureButtonHeight();
    const float primary_w = ButtonWidth(painter, spec.confirm_text, scale);
    const float cancel_w = spec.single_button ? 0.0f : ButtonWidth(painter, spec.cancel_text, scale);
    const float secondary_w = spec.secondary_text.empty()
        ? 0.0f : ButtonWidth(painter, spec.secondary_text, scale);
    const float buttons_w = primary_w + cancel_w +
        (secondary_w > 0.0f ? secondary_w + gap : 0.0f) + (cancel_w > 0.0f ? gap : 0.0f);
    const float text_x = pad;

    // Widen until the message fits in a few lines or the cap is reached.
    float width = std::max(Dip(scale, kMinWidth), buttons_w + pad * 2.0f);
    if (formats.body.get() && factory) {
        const float line = Wrapped(factory, formats.body.get(), L"A", 1000.0f);
        while (width < Dip(scale, kMaxWidth)) {
            const float h = Wrapped(factory, formats.body.get(), spec.message,
                                    width - text_x - pad);
            if (h <= line * kComfortableLines + 0.5f) break;
            width = std::min(width + Dip(scale, 20.0f), Dip(scale, kMaxWidth));
        }
    }
    width = std::ceil(width);
    const float text_w = width - text_x - pad;

    l.title_bar = Dip(scale, kTitleBar);
    l.close = D2D1::RectF(width - Dip(scale, 44.0f), Dip(scale, 4.0f), width - Dip(scale, 4.0f),
                          Dip(scale, 36.0f));
    l.icon = D2D1::RectF(0, 0, 0, 0);
    // The headline keeps clear of the close button.
    float y = Dip(scale, 18.0f);
    const float title_w = width - text_x - Dip(scale, 48.0f);
    const float title_h = std::max(Wrapped(factory, formats.title.get(), spec.title, title_w),
                                   Dip(scale, 24.0f));
    l.title = D2D1::RectF(text_x, y, text_x + title_w, y + title_h);
    y = l.title.bottom + Dip(scale, 4.0f);

    const float msg_h = Wrapped(factory, formats.body.get(), spec.message, text_w);
    l.message = D2D1::RectF(text_x, y, width - pad, y + msg_h);
    if (msg_h > 0.0f) y = l.message.bottom;
    float after = 24.0f;

    const std::vector<std::wstring> rows = ConfirmItemRows(spec);
    if (!rows.empty()) {
        y += Dip(scale, 16.0f);
        const float row_h = Dip(scale, kRowH);
        const float border = 1.0f;
        l.card = D2D1::RectF(text_x, y, width - pad,
                             y + row_h * static_cast<float>(rows.size()) + border * 2.0f);
        for (size_t i = 0; i < rows.size(); ++i) {
            const float top = y + border + row_h * static_cast<float>(i);
            l.rows.push_back(D2D1::RectF(l.card.left, top, l.card.right, top + row_h));
        }
        y = l.card.bottom;
        l.checkable = ConfirmCheckable(spec);
        after = 20.0f;
    }
    if (!spec.note.empty()) {
        y += Dip(scale, 12.0f);
        const float note_h = Wrapped(factory, formats.note.get(), spec.note, text_w);
        l.note = D2D1::RectF(text_x, y, width - pad, y + note_h);
        y = l.note.bottom;
        after = 24.0f;
    }

    y = std::round(y + Dip(scale, after));
    const float footer_h = Dip(scale, kFooterH);
    l.footer = D2D1::RectF(0, y, width, y + footer_h);
    const float by = y + (footer_h - btn_h) * 0.5f;
    float right = width - pad;
    l.primary = D2D1::RectF(right - primary_w, by, right, by + btn_h);
    right = l.primary.left - gap;
    if (secondary_w > 0.0f) {
        l.secondary = D2D1::RectF(right - secondary_w, by, right, by + btn_h);
        right = l.secondary.left - gap;
    }
    // Single button: an empty cancel rect, so nothing draws or hits there.
    l.cancel = spec.single_button ? D2D1::RectF() : D2D1::RectF(right - cancel_w, by, right, by + btn_h);
    l.width = width;
    l.height = std::ceil(l.footer.bottom);
    return l;
}

int HitTestConfirmDialog(const ConfirmDialogLayout& layout, bool has_secondary,
                         float x, float y) {
    if (ContainsRect(layout.close, x, y)) return kConfirmClose;
    if (ContainsRect(layout.primary, x, y)) return kConfirmPrimary;
    if (has_secondary && ContainsRect(layout.secondary, x, y)) return kConfirmSecondary;
    if (ContainsRect(layout.cancel, x, y)) return kConfirmCancel;
    if (layout.checkable) {
        for (size_t i = 0; i < layout.rows.size(); ++i)
            if (ContainsRect(layout.rows[i], x, y)) return kConfirmRowBase + static_cast<int>(i);
    }
    return kConfirmNone;
}

void DrawDialogParagraph(Compositor& compositor, IDWriteTextFormat* format,
                         const D2D1_RECT_F& bounds, std::wstring_view text,
                         const D2D1_COLOR_F& color) {
    auto* factory = compositor.DwriteFactory();
    auto* dc = compositor.Dc();
    if (!factory || !dc || !format || text.empty()) return;
    const float width = bounds.right - bounds.left;
    const float height = std::max(bounds.bottom - bounds.top, 1.0f);
    if (width <= 0.0f) return;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()),
                                         format, width, height, &layout)) || !layout.get())
        return;
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(color, &brush)) || !brush.get()) return;
    compositor.DrawTextLayout(D2D1::Point2F(bounds.left, bounds.top), layout.get(), brush.get());
}

void DrawConfirmDialog(Compositor& compositor, fluent::Painter& painter, const Theme& theme,
                       const ConfirmDialogSpec& spec, const ConfirmDialogLayout& l,
                       const ConfirmDialogFormats& formats, const ConfirmDialogVisual& visual,
                       bool dark, bool high_contrast) {
    const float scale = painter.Scale();

    fluent::ControlState close_state{};
    close_state.hovered = visual.hover == kConfirmClose;
    close_state.pressed = visual.pressed == kConfirmClose;
    painter.DrawTitleBarButton(l.close, fluent::TitleBarButtonRole::Close, {}, close_state);

    const float rise = Dip(scale, 6.0f);
    {
        RiseIn in(compositor, visual.body_in, rise);
        DrawDialogParagraph(compositor, formats.title.get(), l.title, spec.title, theme.text);
        DrawDialogParagraph(compositor, formats.body.get(), l.message, spec.message,
                            high_contrast ? theme.text : theme.text_secondary);
    }

    const std::vector<std::wstring> rows = ConfirmItemRows(spec);
    if (!rows.empty()) {
        const D2D1_COLOR_F stroke = high_contrast ? theme.text
            : dark ? HexColor(0xFFFFFF, 0.0698f) : HexColor(0x000000, 0.0578f);
        {
            RiseIn in(compositor, visual.rows_in[0], rise);
            painter.StrokeRoundedRect(D2D1::RectF(l.card.left + 0.5f, l.card.top + 0.5f,
                                                  l.card.right - 0.5f, l.card.bottom - 0.5f),
                                      Dip(scale, 6.0f), stroke, 1.0f);
        }
        const std::wstring_view glyph = spec.item_glyph.empty()
            ? std::wstring_view(L"\xE8A5") : std::wstring_view(spec.item_glyph);
        const bool truncated = ConfirmItemCount(spec) > rows.size();
        for (size_t i = 0; i < rows.size() && i < l.rows.size(); ++i) {
            const D2D1_RECT_F& row = l.rows[i];
            RiseIn in(compositor, visual.rows_in[std::min(i, kConfirmMaxItemRows - 1)], rise);
            if (i > 0) {
                const float line = std::round(row.top);
                painter.FillRoundedRect(D2D1::RectF(row.left + 1.0f, line, row.right - 1.0f,
                                                    line + 1.0f),
                                        0, theme.stroke_divider);
            }
            const bool more_row = truncated && i + 1 == rows.size();
            const int row_id = kConfirmRowBase + static_cast<int>(i);
            if (l.checkable) {
                if (visual.hover == row_id && !high_contrast) {
                    painter.FillRoundedRect(D2D1::RectF(row.left + Dip(scale, 4.0f), row.top + Dip(scale, 3.0f),
                                                        row.right - Dip(scale, 4.0f), row.bottom - Dip(scale, 3.0f)),
                                            Dip(scale, 4.0f), theme.fill_hover);
                }
                fluent::ControlState check{};
                check.checked = ConfirmRowChecked(spec, i);
                check.hovered = visual.hover == row_id;
                check.pressed = visual.pressed == row_id && visual.hover == row_id;
                painter.DrawCheckBox(D2D1::RectF(row.left + Dip(scale, 11.0f), row.top,
                                                 row.left + Dip(scale, 31.0f), row.bottom),
                                     L"", check);
                if (visual.show_focus && !visual.focus_glide && visual.focus == row_id)
                    painter.DrawFocusRing(row, Dip(scale, theme.radius_control));
            } else if (!more_row) {
                const std::wstring_view own = i < spec.item_glyphs.size() && !spec.item_glyphs[i].empty()
                    ? std::wstring_view(spec.item_glyphs[i]) : glyph;
                painter.DrawGlyph(own, D2D1::RectF(row.left + Dip(scale, 12.0f), row.top,
                                                     row.left + Dip(scale, 30.0f), row.bottom),
                                  theme.text_secondary);
            }
            const D2D1_RECT_F text_rect = D2D1::RectF(
                row.left + Dip(scale, more_row ? 13.0f : 39.0f), row.top,
                row.right - Dip(scale, 13.0f), row.bottom);
            const std::wstring text = more_row ? rows[i] : FitConfirmItemText(
                rows[i], text_rect.right - text_rect.left,
                [&](std::wstring_view s) {
                    return typography::MeasureLine(&compositor, compositor.TextFormat(), s);
                });
            painter.DrawText(text, text_rect,
                             compositor.TextFormat(),
                             more_row ? theme.text_secondary : theme.text);
        }
    }
    {
        RiseIn in(compositor, visual.body_in, rise);
        DrawDialogParagraph(compositor, formats.note.get(), l.note, spec.note,
                            high_contrast ? theme.text : theme.text_secondary);
    }

    // Footer band: the one the file-operation dialogs share.
    painter.FillRoundedRect(l.footer, 0, high_contrast ? theme.bg
                            : dark ? HexColor(0x000000, 0.2f) : HexColor(0x000000, 0.045f));
    painter.FillRoundedRect(D2D1::RectF(l.footer.left, l.footer.top, l.footer.right,
                                        l.footer.top + 1.0f),
                            0, theme.stroke_divider);

    auto state_for = [&](int id) {
        fluent::ControlState state{};
        state.hovered = visual.hover == id;
        state.pressed = visual.pressed == id;
        state.keyboard_focus = visual.show_focus && visual.focus == id && !visual.focus_glide;
        return state;
    };
    // A button held down (pointer still on it) sinks a little.
    auto pressed_scale = [&](int id) {
        return visual.pressed == id && visual.hover == id && !high_contrast ? 0.97f : 1.0f;
    };
    {
        RiseIn in(compositor, visual.footer_in, rise);
        if (!spec.single_button) {
            AroundCentre press(compositor, Centre(l.cancel), pressed_scale(kConfirmCancel), 0.0f);
            painter.DrawButton({l.cancel, spec.cancel_text, {}, fluent::ButtonKind::Standard,
                                state_for(kConfirmCancel)});
        }
        if (!spec.secondary_text.empty()) {
            AroundCentre press(compositor, Centre(l.secondary), pressed_scale(kConfirmSecondary),
                               0.0f);
            fluent::ControlState secondary_state = state_for(kConfirmSecondary);
            secondary_state.enabled = ConfirmAnyChecked(spec);
            painter.DrawButton({l.secondary, spec.secondary_text, {},
                                fluent::ButtonKind::Standard, secondary_state});
        }
        // Check box dialogs: nothing ticked, nothing to confirm.
        const bool any_checked = ConfirmAnyChecked(spec);
        {
            AroundCentre press(compositor, Centre(l.primary), pressed_scale(kConfirmPrimary),
                               0.0f);
            fluent::ControlState primary_state = state_for(kConfirmPrimary);
            primary_state.enabled = any_checked;
            painter.DrawButton({l.primary, spec.confirm_text, {},
                                spec.danger ? fluent::ButtonKind::Danger
                                            : fluent::ButtonKind::Primary,
                                primary_state});
        }
        // The focus ring travelling between buttons (the window springs focus_rect).
        const bool ring = visual.show_focus && visual.focus_glide;
        if (ring) painter.DrawFocusRing(visual.focus_rect, Dip(scale, theme.radius_control));
        // The shared accent ring vanishes against a filled button; add the Fluent
        // outer ring in the text colour.
        if (visual.show_focus && visual.focus == kConfirmPrimary) {
            const D2D1_RECT_F& r = ring ? visual.focus_rect : l.primary;
            const float gap = Dip(scale, 3.0f);
            painter.StrokeRoundedRect(D2D1::RectF(r.left - gap, r.top - gap,
                                                  r.right + gap, r.bottom + gap),
                                      Dip(scale, theme.radius_control) + gap, theme.text,
                                      Dip(scale, 1.5f));
        }
    }

}

} // namespace pulse::ui
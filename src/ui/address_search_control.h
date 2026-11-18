#pragma once
#include "address_search_layout.h"
#include "fluent_components.h"
#include "../common/localization.h"
#include <wrl/client.h>

namespace pulse::ui {
enum class AddressSearchPart { None, Field, Scope, Mode, Options, Close };
inline D2D1_COLOR_F AddressSearchHostedFill(const Theme& theme, bool high_contrast) {
    // The hosted-edit branch of Painter::DrawTextFieldFrame uses these tokens.
    return high_contrast ? theme.fill_input : theme.fill_input_focus;
}
struct AddressSearchControlState {
    bool focused = true, current = false, content = false;
    float scope_animation = 0;
    std::wstring scope_label;
    AddressSearchPart hover = AddressSearchPart::None;
};
inline AddressSearchLayout LayoutAddressSearchControl(D2D1_RECT_F field, float scale, bool options = true) {
    auto layout = LayoutAddressSearch(field, scale);
    if (!options) {
        const float width = layout.options.right - layout.options.left;
        layout.mode.left += width; layout.mode.right += width;
        layout.name = layout.mode;
        layout.content.left = layout.content.right = layout.mode.right;
        layout.input.right = layout.mode.left - 4 * scale;
        layout.options.left = layout.options.right;
    }
    return layout;
}
// The active search field shared by the address toolbar and global search.
// Text/icon callbacks use the owner's existing typography and icon renderer.
template<class Icon, class Text, class Measure>
void DrawAddressSearchControl(fluent::Painter& painter, ID2D1DeviceContext* dc,
    const Theme& theme, D2D1_RECT_F field, const AddressSearchLayout& layout,
    float scale, const AddressSearchControlState& state, Icon&& icon, Text&& text, Measure&& measure) {
    fluent::ControlState field_state{};
    field_state.focused = state.focused; field_state.hovered = state.hover == AddressSearchPart::Field;
    painter.DrawTextFieldFrame(field, field_state, true);
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(theme.text, &brush))) return;
    const auto fill = [&](D2D1_RECT_F bounds, bool accent, AddressSearchPart part) {
        auto color = accent ? theme.accent : theme.text;
        color.a *= accent ? .18f : .07f;
        if (state.hover == part) color.a = std::min(1.0f, color.a + .06f);
        brush->SetColor(color); dc->FillRoundedRectangle(D2D1::RoundedRect(bounds, 4 * scale, 4 * scale), brush.Get());
    };
    if (state.scope_animation > 0) {
        auto color = theme.accent; color.a *= state.scope_animation * .18f; brush->SetColor(color);
        dc->FillRoundedRectangle(D2D1::RoundedRect(layout.scope, 4 * scale, 4 * scale), brush.Get());
    }
    const auto& scope = layout.scope;
    if (scope.right > scope.left) {
        const float h = scope.bottom - scope.top;
        const auto glyph = state.current ? L"\xE8B7" : L"\xE774";
        const auto color = state.current ? theme.accent : theme.text_secondary;
        fill(scope, state.current, AddressSearchPart::Scope);
        if (layout.scope_label) {
            icon(scope.left + 3 * scale, scope.top, 18 * scale, h, glyph, color, .7f);
            icon(scope.right - 16 * scale, scope.top, 14 * scale, h, L"\xE70D", theme.text_secondary, .45f);
            const auto label = state.scope_label.empty() ? l10n::Get(state.current ? l10n::StringId::SearchScopeHere : l10n::StringId::SearchScopeAll) : state.scope_label;
            text(label, {scope.left + 23 * scale, scope.top, scope.right - 18 * scale, scope.bottom}, theme.text);
        } else icon(scope.left, scope.top, scope.right - scope.left, h, glyph, color, .7f);
    }
    const auto& mode = layout.mode;
    if (mode.right > mode.left) {
        const float h = mode.bottom - mode.top;
        fill(mode, state.content, AddressSearchPart::Mode);
        const auto glyph = state.content ? L"\xE8A5" : L"\xE8D2";
        if (layout.mode_label) {
            const auto& label = l10n::Get(state.content ? l10n::StringId::SearchModeContent : l10n::StringId::SearchModeName);
            const float iw = 14 * scale, gap = 3 * scale;
            const float x = std::max(mode.left + 4 * scale, (mode.left + mode.right - measure(label) - iw - gap) * .5f);
            icon(x, mode.top, iw, h, glyph, state.content ? theme.accent : theme.text_secondary, .6f);
            text(label, {x + iw + gap, mode.top, mode.right, mode.bottom}, theme.text);
        } else icon(mode.left, mode.top, mode.right - mode.left, h, glyph, state.content ? theme.accent : theme.text, .8f);
    }
    const auto button = [&](D2D1_RECT_F bounds, const wchar_t* glyph, AddressSearchPart part) {
        if (bounds.right <= bounds.left) return;
        fluent::ButtonSpec spec; spec.bounds = bounds; spec.icon_only = true; spec.skip_glyph = true;
        spec.kind = fluent::ButtonKind::Transparent; spec.bordered = false; spec.state.hovered = state.hover == part;
        painter.DrawButton(spec);
        const float edge = std::min({20 * scale, bounds.right - bounds.left - 4 * scale, bounds.bottom - bounds.top - 4 * scale});
        icon((bounds.left + bounds.right - edge) * .5f, (bounds.top + bounds.bottom - edge) * .5f, edge, edge, glyph, theme.text, .8f);
    };
    button(layout.options, L"\xE9E9", AddressSearchPart::Options);
    button(layout.close, L"\xE711", AddressSearchPart::Close);
}
}

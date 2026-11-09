#include "ui_renderer.h"
#include "ui_renderer_internal.h"
#include "address_search_control.h"

namespace pulse::ui {

D2D1_RECT_F MainRenderer::AddressSearchButtonRect(float w) const {
    const auto field = SearchBarRect(w);
    const float width = (field.right - field.left) / scale_ - 8.0f;
    return D2D1::RectF(field.right - (width + 4.0f) * scale_, field.top + 3.0f * scale_,
                      field.right - 4.0f * scale_, field.bottom - 3.0f * scale_);
}

void MainRenderer::DrawAddressSearchChrome(const WindowViewModel& vm, float w, const Theme& theme) {
    if (search_hidden_) return;
    const auto field = SearchBarRect(w);
    const auto toolbar = ToolbarLayoutAt(w, 0);
    if (!vm.address_searching && toolbar.merged_search) {
        const auto button=D2D1::RectF(field.right-32*scale_,field.top,field.right,field.bottom);
        DrawButton(button,theme,IsHovered(vm,HitTestResult::AddressSearch) ? theme.fill_hover : kTransparent,
            kIconSearch,L"",theme.text_secondary,true,true);
        return;
    }
    fluent::ControlState state{};
    state.focused = vm.address_searching;
    state.hovered = IsHovered(vm, HitTestResult::AddressSearch);
    if (vm.address_searching) {
        const auto layout = LayoutAddressSearch(field, scale_);
        AddressSearchControlState control;
        control.focused = vm.address_searching;
        control.current = vm.address_search_current; control.content = vm.address_search_content;
        control.scope_label = vm.address_search_scope_label; control.scope_animation = vm.address_scope_animation;
        if (IsHovered(vm, HitTestResult::AddressSearchScope)) control.hover = AddressSearchPart::Scope;
        else if (IsHovered(vm, HitTestResult::AddressSearchMode)) control.hover = AddressSearchPart::Mode;
        else if (IsHovered(vm, HitTestResult::AddressSearchOptions)) control.hover = AddressSearchPart::Options;
        else if (IsHovered(vm, HitTestResult::AddressSearchClear) || IsHovered(vm, HitTestResult::AddressSearchClose)) control.hover = AddressSearchPart::Close;
        else if (state.hovered) control.hover = AddressSearchPart::Field;
        DrawAddressSearchControl(painter_, compositor_->Dc(), theme, field, layout, scale_, control,
            [&](float x, float y, float width, float height, const wchar_t* glyph, D2D1_COLOR_F color, float ratio) {
                DrawIconText(x, y, width, height, glyph, L"", color, ratio);
            },
            [&](const std::wstring& text, D2D1_RECT_F bounds, D2D1_COLOR_F color) {
                MakeBrush(compositor_->Dc(), color, brText_);
                DrawTextEndEllipsis(compositor_, compositor_->SmallFormat(), brText_.get(), text,
                    bounds.left, bounds.top, std::max(0.0f, bounds.right - bounds.left), bounds.bottom - bounds.top);
            },
            [&](const std::wstring& text) { return MeasureTextWidth(compositor_->DwriteFactory(), compositor_->SmallFormat(), text); });
        if (!vm.address_editing) {
            const auto text = vm.address_search_text.empty()
                ? l10n::Get(vm.address_search_content ? l10n::StringId::SearchContentHint : l10n::StringId::SearchNameHint) : vm.address_search_text;
            ComPtr<ID2D1SolidColorBrush> brush;
            compositor_->Dc()->CreateSolidColorBrush(vm.address_search_text.empty()
                ? theme.text_secondary : theme.text, &brush);
            if (brush.get()) DrawTextRect(compositor_->Dc(), compositor_->AddressFormat(), brush.get(),
                text, layout.input.left, layout.input.top, layout.input.right - layout.input.left,
                layout.input.bottom - layout.input.top);
        }


    } else {
        fluent::TextFieldSpec search;
        search.bounds = field;
        search.state = state;
        search.placeholder = toolbar.navigation_tier == NavigationTier::Narrow || vm.address_search_placeholder.empty()
            ? l10n::Get(l10n::StringId::Search) : vm.address_search_placeholder;
        search.leading_glyph = L"\xE721";
        search.trailing_keycap = field.right - field.left >= 220 * scale_ ? L"Ctrl+K" : L"";
        search.suppress_text = true;
        painter_.DrawTextField(search);
        MakeBrush(compositor_->Dc(),theme.text_secondary,brTextSecondary_);
        const float text_left=field.left+36*scale_;
        const float text_right=field.right-14*scale_-painter_.MeasureBadgeWidth(search.trailing_keycap);
        DrawTextEndEllipsis(compositor_, compositor_->TextFormat(),
            brTextSecondary_.get(),std::wstring(search.placeholder),text_left,field.top,
            std::max(0.0f,text_right-text_left),field.bottom-field.top);
    }
    if (vm.address_search_animation > 0.0f) {
        auto color = theme.accent;
        color.a *= vm.address_search_animation;
        ComPtr<ID2D1SolidColorBrush> brush;
        compositor_->Dc()->CreateSolidColorBrush(color, &brush);
        const float right = field.right - 6.0f * scale_;
        const float width = (field.right - field.left - 12.0f * scale_) * vm.address_search_animation;
        if (brush.get()) compositor_->Dc()->DrawLine(D2D1::Point2F(right - width, field.bottom - scale_),
            D2D1::Point2F(right, field.bottom - scale_), brush.get(), 2.0f * scale_);
    }
}

} // namespace pulse::ui

#include "ui_renderer.h"
#include "ui_renderer_internal.h"
#include "address_search_layout.h"

namespace pulse::ui {

D2D1_RECT_F MainRenderer::AddressSearchButtonRect(float w) const {
    const auto field = AddressBarRect(w);
    const float width = field.right - field.left >= 180.0f * scale_ ? 82.0f : 28.0f;
    return D2D1::RectF(field.right - (width + 4.0f) * scale_, field.top + 3.0f * scale_,
                      field.right - 4.0f * scale_, field.bottom - 3.0f * scale_);
}

void MainRenderer::DrawAddressSearchChrome(const WindowViewModel& vm, float w, const Theme& theme) {
    const auto field = AddressBarRect(w);
    auto button = [&](D2D1_RECT_F bounds, const std::wstring& text, const wchar_t* glyph,
                      HitTestResult::Region region, bool dropdown = false) {
        fluent::ButtonSpec spec;
        spec.bounds = bounds;
        spec.text = text;
        spec.glyph = glyph;
        spec.icon_only = text.empty();
        spec.skip_glyph = true;
        spec.kind = fluent::ButtonKind::Transparent;
        spec.bordered = false;
        spec.drop_down = dropdown;
        spec.state.hovered = IsHovered(vm, region);
        painter_.DrawButton(spec);
        {
            // Compact buttons cannot use the text button's 8-DIP side padding.
            const float edge = std::min({20.0f * scale_, bounds.right - bounds.left - 4.0f * scale_,
                                         bounds.bottom - bounds.top - 4.0f * scale_});
            const float cx = spec.icon_only ? (bounds.left + bounds.right) * 0.5f
                                            : bounds.left + 18.0f * scale_;
            const float cy = (bounds.top + bounds.bottom) * 0.5f;
            DrawIconText(cx - edge * 0.5f, cy - edge * 0.5f, edge, edge,
                         glyph, L"", theme.text, 0.8f);
        }
    };
    if (vm.address_searching) {
        const auto layout = LayoutAddressSearch(field, scale_);
        if (!vm.address_editing) {
            const auto text = vm.address_search_text.empty()
                ? l10n::Get(l10n::StringId::Search) : vm.address_search_text;
            ComPtr<ID2D1SolidColorBrush> brush;
            compositor_->Dc()->CreateSolidColorBrush(vm.address_search_text.empty()
                ? theme.text_secondary : theme.text, &brush);
            if (brush.get()) DrawTextRect(compositor_->Dc(), compositor_->AddressFormat(), brush.get(),
                text, layout.input.left, layout.input.top, layout.input.right - layout.input.left,
                layout.input.bottom - layout.input.top);
        }
        if (vm.address_scope_animation > 0.0f) {
            auto color = theme.accent;
            color.a *= vm.address_scope_animation * 0.18f;
            ComPtr<ID2D1SolidColorBrush> brush;
            compositor_->Dc()->CreateSolidColorBrush(color, &brush);
            if (brush.get()) compositor_->Dc()->FillRoundedRectangle(
                D2D1::RoundedRect(layout.scope, 4.0f * scale_, 4.0f * scale_), brush.get());
        }
        const auto label = l10n::Get(vm.address_search_current
            ? l10n::StringId::LocationCurrent : l10n::StringId::LocationIndexed);
        button(layout.scope, layout.scope_label ? label : L"", L"\xE721",
               HitTestResult::AddressSearchScope, layout.scope_label);
        if (vm.address_search_has_text && layout.clear.right > layout.clear.left)
            button(layout.clear, L"", L"\xE711", HitTestResult::AddressSearchClear);
        button(layout.close, L"", L"\xE72B", HitTestResult::AddressSearchClose);
    } else if (!vm.address_editing) {
        const auto bounds = AddressSearchButtonRect(w);
        button(bounds, bounds.right - bounds.left > 40.0f * scale_
            ? l10n::Get(l10n::StringId::Search) : L"", L"\xE721", HitTestResult::AddressSearch);
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

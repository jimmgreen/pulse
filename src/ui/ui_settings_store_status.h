#pragma once

#include "ui_renderer_internal.h"

namespace pulse::ui::store_notify {

inline void DrawStatusBanners(fluent::Painter& painter, const SettingsLayout& layout,
    const ExtensionStoreView& view, const Theme& theme, bool browse, bool installed, bool restart_hovered) {
    const auto draw = [&](D2D1_RECT_F bounds, D2D1_RECT_F text, store_text::StatusNotice notice) {
        if (!store_view::Has(bounds) || notice.text.empty()) return;
        fluent::InfoBarSpec bar{};
        bar.bounds = bounds;
        bar.kind = notice.kind;
        bar.show_close = false;
        painter.DrawInfoBar(bar);
        // The store allows multi-line diagnostics and a stacked action in narrow windows.
        painter.DrawWrappedCaption(notice.text, {text.left, text.top}, text.right - text.left, theme.text);
    };
    draw(layout.store_notice_surface, layout.store_notice, store_text::Notice(view, browse, installed));
    draw(layout.store_restart_surface, layout.store_restart, store_text::RestartNotice(view));
    if (store_view::Has(layout.store_restart_button)) {
        fluent::ControlState state{};
        state.enabled = store_text::CanRestart(view);
        state.hovered = state.enabled && restart_hovered;
        painter.DrawButton({layout.store_restart_button, l10n::Get(l10n::StringId::StoreRestartNow), {},
            fluent::ButtonKind::Primary, state});
    }
}

} // namespace pulse::ui::store_notify

// panel_metrics.h — Window-relative limits for the sidebar and the details panel.
#pragma once
#include <algorithm>

namespace pulse::ui {

// File Explorer sizes its panes against the window instead of using fixed caps:
// dragging a splitter stops only once the file list reaches its minimum width, so
// either side panel can take most of a wide window while the list stays usable.
//
// Measured on Windows 11 at 100% scale with build-selftest\measure-explorer-panes.ps1:
//   navigation pane: 64 px rail, otherwise up to  window - 144 px   with no details
//                                                  window - 144 px - details otherwise
//   details pane   : 307 px minimum, up to window - panel - 144 px
// There is no ratio cap and no absolute cap: at a 3000 px window the navigation pane
// still reaches 2856 px. Pulse reserves a little less than Explorer does so either
// panel can grow further; the list keeps one readable column instead of a full row.
inline constexpr float kListMinWidthDip = 112.0f;
inline constexpr float kSidebarMinWidthDip = 160.0f;
inline constexpr float kSidebarRailWidthDip = 48.0f;
inline constexpr float kDetailsMinWidthDip = 300.0f;
// Sanity bound for stored preferences; the window decides the effective limit.
inline constexpr float kPanelWidthMaxDip = 4000.0f;

// Below these window widths the sidebar folds into its rail and the details panel
// gives way entirely.
inline constexpr float kSidebarRailWindowDip = 900.0f;
// A sidebar this narrow is the icon rail: the layout, hit-testing, and the rail
// tooltips all ask here instead of open-coding the threshold. Slightly above
// kSidebarRailWidthDip so a rail drawn at a fractional scale still counts.
inline constexpr float kSidebarRailLayoutDip = 60.0f;
inline constexpr bool SidebarRailLayout(float sidebar_width_px, float scale) {
    return sidebar_width_px <= kSidebarRailLayoutDip * scale;
}
inline constexpr float kDetailsVisibleWindowDip = 1000.0f;

// Widest the sidebar may become: window minus the open details panel, minus the
// window chrome (margins) and the file-list minimum.
inline float MaxSidebarWidthDip(float window_dip, float details_dip, bool details_visible,
                                float chrome_dip) {
    const float reserved = (details_visible ? details_dip : 0.0f) + chrome_dip + kListMinWidthDip;
    return (std::max)(kSidebarMinWidthDip, window_dip - reserved);
}

// Widest the details panel may become: window minus the sidebar (or its rail),
// minus the window chrome and the file-list minimum.
inline float MaxDetailsWidthDip(float window_dip, float sidebar_dip, bool sidebar_railed,
                                float chrome_dip) {
    const float reserved = (sidebar_railed ? kSidebarRailWidthDip : sidebar_dip)
        + chrome_dip + kListMinWidthDip;
    return (std::max)(kDetailsMinWidthDip, window_dip - reserved);
}

} // namespace pulse::ui

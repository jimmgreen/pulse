#pragma once
// 界面 quick panel (ui_modes_mockup flyout()): the three starting points with
// mini previews, display toggles and the sidebar / commands / address /
// density segments. Borderless popup hosted like GitBranchPopup (Compositor +
// fluent::Painter). It only draws a LayoutPanelState and reports clicks; the
// host changes the preferences and calls Update() with the new state.
#include "fluent_components.h"
#include "ui_compositor.h"

#include <array>
#include <functional>
#include <string>

namespace pulse::ui {

enum class LayoutPanelToggle : int {
    Tabs, SidebarLite, Details, StatusBar, Tray, Zebra, SelectionFloat, Extensions, Count
};
inline constexpr int kLayoutPanelToggleCount = static_cast<int>(LayoutPanelToggle::Count);

enum class LayoutPanelSegment : int { Sidebar, Commands, Address, Density, Count };
inline constexpr int kLayoutPanelSegmentCount = static_cast<int>(LayoutPanelSegment::Count);

struct LayoutPanelState {
    int mode = 2;                // starting point: 0 专注, 1 标准, 2 工作台
    int changed = 0;             // options that differ from the mode's preset
    std::array<bool, kLayoutPanelToggleCount> toggles{};
    std::array<int, kLayoutPanelSegmentCount> segments{};  // selected option per row
};

struct LayoutPanelCallbacks {
    std::function<void(int mode)> on_preset;          // a card, 恢复, Alt+Shift+1..3
    std::function<void(LayoutPanelToggle)> on_toggle;
    std::function<void(LayoutPanelSegment, int value)> on_segment;
    std::function<void()> on_more_settings;           // 更多界面设置 (the panel is closed first)
    std::function<void()> on_closed;
};

// A starting point's miniature window (installer MiniPreview); its height is
// 10/16 of box's width. Also drawn by 设置 › 界面布局.
void DrawLayoutMiniPreview(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* brush, const D2D1_RECT_F& box,
                           int mode, const Theme& theme, bool dark, float scale);

class LayoutPanel {
public:
    LayoutPanel() = default;
    ~LayoutPanel();
    LayoutPanel(const LayoutPanel&) = delete;
    LayoutPanel& operator=(const LayoutPanel&) = delete;

    // `anchor` (screen px): the panel's top-right corner, kept on the monitor.
    void Show(HWND owner, POINT anchor, bool dark, D2D1_COLOR_F accent,
              const LayoutPanelState& state, LayoutPanelCallbacks callbacks);
    void Update(const LayoutPanelState& state, bool dark, D2D1_COLOR_F accent);
    void Hide();
    bool IsOpen() const noexcept;
    // When the panel last closed: a click on the 界面 button that closed it
    // by taking the activation must not open it again.
    ULONGLONG LastHideTick() const noexcept { return hide_tick_; }

private:
    enum class HitKind : uint8_t { None, Restore, Preset, Toggle, Segment, MoreSettings };
    struct Hit {
        HitKind kind = HitKind::None;
        int index = -1;
        int value = -1;
        bool operator==(const Hit&) const = default;
    };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);
    bool EnsureWindow(HWND owner);
    void PositionPanel();
    void ApplyTheme();
    Theme CurrentTheme() const;
    void RecreateFormats();
    void Layout();
    void Render();
    void DrawMiniPreview(ID2D1DeviceContext* dc, const D2D1_RECT_F& box, int mode, const Theme& theme);
    Hit HitTest(float x, float y) const;
    void OnClick(const Hit& hit);
    float Measure(std::wstring_view text, IDWriteTextFormat* format) const;
    float PanelHeightDip() const;
    // Transparent border around the card for the drop shadow (FluentMenu's
    // kShadowMargin); 0 when the swap chain is opaque.
    float Margin() const noexcept { return static_cast<float>(margin_); }
    D2D1_RECT_F CardRect() const;
    void StepHover(bool restart);

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    Compositor compositor_;
    fluent::Painter painter_;
    float scale_ = 1.0f;
    bool dark_ = false;
    D2D1_COLOR_F accent_{0.0f, 0.47f, 0.83f, 1.0f};
    POINT anchor_{};
    LayoutPanelState state_{};
    LayoutPanelCallbacks callbacks_;

    ComPtr<IDWriteTextFormat> title_;
    ComPtr<IDWriteTextFormat> body_;
    ComPtr<IDWriteTextFormat> caption_;
    ComPtr<IDWriteTextFormat> caption_bold_;
    ComPtr<IDWriteTextFormat> small_;
    ComPtr<IDWriteTextFormat> kbd_;
    ComPtr<ID2D1SolidColorBrush> brush_;

    // Layout (px, client coordinates).
    D2D1_RECT_F restore_rc_{};
    std::array<D2D1_RECT_F, 3> preset_rc_{};
    std::array<D2D1_RECT_F, kLayoutPanelToggleCount> toggle_rc_{};
    std::array<std::array<D2D1_RECT_F, 3>, kLayoutPanelSegmentCount> segment_rc_{};
    std::array<float, kLayoutPanelSegmentCount> segment_y_{};
    float section_y_ = 0.0f;
    float footer_y_ = 0.0f;
    D2D1_RECT_F more_rc_{};

    Hit hover_{};
    Hit pressed_{};
    bool tracking_leave_ = false;
    int margin_ = 0;
    // Preset-card hover, 0..1 (eased in Render), stepped on a timer.
    std::array<float, 3> hover_t_{};
    ULONGLONG hover_step_tick_ = 0;
    bool closing_ = false;
    ULONGLONG hide_tick_ = 0;
};

}  // namespace pulse::ui
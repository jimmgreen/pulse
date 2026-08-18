// settings_window.h — First Fluent settings window (右键菜单 page).
#pragma once
#include "FluentTokens.h"
#include "fluent_components.h"
#include "ui_compositor.h"
#include "../app/context_menu_prefs.h"

namespace pulse::ui {

class SettingsWindow {
public:
    SettingsWindow();
    ~SettingsWindow();

    bool Create(HWND owner, app::ContextMenuPrefs* prefs);
    void Destroy();
    void SetTheme(bool dark, D2D1_COLOR_F accent);
    void Show(bool activate = true);
    void Hide();
    bool IsVisible() const;
    HWND Hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);
    void ApplyWindowTheme();
    void RebuildHits();
    void Render();
    int HitTest(float x, float y) const;
    void OnClick(int hit);
    void ClampScroll();

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    Compositor compositor_;
    fluent::Painter painter_;
    app::ContextMenuPrefs* prefs_ = nullptr;
    bool dark_ = false;
    bool backdrop_enabled_ = false;
    bool positioned_ = false;
    float scale_ = 1.0f;
    D2D1_COLOR_F accent_ = HexColor(0x0078D4);
    int hover_ = 0;
    int pressed_ = 0;
    float scroll_ = 0.0f;
    float content_h_ = 0.0f;

    struct Hit {
        D2D1_RECT_F rect{};
        int id = 0; // 1 close, 2 restore, 10+ category, 100+ item
        bool scrolled = true;
    };
    std::vector<Hit> hits_;
};

} // namespace pulse::ui

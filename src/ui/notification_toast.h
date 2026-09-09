#pragma once
#include "fluent_components.h"

namespace pulse::ui {
class NotificationToast {
public:
    void Show(HWND owner, std::wstring title, std::wstring message, bool persistent = true);
    bool HandleMessage(HWND owner, UINT message, WPARAM wparam, LPARAM lparam);
    void Draw(Compositor& compositor, const Theme& theme, float scale, bool high_contrast);
    bool IsVisible() const noexcept { return visible_; }
    D2D1_RECT_F Bounds() const noexcept { return card_; }
private:
    void Dismiss(HWND owner);
    static constexpr UINT_PTR timer = 0x50554C54;
    fluent::Painter painter_;
    std::wstring title_, message_;
    D2D1_RECT_F card_{}, close_{};
    ULONGLONG born_ = 0, last_tick_ = 0, remaining_ = 0;
    bool visible_ = false, hovered_ = false, pressed_close_ = false, pressed_card_ = false, persistent_ = true;
};
}

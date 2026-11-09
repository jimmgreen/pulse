#pragma once
#include "fluent_components.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pulse::ui {
struct SnackPalette;   // snack_style.h
// Multi-window M7i (toast redesign A): what a capsule notification shows on
// its left.
enum class SnackIcon { Tab, Window, Undo, Merge, Suggest };
// A capsule button: posts `message` to the owner and closes it (0: closes).
struct SnackAction {
    std::wstring label;
    UINT message = 0;
    WPARAM wparam = 0;
};

class NotificationToast {
public:
    void Show(HWND owner, std::wstring title, std::wstring message, bool persistent = true, UINT action_message = 0);
    void ShowError(HWND owner, std::wstring title, std::wstring message);
    // The corner card with up to two buttons under its text (the first one
    // reads as the primary): each posts its message and closes the card. It
    // waits to be read like an error; `kind` picks the glyph and tone. The
    // text gets up to four lines.
    void ShowActionCard(HWND owner, fluent::InfoBarKind kind, std::wstring title, std::wstring message,
                        std::vector<SnackAction> actions);
    // The tone of the card shown by the last Show (its glyph; an error also
    // gets an edge). Show picks warning for persistent cards, else info.
    void SetKind(fluent::InfoBarKind kind) noexcept { kind_ = kind; }
    // A one-line capsule at the bottom center (M7i): an icon, what happened
    // (`text`) and a quieter `detail`, up to two buttons and a shortcut chip.
    // It goes after `duration_ms` (paused under the cursor); only the buttons
    // act, the close button and Esc dismiss.
    void ShowSnack(HWND owner, SnackIcon icon, std::wstring text, std::wstring detail,
                   std::vector<SnackAction> actions, std::wstring key = {}, UINT duration_ms = 6000);
    // The next ShowSnack replaces the capsule that is up (its plate morphs)
    // instead of stacking over it: the outcome of the work it reported.
    void ReplaceNextSnack() noexcept { replace_next_ = true; }
    bool HandleMessage(HWND owner, UINT message, WPARAM wparam, LPARAM lparam);
    void Draw(Compositor& compositor, const Theme& theme, float scale, bool high_contrast);
    // Whether the toast takes input. A dismissed toast still draws its exit
    // for ~150 ms; Bounds is always the settled layout (hit-testing never
    // follows the motion).
    bool IsVisible() const noexcept { return visible_; }
    D2D1_RECT_F Bounds() const noexcept { return card_; }
    // Hides the toast when clicking it would post `action_message`: what it
    // offers was done another way or no longer applies.
    void DismissFor(HWND owner, UINT action_message) {
        if (!visible_ || !action_message) return;
        bool offered = action_message_ == action_message;
        for (const SnackAction& action : actions_) offered = offered || action.message == action_message;
        if (offered) Dismiss(owner);
    }
private:
    void Dismiss(HWND owner);
    void DrawSnack(Compositor& compositor, const Theme& theme, float scale, bool high_contrast);
    // Where the coming / replacing / leaving motion stands this frame.
    struct Pose {
        float dy = 0.0f;             // drop below the settled place, px
        float rot = 0.0f;            // degrees (a dismissed capsule tips away)
        float plate_w = 0.0f;        // plate width, px (the capsule unfurls)
        float plate_alpha = 1.0f;
        float content_alpha = 1.0f;
    };
    Pose CurrentPose(float scale, float target_w, bool high_contrast) const;
    static constexpr UINT_PTR timer = 0x50554C54;
    fluent::Painter painter_;
    std::wstring title_, message_;
    UINT action_message_ = 0;
    D2D1_RECT_F card_{}, close_{};
    ULONGLONG born_ = 0, last_tick_ = 0, remaining_ = 0;
    bool visible_ = false, hovered_ = false, pressed_close_ = false, pressed_card_ = false, persistent_ = true;
    fluent::InfoBarKind kind_ = fluent::InfoBarKind::Warning;
    // The capsule (M7i).
    bool snack_ = false;
    SnackIcon icon_ = SnackIcon::Tab;
    std::wstring key_;
    std::vector<SnackAction> actions_;
    std::vector<D2D1_RECT_F> action_rects_;
    int pressed_action_ = -1;
    ULONGLONG duration_ = 6000;
    // Motion (motion::NowMs clock). A Show over a showing toast morphs its
    // plate from the drawn width instead of rising again.
    uint64_t enter_at_ = 0, exit_at_ = 0;
    bool animate_ = true, exiting_ = false, swapped_ = false, swap_snack_ = false;
    float swap_from_w_ = 0.0f, drawn_w_ = 0.0f;
    // Motion v2 "toaststack" (rauno-notif-stack): a capsule shown over a
    // capsule pushes the old one back - 9 px higher, 5% narrower and fainter
    // per step, at most three - and hovering the stack fans it out 52 px a
    // step. Earlier capsules only show what they said; the front one acts.
    struct Ghost {
        std::wstring title, detail;
        SnackIcon icon = SnackIcon::Tab;
        float width = 0.0f;
        ULONGLONG at = 0;   // GetTickCount64 when pushed back
    };
    std::vector<Ghost> ghosts_;
    bool replace_next_ = false;
    uint64_t stack_push_at_ = 0, expand_at_ = 0;   // motion::NowMs
    float expand_from_ = 0.0f, expand_to_ = 0.0f;
    D2D1_RECT_F stack_area_{};   // the drawn ghosts (hover keeps the fan open)
    float Expand() const noexcept;
    bool StackMoving() const noexcept;
    void AccountTime(ULONGLONG now) noexcept;  // spend time on the expiry unless paused
    void DrawGhosts(Compositor& compositor, const SnackPalette& pal, float scale, float height,
                    const Pose& pose, bool high_contrast);
};
}

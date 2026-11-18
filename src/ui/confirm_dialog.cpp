#include "../common/windows_compat.h"
#include "confirm_dialog.h"

#include "confirm_dialog_view.h"
#include "ui_motion.h"
#include "window_helpers.h"
#include "surface_capture.h"

#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace pulse::ui {
namespace {

constexpr wchar_t kConfirmClass[] = L"PulseConfirmWindow";

// Motion (animation phase 9b, pulse_motion_v2): the icon pops in (and a
// warning glyph wobbles once), the text, item rows and buttons rise in one
// after another, and the keyboard focus ring springs between the buttons.
// Frames come from a timer armed only while something moves.
constexpr UINT_PTR kConfirmFrameTimer = 0x43464D54;  // 'CFMT'
constexpr float kBodyDelayMs = 30.0f;
constexpr float kRowsDelayMs = 50.0f;
constexpr float kFooterDelayMs = 80.0f;
constexpr float kWobbleDelayMs = 90.0f;
constexpr float kWobbleMs = 460.0f;
constexpr motion::SpringSpec kFocusSpring{700.0f, 44.0f};

float After(float t_ms, float delay_ms, motion::SpringSpec spec) {
    return t_ms <= delay_ms ? 0.0f : motion::SpringProgress(spec, t_ms - delay_ms);
}

bool CopyTextToClipboard(HWND hwnd, const std::wstring& text) {
    if (!OpenClipboard(hwnd)) return false;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool ok = false;
    if (memory) {
        if (void* target = GlobalLock(memory)) {
            std::memcpy(target, text.c_str(), bytes);
            GlobalUnlock(memory);
            ok = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
        }
        if (!ok) GlobalFree(memory);
    }
    CloseClipboard();
    return ok;
}

class ConfirmWindow {
public:
    ConfirmChoice Show(HWND owner, const ConfirmDialogSpec& spec, bool dark,
                       D2D1_COLOR_F accent) {
        if (spec.cancel_event && WaitForSingleObject(spec.cancel_event, 0) != WAIT_TIMEOUT)
            return ConfirmChoice::Cancel;
        owner_ = owner;
        spec_ = NormalizeConfirmSpec(spec);
        dark_ = dark;
        accent_ = accent;
        choice_ = ConfirmChoice::Cancel;
        visual_.focus = ConfirmControlFor(spec_.default_choice, HasSecondary());
        // Any other default is not what the colours suggest, so show it at once.
        visual_.show_focus = visual_.focus != kConfirmPrimary;
        animations_ = motion::SystemAnimationsEnabled() && !IsHighContrast();
        focus_motion_.SetAnimationsEnabled(animations_);
        scale_ = static_cast<float>(pulse::compat::WindowDpi(owner ? owner : GetDesktopWindow()))
               / 96.0f;

        WNDCLASSEXW wc{ sizeof(wc) };
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = WndProc;
        wc.lpszClassName = kConfirmClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        if (!GetClassInfoExW(wc.hInstance, kConfirmClass, &wc)) RegisterClassExW(&wc);

        hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, kConfirmClass,
            spec_.title.c_str(), WS_POPUP | WS_THICKFRAME | WS_SYSMENU,
            CW_USEDEFAULT, CW_USEDEFAULT, static_cast<int>(420.0f * scale_),
            static_cast<int>(200.0f * scale_), owner, nullptr, wc.hInstance, this);
        if (!hwnd_) return ConfirmChoice::Cancel;
        RECT placed{};
        GetWindowRect(hwnd_, &placed);
        CenterOwnedWindow(hwnd_, owner_, placed.right - placed.left,
                          placed.bottom - placed.top);
        const bool owner_enabled = owner_ && IsWindowEnabled(owner_);
        if (owner_enabled) EnableWindow(owner_, FALSE);
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
        if (spec_.on_open) spec_.on_open(hwnd_);

        MSG message{};
        while (!done_) {
            if (spec_.cancel_event) {
                const DWORD wait = MsgWaitForMultipleObjectsEx(1, &spec_.cancel_event, INFINITE,
                    QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED) { choice_ = ConfirmChoice::Cancel; break; }
                if (!PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) continue;
                if (message.message == WM_QUIT) { PostQuitMessage(static_cast<int>(message.wParam)); break; }
            } else if (GetMessageW(&message, nullptr, 0, 0) <= 0) break;
            RedirectStrayModalKey(message, hwnd_);
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (spec_.on_open) spec_.on_open(nullptr);
        if (IsWindow(hwnd_)) { HideComposedDialog(hwnd_, owner_); DestroyWindow(hwnd_); }
        hwnd_ = nullptr;
        if (owner_enabled && IsWindow(owner_)) EnableWindow(owner_, TRUE);
        if (spec_.cancel_event && WaitForSingleObject(spec_.cancel_event, 0) != WAIT_TIMEOUT)
            choice_ = ConfirmChoice::Cancel;
        return choice_;
    }

    bool SaveFrame(const std::wstring& path) {
        // DWM's acrylic backdrop is not part of the application's swapchain.
        // Capture the same live controls over their opaque host theme surface.
        const bool saved = CaptureDialogSurface(path, [this] { Render(true); });
        Render();
        return saved;
    }
private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<ConfirmWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<ConfirmWindow*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }
        return self ? self->Handle(message, wparam, lparam)
                    : DefWindowProcW(hwnd, message, wparam, lparam);
    }

    bool HasSecondary() const { return !spec_.secondary_text.empty(); }

    void SizeToContent() {
        if (!hwnd_ || !compositor_.DwriteFactory()) return;
        formats_.Create(compositor_.DwriteFactory(), scale_);
        layout_ = LayoutConfirmDialog(spec_, painter_, compositor_.DwriteFactory(), formats_,
                                      scale_);
        const int width = static_cast<int>(std::ceil(layout_.width));
        const int height = static_cast<int>(std::ceil(layout_.height));
        sizing_ = true;
        SetWindowPos(hwnd_, nullptr, 0, 0, width, height,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOOWNERZORDER);
        sizing_ = false;
        compositor_.Resize(width, height);
    }

    void Complete(ConfirmChoice choice) {
        choice_ = choice;
        done_ = true;
        if (hwnd_) { HideComposedDialog(hwnd_, owner_); DestroyWindow(hwnd_); }
    }

    void Activate(int id) {
        if (id >= kConfirmRowBase) {
            ToggleRow(static_cast<size_t>(id - kConfirmRowBase));
            return;
        }
        if ((id == kConfirmPrimary || id == kConfirmSecondary) && !ConfirmAnyChecked(spec_)) {
            MessageBeep(MB_OK);
            return;
        }
        if (id == kConfirmPrimary && !spec_.confirm_again_text.empty() && !armed_) {
            // The first of two presses: the button and the note say so.
            armed_ = true;
            spec_.confirm_text = spec_.confirm_again_text;
            if (!spec_.confirm_again_note.empty()) spec_.note = spec_.confirm_again_note;
            SizeToContent();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (id == kConfirmPrimary) Complete(ConfirmChoice::Confirm);
        else if (id == kConfirmSecondary) Complete(ConfirmChoice::Secondary);
        else if (id == kConfirmCancel || id == kConfirmClose) Complete(ConfirmChoice::Cancel);
    }

    void ToggleRow(size_t row) {
        if (!layout_.checkable || !spec_.item_checks || row >= spec_.item_checks->size()) return;
        (*spec_.item_checks)[row] = !(*spec_.item_checks)[row];
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // Up / Down walk the check box rows; Up from the buttons reaches the last row.
    void MoveRowFocus(int direction) {
        const int rows = static_cast<int>(layout_.rows.size());
        if (!layout_.checkable || rows == 0) return;
        const int current = visual_.focus >= kConfirmRowBase ? visual_.focus - kConfirmRowBase : -1;
        int next = current;
        if (current < 0) next = direction < 0 ? rows - 1 : current;
        else next = current + direction;
        if (next >= rows) visual_.focus = kConfirmPrimary;
        else if (next >= 0) visual_.focus = kConfirmRowBase + next;
        visual_.show_focus = true;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void MoveFocus(int direction) {
        const std::vector<int> order = ConfirmFocusOrder(spec_);
        auto it = std::find(order.begin(), order.end(), visual_.focus);
        int index = it == order.end() ? 0 : static_cast<int>(it - order.begin());
        const int count = static_cast<int>(order.size());
        index = (index + direction + count) % count;
        visual_.focus = order[static_cast<size_t>(index)];
        visual_.show_focus = true;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    D2D1_RECT_F ButtonRect(int id) const {
        if (id >= kConfirmRowBase) {
            const size_t row = static_cast<size_t>(id - kConfirmRowBase);
            return row < layout_.rows.size() ? layout_.rows[row] : layout_.primary;
        }
        switch (id) {
        case kConfirmPrimary: return layout_.primary;
        case kConfirmSecondary: return layout_.secondary;
        default: return layout_.cancel;
        }
    }

    // Fills the motion fields of visual_ for this frame; true while moving.
    bool StepMotion() {
        const uint64_t now = motion::NowMs();
        if (!opened_at_) opened_at_ = now;  // the first frame starts it
        ++frame_;
        bool moving = false;
        if (animations_) {
            const float t = static_cast<float>(now - opened_at_);
            const motion::SpringSpec rise = motion::spring::Snappy;
            visual_.icon_scale = 0.6f + 0.4f * motion::SpringProgress(motion::spring::Pop, t);
            visual_.body_in = After(t, kBodyDelayMs, rise);
            for (size_t i = 0; i < kConfirmMaxItemRows; ++i)
                visual_.rows_in[i] = After(t, kRowsDelayMs + 9.0f * static_cast<float>(i), rise);
            visual_.footer_in = After(t, kFooterDelayMs, rise);
            const ConfirmTone tone = ResolveConfirmTone(spec_);
            const bool wobble = tone == ConfirmTone::Danger || tone == ConfirmTone::Warning;
            const float w = t - kWobbleDelayMs;
            visual_.icon_turn = wobble && w > 0.0f && w < kWobbleMs
                ? 10.0f * std::sin(w * 0.0345f) * std::exp(-w / 120.0f) : 0.0f;
            const float settle = kFooterDelayMs + static_cast<float>(motion::SpringSettleMs(rise));
            if (t < std::max(settle, kWobbleDelayMs + kWobbleMs)) moving = true;
            else {
                visual_.icon_scale = visual_.body_in = visual_.footer_in = 1.0f;
                for (float& k : visual_.rows_in) k = 1.0f;
                visual_.icon_turn = 0.0f;
            }
        }
        visual_.focus_glide = animations_;
        if (visual_.show_focus) {
            visual_.focus_rect = focus_motion_.Update(1, visual_.focus, ButtonRect(visual_.focus),
                                                      frame_, now, kFocusSpring);
            if (focus_motion_.Active(now)) moving = true;
        } else {
            focus_motion_.Hide();
        }
        return moving;
    }

    void Render(bool opaque_capture = false) {
        if (compositor_.NeedsRecovery()) {
            if (!compositor_.Recover()) return;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
        }
        if (!compositor_.Dc()) return;
        const bool high_contrast = IsHighContrast();
        Theme theme = high_contrast ? MakeHighContrastTheme() : MakeTheme(dark_, accent_);
        const bool backdrop = backdrop_enabled_ && !opaque_capture;
        if (!high_contrast && backdrop && IsAcrylicBackdrop(hwnd_)) ApplyGlass(theme, dark_);
        const bool moving = StepMotion();
        BeginSurface(compositor_, painter_, theme, dark_, high_contrast, backdrop,
                     scale_);
        DrawConfirmDialog(compositor_, painter_, theme, spec_, layout_, formats_, visual_,
                          dark_, high_contrast);
        EndSurface(compositor_);
        if (moving) SetTimer(hwnd_, kConfirmFrameTimer, 15, nullptr);
    }

    int Hit(LPARAM lparam) const {
        return HitTestConfirmDialog(layout_, HasSecondary(),
                                    static_cast<float>(GET_X_LPARAM(lparam)),
                                    static_cast<float>(GET_Y_LPARAM(lparam)));
    }

    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_CREATE:
            scale_ = static_cast<float>(pulse::compat::WindowDpi(hwnd_)) / 96.0f;
            backdrop_enabled_ = ApplyBackdrop(hwnd_, dark_);
            if (!compositor_.Init(hwnd_)) return -1;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
            SizeToContent();
            return 0;
        case WM_NCCALCSIZE:
            return 0;
        case WM_NCHITTEST: {
            // The size follows the content: edges are not resize handles.
            const LRESULT hit = BorderlessHitTest(hwnd_, lparam, layout_.title_bar,
                                                  layout_.close);
            return hit >= HTLEFT && hit <= HTBOTTOMRIGHT ? HTCLIENT : hit;
        }
        case WM_SIZE:
            if (!sizing_ && compositor_.Dc()) compositor_.Resize(LOWORD(lparam), HIWORD(lparam));
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_MOVE:
            compositor_.UpdateTextRenderingParams(
                MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST));
            return 0;
        case WM_DPICHANGED: {
            scale_ = HIWORD(wparam) / 96.0f;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetScale(scale_);
            if (const auto* suggested = reinterpret_cast<RECT*>(lparam)) {
                SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
            }
            SizeToContent();
            return 0;
        }
        case WM_MOUSEMOVE: {
            const int next = Hit(lparam);
            if (next != visual_.hover) {
                visual_.hover = next;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, hwnd_, 0 };
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            visual_.hover = kConfirmNone;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
            visual_.pressed = Hit(lparam);
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP: {
            const int hit = Hit(lparam);
            const int pressed = visual_.pressed;
            visual_.pressed = kConfirmNone;
            ReleaseCapture();
            if (hit == pressed) Activate(hit);
            if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_KEYDOWN:
            switch (wparam) {
            case VK_ESCAPE: Complete(ConfirmChoice::Cancel); return 0;
            case VK_TAB: MoveFocus((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1); return 0;
            case VK_LEFT: MoveFocus(-1); return 0;
            case VK_RIGHT: MoveFocus(1); return 0;
            case VK_UP:
            case VK_DOWN:
                if (layout_.checkable) {
                    MoveRowFocus(wparam == VK_DOWN ? 1 : -1);
                    return 0;
                }
                break;
            case VK_RETURN:
                // Enter on a check box row confirms; Space ticks it.
                Activate(visual_.focus >= kConfirmRowBase ? kConfirmPrimary : visual_.focus);
                return 0;
            case VK_SPACE: Activate(visual_.focus); return 0;
            case 'C':
                if (GetKeyState(VK_CONTROL) & 0x8000) {
                    CopyTextToClipboard(hwnd_, ConfirmClipboardText(spec_));
                    return 0;
                }
                break;
            }
            break;
        case WM_CLOSE:
            Complete(ConfirmChoice::Cancel);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(hwnd_, &paint);
            Render();
            EndPaint(hwnd_, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_TIMER:
            if (wparam == kConfirmFrameTimer) {
                KillTimer(hwnd_, kConfirmFrameTimer);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            break;
        case WM_DESTROY:
            compositor_.Shutdown();
            done_ = true;
            return 0;
        }
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    Compositor compositor_;
    fluent::Painter painter_{&compositor_};
    ConfirmDialogFormats formats_;
    ConfirmDialogLayout layout_;
    ConfirmDialogVisual visual_;
    ConfirmDialogSpec spec_;
    D2D1_COLOR_F accent_ = HexColor(0x0078D4);
    ConfirmChoice choice_ = ConfirmChoice::Cancel;
    float scale_ = 1.0f;
    bool dark_ = false;
    bool backdrop_enabled_ = false;
    bool done_ = false;
    bool sizing_ = false;
    bool armed_ = false;   // confirm_again_text: the first press is in
    bool animations_ = true;
    uint64_t opened_at_ = 0;
    uint64_t frame_ = 0;
    motion::SpringRectMotion focus_motion_;
};

} // namespace

ConfirmChoice ShowConfirmDialogEx(HWND owner, const ConfirmDialogSpec& spec, bool dark,
                                  D2D1_COLOR_F accent) {
    ConfirmWindow window;
    return window.Show(owner, spec, dark, accent);
}

bool ShowConfirmDialog(HWND owner, const ConfirmDialogSpec& spec, bool dark,
                       D2D1_COLOR_F accent) {
    return ShowConfirmDialogEx(owner, spec, dark, accent) == ConfirmChoice::Confirm;
}

bool SaveConfirmDialogFrame(HWND window, const std::wstring& path) {
    wchar_t name[64]{};
    DWORD process = 0;
    const DWORD thread = GetWindowThreadProcessId(window, &process);
    if (process != GetCurrentProcessId() || thread != GetCurrentThreadId() ||
        !GetClassNameW(window, name, 64) || std::wstring_view(name) != kConfirmClass) return false;
    auto* instance = reinterpret_cast<ConfirmWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    return instance && instance->SaveFrame(path);
}

} // namespace pulse::ui

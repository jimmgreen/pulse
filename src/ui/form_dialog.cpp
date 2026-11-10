// form_dialog.cpp - see form_dialog.h. Window pattern of connect_server_dialog:
// a composed borderless popup drawn with the Fluent painter; the form is an
// ExtensionTreeView child filling the area between message and footer.
#include "form_dialog.h"
#include "../common/windows_compat.h"
#include "dialog_lifecycle.h"
#include "extension_tree.h"
#include "FluentTokens.h"
#include "fluent_components.h"
#include "ui_compositor.h"
#include "window_helpers.h"
#include "../common/localization.h"

#include <windowsx.h>
#include <algorithm>
#include <cmath>

namespace pulse::ui {
namespace {
constexpr wchar_t kClass[] = L"PulseFormDialogWindow";
constexpr UINT kRunMessage = WM_APP + 0x31;
constexpr UINT kFitMessage = WM_APP + 0x32;
constexpr float kMinFittedFormH = 24.0f;
constexpr float kTitleH = 36.0f, kPad = 20.0f, kButtonH = 32.0f, kButtonGap = 8.0f, kMinButtonW = 88.0f;
constexpr int kHitClose = 1, kHitButton = 10;
} // namespace

struct FormDialog::Impl : std::enable_shared_from_this<Impl> {
    HWND hwnd = nullptr, owner = nullptr;
    FormDialogSpec spec;
    Compositor compositor;
    fluent::Painter painter;
    ExtensionTreeView tree;
    bool dark = false, done = false, running = false, owner_disabled = false;
    bool backdrop = false;
    float scale = 1.0f, message_h = 0.0f, height = 200.0f, fitted_form_h = 0.0f;
    D2D1_COLOR_F accent = HexColor(0x0078D4);
    int hover = 0, pressed = 0;

    float Width() const { return std::clamp(spec.width_dip, 360.0f, 720.0f); }
    // The requested height is a ceiling; the form shrinks to its laid-out content.
    float MaxFormHeight() const { return spec.form ? std::clamp(spec.form_height_dip, 40.0f, 640.0f) : 0.0f; }
    float FormHeight() const { return fitted_form_h > 0.0f ? fitted_form_h : MaxFormHeight(); }
    float MessageTop() const { return kTitleH + 16.0f; }
    float FormTop() const { return MessageTop() + (message_h > 0 ? message_h / scale + 12.0f : 0.0f); }
    float FooterTop() const { return FormTop() + (spec.form ? FormHeight() + 16.0f : 4.0f); }
    D2D1_RECT_F CloseRect() const { return DipRect(scale, Width() - 46.0f, 0, 46.0f, kTitleH); }
    D2D1_RECT_F ButtonRect(size_t index) const {
        float right = Width() - kPad;
        for (size_t i = spec.buttons.size(); i-- > index;) {
            const float w = std::max(kMinButtonW, painter.MeasureButtonWidth(spec.buttons[i].text) / scale);
            if (i == index) return DipRect(scale, right - w, FooterTop(), w, kButtonH);
            right -= w + kButtonGap;
        }
        return {};
    }
    // Outset by the tree's own content inset so fields line up with the message and buttons.
    RECT FormRect() const {
        const auto r = DipRect(scale, kPad, FormTop(), Width() - 2 * kPad, FormHeight());
        constexpr LONG inset = kExtensionTreeContentInset;
        return RECT{static_cast<LONG>(std::lround(r.left)) - inset, static_cast<LONG>(std::lround(r.top)) - inset,
                    static_cast<LONG>(std::lround(r.right)) + inset, static_cast<LONG>(std::lround(r.bottom)) + inset};
    }

    // Resizes the dialog to the form content (up to the requested height). Keeps the top edge.
    void Fit(bool center) {
        if (!hwnd || !tree.Hwnd()) return;
        const float content = std::ceil(static_cast<float>(tree.ContentHeight()) / scale);
        const float next = std::clamp(content, kMinFittedFormH, MaxFormHeight());
        if (std::abs(next - fitted_form_h) < 0.5f && !center) return;
        fitted_form_h = next;
        Measure();
        const int width = static_cast<int>(std::lround(Width() * scale));
        const int pixels = static_cast<int>(std::lround(height * scale));
        if (center) CenterOwnedWindow(hwnd, owner, width, pixels);
        else SetWindowPos(hwnd, nullptr, 0, 0, width, pixels, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        tree.SetBounds(FormRect());
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    void Measure() {
        message_h = spec.message.empty() ? 0.0f
            : painter.MeasureWrappedCaptionHeight(spec.message, (Width() - 2 * kPad) * scale);
        height = FooterTop() + kButtonH + kPad;
    }

    int Hit(float x, float y) const {
        if (ContainsRect(CloseRect(), x, y)) return kHitClose;
        for (size_t i = 0; i < spec.buttons.size(); ++i)
            if (ContainsRect(ButtonRect(i), x, y)) return kHitButton + static_cast<int>(i);
        return 0;
    }

    void Render() {
        if (compositor.NeedsRecovery()) {
            if (!compositor.Recover()) return;
            compositor.RecreateTextFormats(scale);
            painter.SetCompositor(&compositor);
            painter.SetScale(scale);
        }
        if (!compositor.Dc()) return;
        const bool high_contrast = IsHighContrast();
        const Theme theme = high_contrast ? MakeHighContrastTheme() : MakeTheme(dark, accent);
        BeginSurface(compositor, painter, theme, dark, high_contrast, backdrop, scale);
        // Opaque like the form beneath, so the tree view's background matches.
        painter.FillRoundedRect(DipRect(scale, 0, 0, Width(), height), 0, theme.bg);
        painter.DrawText(spec.title, DipRect(scale, 16, 0, Width() - 70, kTitleH), compositor.SmallFormat(), theme.text);
        fluent::ControlState close{};
        close.hovered = hover == kHitClose;
        close.pressed = pressed == kHitClose;
        painter.DrawTitleBarButton(CloseRect(), fluent::TitleBarButtonRole::Close, {}, close);
        painter.FillRoundedRect(DipRect(scale, 0, kTitleH, Width(), 1), 0, theme.stroke_divider);
        if (!spec.message.empty())
            painter.DrawWrappedCaption(spec.message, D2D1::Point2F(kPad * scale, MessageTop() * scale),
                                       (Width() - 2 * kPad) * scale, theme.text);
        for (size_t i = 0; i < spec.buttons.size(); ++i) {
            const int id = kHitButton + static_cast<int>(i);
            fluent::ControlState state{};
            state.hovered = hover == id;
            state.pressed = pressed == id;
            state.focused = static_cast<int>(i) == spec.default_button;
            const auto kind = spec.buttons[i].kind == FormDialogButtonKind::Primary ? fluent::ButtonKind::Primary
                : spec.buttons[i].kind == FormDialogButtonKind::Danger ? fluent::ButtonKind::Danger
                : fluent::ButtonKind::Standard;
            painter.DrawButton({ButtonRect(i), spec.buttons[i].text, {}, kind, state});
        }
        EndSurface(compositor);
    }

    std::vector<extensions::UiStateEntry> States() const {
        return tree.Hwnd() ? tree.StateValues() : std::vector<extensions::UiStateEntry>{};
    }

    // Closes the window; `answer` decides whether on_closed runs.
    void Finish(int button, bool answer) {
        if (done) return;
        done = true;
        auto keep = shared_from_this();
        auto states = States();
        auto callback = answer ? std::move(spec.on_closed) : decltype(spec.on_closed){};
        spec.on_closed = nullptr;
        spec.on_states_changed = nullptr;
        if (owner_disabled && IsWindow(owner)) { EnableWindow(owner, TRUE); owner_disabled = false; }
        if (IsWindow(hwnd)) {
            HideComposedDialog(hwnd, nullptr);
            tree.Close();
            DestroyWindow(hwnd);
        }
        if (IsWindow(owner) && answer) SetForegroundWindow(owner);
        if (callback) callback(button, std::move(states));
    }

    void Cancel() { Finish(spec.cancel_button, true); }
    void Press(int index) {
        if (index >= 0 && index < static_cast<int>(spec.buttons.size())) Finish(index, true);
    }

    void Run() {
        if (done || running) return;
        running = true;
        auto keep = shared_from_this();
        MSG message{};
        while (!done) {
            const BOOL got = GetMessageW(&message, nullptr, 0, 0);
            if (got <= 0) {
                if (got == 0) PostQuitMessage(static_cast<int>(message.wParam));
                Finish(spec.cancel_button, true);
                break;
            }
            RedirectStrayModalKey(message, hwnd);
            const bool ours = message.hwnd == hwnd || IsChild(hwnd, message.hwnd);
            if (ours && message.message == WM_KEYDOWN) {
                if (message.wParam == VK_ESCAPE) { Cancel(); continue; }
                if (message.wParam == VK_RETURN && spec.default_button >= 0 && !tree.InputComposing() &&
                    !(GetKeyState(VK_SHIFT) & 0x8000) && !(GetKeyState(VK_CONTROL) & 0x8000)) {
                    Press(spec.default_button);
                    continue;
                }
            }
            if (ours && message.message != WM_KEYDOWN && IsDialogMessageW(hwnd, &message)) continue;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        running = false;
    }

    static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd = window;
        }
        return self ? self->Handle(message, wparam, lparam) : DefWindowProcW(window, message, wparam, lparam);
    }

    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_CREATE:
            backdrop = ApplyBackdrop(hwnd, dark);
            if (!compositor.Init(hwnd)) return -1;
            compositor.RecreateTextFormats(scale);
            painter.SetCompositor(&compositor);
            painter.SetScale(scale);
            return 0;
        case WM_NCCALCSIZE:
            return 0;
        case WM_NCHITTEST: {
            const LRESULT hit = BorderlessHitTest(hwnd, lparam, kTitleH * scale, CloseRect());
            return hit == HTCAPTION || hit == HTCLIENT ? hit : HTCLIENT;  // fixed size
        }
        case WM_SIZE:
            if (compositor.Dc()) compositor.Resize(LOWORD(lparam), HIWORD(lparam));
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_MOUSEMOVE: {
            const int next = Hit(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
            if (next != hover) { hover = next; InvalidateRect(hwnd, nullptr, FALSE); }
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            hover = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
            pressed = Hit(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
            if (pressed) SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP: {
            const int hit = Hit(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
            const int was = pressed;
            pressed = 0;
            ReleaseCapture();
            if (hit && hit == was) {
                if (hit == kHitClose) Cancel();
                else Press(hit - kHitButton);
                return 0;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_CLOSE:
            Cancel();
            return 0;
        case kRunMessage:
            Run();
            return 0;
        case kFitMessage:
            if (!done) Fit(false);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(hwnd, &paint);
            Render();
            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            tree.Close();
            compositor.Shutdown();
            hwnd = nullptr;
            return 0;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
};

FormDialog::FormDialog() : impl_(std::make_shared<Impl>()) {}
FormDialog::~FormDialog() { Destroy(); }

bool FormDialog::Show(HWND owner, FormDialogSpec spec, bool dark, D2D1_COLOR_F accent) {
    if (IsOpen() || !IsWindow(owner) || spec.buttons.empty() || spec.buttons.size() > 3) return false;
    impl_ = std::make_shared<Impl>();
    auto& self = *impl_;
    self.owner = owner;
    self.spec = std::move(spec);
    self.dark = dark;
    self.accent = accent;
    self.scale = static_cast<float>(pulse::compat::WindowDpi(owner)) / 96.0f;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = Impl::Proc;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!GetClassInfoExW(wc.hInstance, kClass, &wc)) RegisterClassExW(&wc);
    const int width = static_cast<int>(std::lround(self.Width() * self.scale));
    if (!CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_CONTROLPARENT, kClass, self.spec.title.c_str(),
                         WS_POPUP | WS_THICKFRAME | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, width,
                         static_cast<int>(200 * self.scale), owner, nullptr, wc.hInstance, &self) || !self.hwnd)
        return false;
    self.Measure();
    const int height = static_cast<int>(std::lround(self.height * self.scale));
    CenterOwnedWindow(self.hwnd, owner, width, height);
    if (self.spec.form) {
        ExtensionTreeCallbacks callbacks;
        std::weak_ptr<Impl> weak = impl_;
        callbacks.on_state_changed = [weak](uint64_t, const std::vector<ExtensionStateChange>& changes) {
            const auto alive = weak.lock();
            if (!alive || alive->done) return;
            // Bound visibility may have changed the content height; refit after layout.
            if (alive->hwnd) PostMessageW(alive->hwnd, kFitMessage, 0, 0);
            if (!alive->spec.on_states_changed) return;
            std::vector<extensions::UiStateEntry> entries;
            for (const auto& change : changes) entries.push_back({change.key, change.value});
            if (!entries.empty()) alive->spec.on_states_changed(entries);
        };
        callbacks.command_enabled = [](const std::string&) { return false; };
        if (!self.tree.Create(self.hwnd, *self.spec.form, std::move(callbacks), dark, accent, self.scale)) {
            DestroyWindow(self.hwnd);
            return false;
        }
        self.tree.SetBounds(self.FormRect());
        self.Fit(true);
    }
    EnableWindow(owner, FALSE);
    self.owner_disabled = true;
    ShowWindow(self.hwnd, SW_SHOW);
    SetForegroundWindow(self.hwnd);
    if (self.tree.Hwnd()) SetFocus(self.tree.Hwnd());
    PostMessageW(self.hwnd, kRunMessage, 0, 0);
    return true;
}

bool FormDialog::SetStates(const std::vector<extensions::UiStateEntry>& states) {
    if (!IsOpen() || !impl_->tree.Hwnd() || !impl_->tree.SetStates(states)) return false;
    PostMessageW(impl_->hwnd, kFitMessage, 0, 0);
    return true;
}
void FormDialog::Dismiss() { if (IsOpen()) impl_->Cancel(); }
void FormDialog::Destroy() { if (impl_ && impl_->hwnd && !impl_->done) impl_->Finish(-1, false); }
bool FormDialog::IsOpen() const { return impl_ && impl_->hwnd && !impl_->done; }
HWND FormDialog::Hwnd() const { return IsOpen() ? impl_->hwnd : nullptr; }

} // namespace pulse::ui

// form_dialog.cpp - see form_dialog.h. Window pattern of connect_server_dialog:
// a composed borderless popup drawn with the Fluent painter; the form is an
// ExtensionTreeView child filling the area between message and footer.
#include "form_dialog.h"
#include "../common/picker_owner_guard.h"
#include "../common/windows_compat.h"
#include "dialog_lifecycle.h"
#include "extension_tree.h"
#include "FluentTokens.h"
#include "fluent_components.h"
#include "ui_compositor.h"
#include "window_helpers.h"
#include "store_icons.h"
#include "typography.h"
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
constexpr float kHeadingH = 28.0f, kSubjectH = 56.0f;
// The accent as a foreground on the dialog surface (lighter in dark mode).
D2D1_COLOR_F AccentForeground(const Theme& theme) {
    if (IsHighContrast()) return theme.accent;
    const auto& a = theme.accent;
    return theme.bg.r + theme.bg.g + theme.bg.b < 1.5f
        ? D2D1::ColorF(a.r + (1.0f - a.r) * 0.35f, a.g + (1.0f - a.g) * 0.35f, a.b + (1.0f - a.b) * 0.35f, 1.0f) : a;
}
std::optional<StoreIcon> IconOf(FormDialogIcon icon) {
    switch (icon) {
    case FormDialogIcon::Key: return StoreIcon::Key;
    case FormDialogIcon::Archive: return StoreIcon::Box;
    case FormDialogIcon::Checksum: return StoreIcon::Hash;
    case FormDialogIcon::Info: return StoreIcon::Info;
    case FormDialogIcon::Warning: return StoreIcon::Warn;
    case FormDialogIcon::Folder: return StoreIcon::Folder;
    case FormDialogIcon::Lock: return StoreIcon::Lock;
    default: return std::nullopt;
    }
}
} // namespace

struct FormDialog::Impl : std::enable_shared_from_this<Impl> {
    HWND hwnd = nullptr, owner = nullptr;
    FormDialogSpec spec;
    Compositor compositor;
    fluent::Painter painter;
    ExtensionTreeView tree;
    std::unique_ptr<pulse::PickerOwnerGuard> owner_guard;
    bool dark = false, done = false, running = false;
    bool backdrop = false;
    float scale = 1.0f, message_h = 0.0f, height = 200.0f, fitted_form_h = 0.0f;
    D2D1_COLOR_F accent = HexColor(0x0078D4);
    int hover = 0, pressed = 0;
    ComPtr<IDWriteTextFormat> heading_format, subject_format;
    bool HasHeading() const { return !spec.heading.empty(); }
    bool HasSubject() const { return spec.subject_kind != FormDialogSubject::None || !spec.subject.empty(); }
    void CreateFormats() {
        auto* factory = compositor.DwriteFactory();
        auto make = [&](ComPtr<IDWriteTextFormat>& out, float size, DWRITE_FONT_WEIGHT weight) {
            out.reset();
            if (!factory) return;
            typography::TextFormatSpec format;
            format.role = typography::FontRole::Text;
            format.size = size * scale;
            format.weight = weight;
            if (FAILED(typography::CreateTextFormat(factory, format, &out)) || !out.get()) return;
            out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            out->SetTrimming(&trimming, nullptr);
        };
        make(heading_format, 18.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        make(subject_format, 13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    }

    float Width() const { return std::clamp(spec.width_dip, 360.0f, 720.0f); }
    // The requested height is a ceiling; the form shrinks to its laid-out content.
    float MaxFormHeight() const { return spec.form ? std::clamp(spec.form_height_dip, 40.0f, 640.0f) : 0.0f; }
    float FormHeight() const { return fitted_form_h > 0.0f ? fitted_form_h : MaxFormHeight(); }
    float HeadingTop() const { return kTitleH + 16.0f; }
    float MessageTop() const { return HeadingTop() + (HasHeading() ? kHeadingH + 4.0f : 0.0f); }
    float SubjectTop() const { return MessageTop() + (message_h > 0 ? message_h / scale + 12.0f : 0.0f); }
    float FormTop() const { return SubjectTop() + (HasSubject() ? kSubjectH + 14.0f : 0.0f); }
    float FooterTop() const { return FormTop() + (spec.form ? FormHeight() + 16.0f : 4.0f) + (Banded() ? 12.0f : 0.0f); }
    // The newer layout (heading or card) sets the buttons on a shaded band.
    bool Banded() const { return HasHeading() || HasSubject(); }
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
        height = FooterTop() + kButtonH + (Banded() ? 14.0f : kPad);
    }

    void DrawSubjectArt(D2D1_RECT_F box) {
        auto* dc = compositor.Dc();
        if (spec.subject_kind == FormDialogSubject::Archive && !IsHighContrast()) {
            DrawProductTile(dc, box, ProductArt::SevenZip, HexColor(0x527D70), StoreIcon::Box);
            return;
        }
        // 16-grid artwork scaled into the box: a two-tone folder or a page with a fold.
        const float u = (box.right - box.left) / 16.0f;
        auto at = [&](float x, float y, float w, float h) { return D2D1::RectF(box.left + x * u, box.top + y * u, box.left + (x + w) * u, box.top + (y + h) * u); };
        if (spec.subject_kind == FormDialogSubject::Folder) {
            painter.FillRoundedRect(at(1.5f, 3.25f, 5.2f, 3.0f), u, HexColor(0xE8B84A));
            painter.FillRoundedRect(at(1.5f, 4.75f, 13.0f, 8.25f), u, HexColor(0xE8B84A));
            painter.FillRoundedRect(at(1.5f, 6.25f, 13.0f, 6.75f), u, HexColor(0xF4CB64));
        } else {
            painter.FillRoundedRect(at(3.5f, 1.5f, 9.0f, 13.0f), u * 0.8f, HexColor(0x3E6FC2));
            painter.FillRoundedRect(at(9.5f, 1.5f, 3.0f, 3.0f), u * 0.5f, HexColor(0x9DB7E1));
        }
    }

    void DrawBody(const Theme& theme) {
        auto* dc = compositor.Dc();
        const float content_w = Width() - 2 * kPad;
        if (HasHeading()) {
            float x = kPad;
            if (spec.extension_icon.has_value()) {
                DrawExtensionIcon(dc, *spec.extension_icon, DipRect(scale, x, HeadingTop() + 4.0f, 20.0f, 20.0f),
                                  AccentForeground(theme), &theme, 1.5f);
                x += 28.0f;
            } else if (const auto icon = IconOf(spec.icon)) {
                DrawStoreIcon(dc, *icon, DipRect(scale, x, HeadingTop() + 4.0f, 20.0f, 20.0f), AccentForeground(theme), 1.5f);
                x += 28.0f;
            }
            painter.DrawText(spec.heading, DipRect(scale, x, HeadingTop(), Width() - kPad - x, kHeadingH),
                             heading_format.get() ? heading_format.get() : compositor.HeaderFormat(), theme.text);
        }
        if (HasSubject()) {
            const auto card = DipRect(scale, kPad, SubjectTop(), content_w, kSubjectH);
            painter.FillRoundedRect(card, 6.0f * scale, WithAlpha(theme.text, dark ? 0.06f : 0.035f));
            painter.StrokeRoundedRect(card, 6.0f * scale, theme.stroke_card);
            const float art = spec.subject_kind == FormDialogSubject::Archive ? 36.0f : 30.0f;
            DrawSubjectArt(DipRect(scale, kPad + 10.0f + (36.0f - art) * 0.5f, SubjectTop() + (kSubjectH - art) * 0.5f, art, art));
            const float tx = kPad + 56.0f, tw = content_w - 66.0f;
            const bool two = !spec.subject_detail.empty();
            painter.DrawText(spec.subject, DipRect(scale, tx, SubjectTop() + (two ? 9.0f : 18.0f), tw, 20.0f),
                             subject_format.get() ? subject_format.get() : compositor.TextFormat(), theme.text);
            if (two) painter.DrawText(spec.subject_detail, DipRect(scale, tx, SubjectTop() + 29.0f, tw, 18.0f),
                                      compositor.SmallFormat(), theme.text_secondary);
        }
        if (Banded()) {
            const float band = FooterTop() - 14.0f;
            painter.FillRoundedRect(DipRect(scale, 0, band, Width(), height - band), 0,
                                    dark ? HexColor(0x000000, 0.2f) : HexColor(0x000000, 0.045f));
            painter.FillRoundedRect(DipRect(scale, 0, band, Width(), 1), 0, theme.stroke_divider);
        }
        if (!spec.footnote.empty()) {
            const float right = (spec.buttons.empty() ? Width() - kPad : ButtonRect(0).left / scale) - 12.0f;
            float x = kPad;
            if (spec.extension_footnote_icon.has_value()) {
                DrawExtensionIcon(dc, *spec.extension_footnote_icon, DipRect(scale, x, FooterTop() + 9.0f, 14.0f, 14.0f),
                                  theme.text_secondary, &theme, 1.3f);
                x += 20.0f;
            } else if (const auto icon = IconOf(spec.footnote_icon)) {
                DrawStoreIcon(dc, *icon, DipRect(scale, x, FooterTop() + 9.0f, 14.0f, 14.0f), theme.text_secondary, 1.3f);
                x += 20.0f;
            }
            painter.DrawText(spec.footnote, DipRect(scale, x, FooterTop(), (std::max)(0.0f, right - x), kButtonH),
                             compositor.SmallFormat(), theme.text_secondary);
        }
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
            CreateFormats();
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
                                       (Width() - 2 * kPad) * scale, HasHeading() ? theme.text_secondary : theme.text);
        DrawBody(theme);
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
        spec.on_command = nullptr;
        spec.command_enabled = nullptr;
        if (IsWindow(hwnd)) {
            if (owner_guard) owner_guard->RestoreBeforeDialogHide(hwnd);
            HideComposedDialog(hwnd, nullptr);
            tree.Close();
            DestroyWindow(hwnd);
        }
        owner_guard.reset();
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
            if (owner_guard && !owner_guard->Alive()) {
                Finish(spec.cancel_button, true);
                break;
            }
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
            CreateFormats();
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
    self.owner_guard = std::make_unique<pulse::PickerOwnerGuard>(owner);
    if (self.owner_guard->Error() != ERROR_SUCCESS || !self.owner_guard->Alive()) {
        self.owner_guard.reset();
        return false;
    }
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = Impl::Proc;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!GetClassInfoExW(wc.hInstance, kClass, &wc)) RegisterClassExW(&wc);
    const int width = static_cast<int>(std::lround(self.Width() * self.scale));
    if (!CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_CONTROLPARENT, kClass, self.spec.title.c_str(),
                         WS_POPUP | WS_THICKFRAME | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, width,
                         static_cast<int>(200 * self.scale), owner, nullptr, wc.hInstance, &self) || !self.hwnd) {
        self.owner_guard.reset();
        return false;
    }
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
        callbacks.on_command = [weak](const std::string& id) {
            const auto alive = weak.lock();
            if (!alive || alive->done || !alive->spec.on_command) return;
            alive->spec.on_command(id);
        };
        callbacks.command_enabled = [weak](const std::string& id) {
            const auto alive = weak.lock();
            if (!alive || alive->done || !alive->spec.command_enabled) return false;
            return alive->spec.command_enabled(id);
        };
        if (!self.tree.Create(self.hwnd, *self.spec.form, std::move(callbacks), dark, accent, self.scale)) {
            if (self.owner_guard) self.owner_guard->RestoreBeforeDialogHide(self.hwnd);
            DestroyWindow(self.hwnd);
            self.owner_guard.reset();
            return false;
        }
        self.tree.SetBounds(self.FormRect());
        self.Fit(true);
    }
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

bool FormDialog::UpdateForm(const extensions::UiTree& form) {
    if (!IsOpen() || !impl_->tree.Hwnd() || !impl_->tree.UpdateTree(form)) return false;
    impl_->spec.form = form;
    PostMessageW(impl_->hwnd, kFitMessage, 0, 0);
    return true;
}

std::optional<std::string> FormDialog::StateValue(const std::string& key) const {
    if (!IsOpen() || !impl_->tree.Hwnd()) return std::nullopt;
    return impl_->tree.StateValue(key);
}

void FormDialog::RefreshForm() {
    if (IsOpen() && impl_->tree.Hwnd()) impl_->tree.Refresh();
}
void FormDialog::Dismiss() { if (IsOpen()) impl_->Cancel(); }
void FormDialog::Destroy() { if (impl_ && impl_->hwnd && !impl_->done) impl_->Finish(-1, false); }
bool FormDialog::IsOpen() const { return impl_ && impl_->hwnd && !impl_->done; }
HWND FormDialog::Hwnd() const { return IsOpen() ? impl_->hwnd : nullptr; }

} // namespace pulse::ui

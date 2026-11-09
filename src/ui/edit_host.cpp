#include "edit_host.h"
#include "typography.h"
#include <commctrl.h>
#include <algorithm>
#include <memory>
#include <new>
namespace pulse::ui {
constexpr UINT_PTR kEditCaretTimer = 71;
namespace {
// Re-arm the blink timer after a caret move so the next toggle comes one full
// interval later; the painter restarts the blink phase at the same moment.
void RestartCaretBlink(HWND hwnd) {
    if (GetFocus() == hwnd) SetTimer(hwnd, kEditCaretTimer, GetCaretBlinkTime(), nullptr);
}
constexpr wchar_t kPaintedPlain[] = L"Pulse.PaintedPlainEdit";
struct PaintedEditState {
    HWND parent = nullptr;
    DWORD thread = 0;
    DWORD error = ERROR_SUCCESS;
    bool allow_native_raster = false;
};
PaintedEditState* PaintedState(HWND hwnd) {
    return static_cast<PaintedEditState*>(GetPropW(hwnd, kPaintedPlain));
}
LRESULT CALLBACK PaintedLifetimeProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam,
    UINT_PTR id, DWORD_PTR data) {
    if (message == WM_NCDESTROY) {
        auto* state = reinterpret_cast<PaintedEditState*>(data);
        RemovePropW(hwnd, kPaintedPlain);
        RemoveWindowSubclass(hwnd, PaintedLifetimeProc, id);
        const LRESULT result = DefSubclassProc(hwnd, message, wparam, lparam);
        delete state;
        return result;
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
}
void RecordPaintFailure(HWND hwnd) {
    auto* state = PaintedState(hwnd);
    if (!state || state->error) return;
    // No property allocation or queue insertion is required to retain failure.
    state->error = ERROR_FUNCTION_FAILED;
    KillTimer(hwnd, kEditCaretTimer);
    if (GetCurrentThreadId() == state->thread && GetParent(hwnd) == state->parent &&
        GetWindowThreadProcessId(state->parent, nullptr) == state->thread) {
        // The picker handler only marks done/error; it must not destroy the
        // child while this synchronous presentation stack is still active.
        SendMessageW(state->parent, PulsePaintedChildEditFailureMessage(),
            reinterpret_cast<WPARAM>(hwnd), state->error);
    }
}
constexpr wchar_t kNativeEdit[] = L"Pulse.NativeEditFallback";
constexpr wchar_t kPolicyNative[] = L"Pulse.NativeEditPolicy";
constexpr wchar_t kRestoreUploaded[] = L"Pulse.EditRestoreUploaded";
constexpr wchar_t kMigrating[] = L"Pulse.EditMigrating";
void RestoreNativeCaret(HWND hwnd) {
    if (GetFocus() != hwnd) return;
    GUITHREADINFO info{sizeof(info)};
    if (GetGUIThreadInfo(GetCurrentThreadId(), &info) && info.hwndCaret == hwnd) {
        // Custom mouse/paint paths can hide the system caret more than once.
        // Recreate it at the EDIT's current position to reset that hide count.
        const int width = (std::max)(1L, info.rcCaret.right - info.rcCaret.left);
        const int height = (std::max)(1L, info.rcCaret.bottom - info.rcCaret.top);
        if (CreateCaret(hwnd, nullptr, width, height)) SetCaretPos(info.rcCaret.left, info.rcCaret.top);
    }
    ShowCaret(hwnd);
}
// The EDIT shows and moves the system caret only while redraw is on, and the
// text input framework learns about typed text from those caret events. Under
// the redraw guard it never heard of a commit, so after the next one it put
// the selection back where focus had left it: in the search box the second
// word went in front of the first. Report the caret the EDIT skipped; the
// caret itself stays hidden behind the custom painter.
struct EditCaretState {
    DWORD from = 0;
    DWORD to = 0;
    int length = 0;
    bool operator==(const EditCaretState&) const = default;
};
EditCaretState ReadCaretState(HWND hwnd) {
    EditCaretState state;
    SendMessageW(hwnd, EM_GETSEL, reinterpret_cast<WPARAM>(&state.from), reinterpret_cast<LPARAM>(&state.to));
    state.length = GetWindowTextLengthW(hwnd);
    return state;
}
void ReportNativeCaret(HWND hwnd, const EditCaretState& state) {
    if (GetFocus() != hwnd) return;
    const LRESULT pos = SendMessageW(hwnd, EM_POSFROMCHAR, state.to, 0);
    if (pos != -1) SetCaretPos(static_cast<short>(LOWORD(pos)), static_cast<short>(HIWORD(pos)));
    NotifyWinEvent(EVENT_OBJECT_LOCATIONCHANGE, hwnd, OBJID_CARET, CHILDID_SELF);
    NotifyWinEvent(EVENT_OBJECT_SHOW, hwnd, OBJID_CARET, CHILDID_SELF);
}
bool CustomEdit(Compositor& compositor, HWND hwnd) {
    // Keep intercepting native painting even if the renderer becomes unavailable;
    // the strict host will fail, rather than displaying a different text backend.
    if (GetPropW(hwnd, kPaintedPlain)) return true;
    const bool custom = compositor.CustomEditEnabled() && !GetPropW(hwnd, kNativeEdit);
    if (GetPropW(hwnd, kMigrating)) return custom;
    const bool policy_native = GetPropW(hwnd, kPolicyNative) != nullptr;
    if (!custom && !policy_native) {
        SetPropW(hwnd, kMigrating, reinterpret_cast<HANDLE>(1));
        DWORD flags = 0;
        if (!GetLayeredWindowAttributes(hwnd, nullptr, nullptr, &flags) || !(flags & LWA_ALPHA))
            SetPropW(hwnd, kRestoreUploaded, reinterpret_cast<HANDLE>(1));
        SetPropW(hwnd, kPolicyNative, reinterpret_cast<HANDLE>(1));
        // A redirected host may be preparing its first frame at alpha zero.
        // Backend migration must not reveal that unfinished surface.
        if (!(flags & LWA_ALPHA)) SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
        KillTimer(hwnd, kEditCaretTimer);
        RestoreNativeCaret(hwnd);
        InvalidateRect(hwnd, nullptr, TRUE);
        RemovePropW(hwnd, kMigrating);
    } else if (custom && policy_native) {
        SetPropW(hwnd, kMigrating, reinterpret_cast<HANDLE>(1));
        if (RemovePropW(hwnd, kRestoreUploaded)) {
            const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style & ~WS_EX_LAYERED);
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style | WS_EX_LAYERED);
        }
        RemovePropW(hwnd, kPolicyNative);
        if (GetFocus() == hwnd) {
            HideCaret(hwnd);
            SetTimer(hwnd, kEditCaretTimer, GetCaretBlinkTime(), nullptr);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        RemovePropW(hwnd, kMigrating);
    }
    return custom;
}
bool PresentEdit(Compositor& compositor, HWND hwnd, IDWriteTextFormat* format,
    D2D1_COLOR_F foreground, D2D1_COLOR_F background) {
    bool force_failure = false;
#ifdef PULSE_WITH_SELFTEST
    force_failure = GetPropW(hwnd, L"Pulse.Test.LumaPresentFailure") != nullptr;
#endif
    const bool painted_plain = GetPropW(hwnd, kPaintedPlain) != nullptr;
    if (painted_plain && PulsePaintedChildEditError(hwnd)) return false;
    if (painted_plain) {
        // Child messages only invalidate the authoritative main-surface image.
        // Native EDIT remains visible/focusable, but never owns the displayed bitmap.
        if (force_failure || (!PaintedState(hwnd)->allow_native_raster && !compositor.CustomEditEnabled()) || !InvalidateRect(GetParent(hwnd), nullptr, FALSE)) {
            RecordPaintFailure(hwnd); return false;
        }
        ValidateRect(hwnd, nullptr);
        return true;
    }
    if (!force_failure && compositor.PresentLumaEdit(hwnd, format, foreground, background, painted_plain)) return true;
    if (painted_plain) { RecordPaintFailure(hwnd); return false; }
    // Keep native EDIT input, selection and IME together if presentation fails.
    SetPropW(hwnd, kNativeEdit, reinterpret_cast<HANDLE>(1));
    DWORD flags = 0;
    if (!GetLayeredWindowAttributes(hwnd, nullptr, nullptr, &flags) || !(flags & LWA_ALPHA))
        SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
    KillTimer(hwnd, kEditCaretTimer);
    RestoreNativeCaret(hwnd);
    InvalidateRect(hwnd, nullptr, TRUE);
    return false;
}
}
UINT PulsePaintedChildEditFailureMessage() {
    static const UINT message = RegisterWindowMessageW(L"Pulse.PaintedPlainEditFailure.v1");
    return message;
}
DWORD PulsePaintedChildEditError(HWND hwnd) {
    const auto* state = PaintedState(hwnd);
    return state ? state->error : ERROR_SUCCESS;
}
HWND CreatePulsePaintedChildEdit(HWND parent, const wchar_t* text, DWORD edit_style, bool allow_native_raster) {
    const DWORD thread = GetCurrentThreadId();
    if (!parent || GetWindowThreadProcessId(parent, nullptr) != thread) {
        SetLastError(ERROR_INVALID_WINDOW_HANDLE); return nullptr;
    }
    if (!PulsePaintedChildEditFailureMessage()) { SetLastError(ERROR_GEN_FAILURE); return nullptr; }
    std::unique_ptr<PaintedEditState> state(new (std::nothrow) PaintedEditState{parent, thread, ERROR_SUCCESS, allow_native_raster});
    if (!state) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
    SetLastError(ERROR_SUCCESS);
    HWND edit = CreateWindowExW(0, L"EDIT", text,
        WS_CHILD | WS_CLIPSIBLINGS | WS_TABSTOP | ES_AUTOHSCROLL | edit_style,
        0, 0, 0, 0, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!edit) { const DWORD error = GetLastError(); SetLastError(error ? error : ERROR_GEN_FAILURE); return nullptr; }
    if (!SetPropW(edit, kPaintedPlain, state.get())) {
        const DWORD error = GetLastError(); DestroyWindow(edit);
        SetLastError(error ? error : ERROR_GEN_FAILURE); return nullptr;
    }
    // Separate lifetime hook remains installed when the host removes EditProc
    // before destroying its children, including partial WM_CREATE failures.
    if (!SetWindowSubclass(edit, PaintedLifetimeProc, 72, reinterpret_cast<DWORD_PTR>(state.get()))) {
        const DWORD error = GetLastError(); RemovePropW(edit, kPaintedPlain); DestroyWindow(edit);
        SetLastError(error ? error : ERROR_GEN_FAILURE); return nullptr;
    }
    state.release();
    return edit;
}
bool DrawPulsePaintedChildEdit(Compositor& compositor, IDWriteTextFormat* format,
    D2D1_COLOR_F foreground, D2D1_COLOR_F background, HWND hwnd, const D2D1_RECT_F& bounds) {
    if (!PaintedState(hwnd) || PulsePaintedChildEditError(hwnd)) return false;
    if (compositor.DrawLumaEditOnSurface(hwnd, format, foreground, background, bounds)) return true;
    if (!PaintedState(hwnd)->allow_native_raster) { RecordPaintFailure(hwnd); return false; }
    // Rasterize the same native input model offscreen when the custom renderer
    // is unavailable (including high contrast). Never expose a child surface.
    RECT client{};
    GetClientRect(hwnd, &client);
    const int width = client.right, height = client.bottom;
    if (width <= 0 || height <= 0 || !compositor.Dc()) return false;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!dc || !bitmap || !pixels) {
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        RecordPaintFailure(hwnd); return false;
    }
    HGDIOBJ old = SelectObject(dc, bitmap);
    SetPropW(hwnd, L"Pulse.EditOffscreenPrint", reinterpret_cast<HANDLE>(1));
    SendMessageW(hwnd, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT | PRF_ERASEBKGND);
    RemovePropW(hwnd, L"Pulse.EditOffscreenPrint");
    GdiFlush();
    auto* data = static_cast<DWORD*>(pixels);
    for (int i = 0; i < width * height; ++i) data[i] |= 0xff000000u;
    ID2D1Bitmap* image = nullptr;
    const HRESULT hr = compositor.Dc()->CreateBitmap(D2D1::SizeU(width, height), pixels, width * 4,
        D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
            D2D1_ALPHA_MODE_IGNORE)), &image);
    if (SUCCEEDED(hr)) {
        compositor.Dc()->DrawBitmap(image, bounds);
        image->Release();
    }
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    if (FAILED(hr)) RecordPaintFailure(hwnd);
    return SUCCEEDED(hr);
}
bool SynchronizeChildEditBackend(Compositor& compositor, HWND hwnd) {
    return hwnd && CustomEdit(compositor, hwnd);
}
bool PresentChildEdit(Compositor& compositor, IDWriteTextFormat* format,
    D2D1_COLOR_F foreground, D2D1_COLOR_F background, HWND hwnd) {
    if (!hwnd || !CustomEdit(compositor, hwnd)) return false;
    HideCaret(hwnd);
    return PresentEdit(compositor, hwnd, format, foreground, background);
}
bool HandleChildEditMessage(Compositor& compositor, IDWriteTextFormat* format,
    D2D1_COLOR_F foreground, D2D1_COLOR_F background, HBRUSH background_brush,
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LRESULT& result) {
    (void)background_brush;
    if (GetPropW(hwnd, L"Pulse.EditOffscreenPrint") &&
        (msg == WM_PRINTCLIENT || msg == WM_ERASEBKGND)) {
        result = DefSubclassProc(hwnd, msg, wParam, lParam);
        return true;
    }
    if (msg == WM_NCDESTROY) {
        compositor.SynchronizeEditSelection(hwnd, msg, wParam, lParam);
        RemovePropW(hwnd, kNativeEdit);
        RemovePropW(hwnd, kPolicyNative);
        RemovePropW(hwnd, kRestoreUploaded);
        RemovePropW(hwnd, kMigrating);
        return false;
    }
    if (msg == typography::TextBackendChangedMessage()) {
        if (CustomEdit(compositor, hwnd) && IsWindowVisible(hwnd))
            PresentEdit(compositor, hwnd, format, foreground, background);
        result = 0;
        return true;
    }
    if (CustomEdit(compositor, hwnd) &&
        (msg == WM_PRINT || msg == WM_PRINTCLIENT || msg == WM_NCPAINT)) {
        result = 0;
        return true;
    }
    if (CustomEdit(compositor, hwnd) && msg == WM_ERASEBKGND) {
        result = 1;
        return true;
    }
    switch (msg) {
    case WM_LBUTTONDOWN: {
    case WM_LBUTTONDBLCLK:
    case WM_LBUTTONUP:
    case WM_MOUSEMOVE:
    case WM_CAPTURECHANGED:
        if (!CustomEdit(compositor, hwnd)) return false;
        result = compositor.CallLumaEditMouse(
            hwnd, msg, wParam, lParam, format);
        if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK || msg == WM_LBUTTONUP)
            RestartCaretBlink(hwnd);
        if (msg != WM_MOUSEMOVE || GetCapture() == hwnd) {
            PresentEdit(compositor, hwnd, format,
                                         foreground, background);
        }
        return true;
    }
    case WM_PAINT: {
        if (!CustomEdit(compositor, hwnd)) return false;
        HideCaret(hwnd);
        PresentEdit(compositor, hwnd, format, foreground, background);
        // A sticky failure may return before the compositor presents anything.
        // Always consume the invalid region, including the failure path.
        ValidateRect(hwnd, nullptr);
        result = 0;
        return true;
    }
    case WM_SETFOCUS: {
        result = DefSubclassProc(hwnd, msg, wParam, lParam);
        if (CustomEdit(compositor, hwnd)) {
            HideCaret(hwnd);
            compositor.SynchronizeEditSelection(hwnd, msg, wParam, lParam); // caret starts solid
            SetTimer(hwnd, kEditCaretTimer, GetCaretBlinkTime(), nullptr);
            PresentEdit(compositor, hwnd, format,
                                         foreground, background);
        } else {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return true;
    }
    case WM_KILLFOCUS:
        compositor.SynchronizeEditSelection(hwnd, msg, wParam, lParam);
        KillTimer(hwnd, kEditCaretTimer);
        return false;
    case WM_TIMER:
        if (wParam == kEditCaretTimer) {
            if (GetCapture() != hwnd && CustomEdit(compositor, hwnd)) {
                PresentEdit(compositor, hwnd, format,
                                             foreground, background);
            } else if (GetCapture() != hwnd) {
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            result = 0;
            return true;
        }
        return false;
    default:
        return false;
    }
}

LRESULT DefPresentedChildEditProc(Compositor& compositor, IDWriteTextFormat* format, D2D1_COLOR_F foreground, D2D1_COLOR_F background, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_IME_STARTCOMPOSITION) {
        // Start every composition from a caret, not a selection. When a word
        // replaced the selected stem (F2 selects it), Microsoft Pinyin then
        // moved the caret back to the start of the field, so the next word
        // went in front of the first (#116, #162, #211). Deleting the
        // selection first is what the result would do anyway.
        DWORD from = 0, to = 0;
        SendMessageW(hwnd, EM_GETSEL, reinterpret_cast<WPARAM>(&from), reinterpret_cast<LPARAM>(&to));
        if (from != to) SendMessageW(hwnd, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L""));
    }
    const bool changes_visual = msg == WM_SETTEXT || msg == EM_SETSEL || msg == EM_REPLACESEL ||
        msg == WM_KEYDOWN || msg == WM_CHAR || msg == WM_CUT || msg == WM_PASTE ||
        msg == WM_CLEAR || msg == WM_UNDO || msg == EM_UNDO || msg == WM_IME_COMPOSITION ||
        msg == WM_IME_ENDCOMPOSITION || msg == WM_SETFONT || msg == WM_SIZE || msg == EM_SETMARGINS ||
        msg == EM_SETCUEBANNER;
    const bool custom_paint = changes_visual && CustomEdit(compositor, hwnd) && IsWindowVisible(hwnd);
    const EditCaretState before = custom_paint ? ReadCaretState(hwnd) : EditCaretState{};
    if (custom_paint) SendMessageW(hwnd, WM_SETREDRAW, FALSE, 0);
    const LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
    if (changes_visual) compositor.SynchronizeEditSelection(hwnd, msg, wParam, lParam);
    if (custom_paint) {
        SendMessageW(hwnd, WM_SETREDRAW, TRUE, 0);
        const EditCaretState after = ReadCaretState(hwnd);
        if (!(after == before)) ReportNativeCaret(hwnd, after);
        HideCaret(hwnd);
        RestartCaretBlink(hwnd);
        PresentEdit(compositor, hwnd, format,
            foreground, background);
    }
    return result;
}

}

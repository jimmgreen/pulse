#pragma once
#include <windows.h>
#include "ui_compositor.h"

namespace pulse::ui {
// Explicit non-layered presentation of the existing Pulse edit bitmap. Native
// EDIT remains the input/IME model. Optional native raster fallback is drawn
// offscreen into the same parent frame, never onto a separate child surface.
HWND CreatePulsePaintedChildEdit(HWND parent, const wchar_t* text = L"", DWORD edit_style = 0, bool allow_native_raster = false);
DWORD PulsePaintedChildEditError(HWND hwnd);
// Host invokes this within the main compositor's frame, using child-client
// bounds mapped to parent pixels. It keeps the real EDIT's input/IME geometry.
bool DrawPulsePaintedChildEdit(Compositor& compositor, IDWriteTextFormat* format,
    D2D1_COLOR_F foreground, D2D1_COLOR_F background, HWND hwnd, const D2D1_RECT_F& bounds);
UINT PulsePaintedChildEditFailureMessage();
// Applies the selected backend to existing child surfaces; returns custom mode.
bool SynchronizeChildEditBackend(Compositor& compositor, HWND hwnd);
bool HandleChildEditMessage(Compositor& compositor, IDWriteTextFormat* format,
    D2D1_COLOR_F foreground, D2D1_COLOR_F background, HBRUSH background_brush,
    HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, LRESULT& result);
// Presents a child editor's LumaText bitmap (e.g. right after showing it).
// When the layered present fails, the EDIT switches to native painting so it
// stays visible and clickable. Returns true when the LumaText bitmap is shown.
bool PresentChildEdit(Compositor& compositor, IDWriteTextFormat* format,
    D2D1_COLOR_F foreground, D2D1_COLOR_F background, HWND hwnd);
LRESULT DefPresentedChildEditProc(Compositor& compositor, IDWriteTextFormat* format,
    D2D1_COLOR_F foreground, D2D1_COLOR_F background,
    HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
// Height of a hosted EDIT: one line of its font, never taller than the field
// it sits in (callers centre it vertically in the field).
inline int EditLineHeight(HWND edit, HFONT font, int field_height) {
    int line = field_height;
    if (edit && font) {
        HDC hdc = GetDC(edit);
        HFONT old = static_cast<HFONT>(SelectObject(hdc, font));
        TEXTMETRICW tm{};
        GetTextMetricsW(hdc, &tm);
        SelectObject(hdc, old);
        ReleaseDC(edit, hdc);
        line = tm.tmHeight > 1 ? static_cast<int>(tm.tmHeight) : 1;
    }
    return line < field_height ? line : field_height;
}
// EDIT owns native editing and IME. Always use a Windows-redirected child
// surface: directly uploaded layered bitmaps can be hidden by composition.
// Keep this policy here so every host inherits the same presentation mode.
inline HWND CreateChildEdit(HWND parent, const wchar_t* text = L"", DWORD edit_style = 0) {
    HWND edit = CreateWindowExW(WS_EX_LAYERED, L"EDIT", text,
        WS_CHILD | WS_CLIPSIBLINGS | WS_TABSTOP | ES_AUTOHSCROLL | edit_style,
        0, 0, 0, 0, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (edit && !SetLayeredWindowAttributes(edit, 0, 255, LWA_ALPHA)) {
        DestroyWindow(edit);
        return nullptr;
    }
    return edit;
}
// Composition hosts need a redirected child surface: an uploaded layered
// bitmap can end up behind the parent's DirectComposition surface.
inline HWND CreateRedirectedChildEdit(HWND parent, const wchar_t* text = L"", DWORD edit_style = 0) {
    HWND edit = CreateChildEdit(parent, text, edit_style);
    if (edit) SetLayeredWindowAttributes(edit, 0, 255, LWA_ALPHA);
    return edit;
}
}

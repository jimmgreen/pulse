#pragma once
#include <windows.h>

namespace pulse::ui {
// EDIT owns native editing and IME. Its LumaText bitmap is a child surface,
// clipped and moved by the parent rather than an independently owned popup.
inline HWND CreateChildEdit(HWND parent, const wchar_t* text = L"", DWORD edit_style = 0) {
    return CreateWindowExW(WS_EX_LAYERED, L"EDIT", text,
        WS_CHILD | WS_CLIPSIBLINGS | WS_TABSTOP | ES_AUTOHSCROLL | edit_style,
        0, 0, 0, 0, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
}
}

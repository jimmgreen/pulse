#pragma once
#include <windows.h>
#include <oleidl.h>
namespace pulse::ui {
inline constexpr UINT kPickerPlacesTestPointMessage = WM_APP + 90;
inline constexpr UINT kPickerOleTestMessage = WM_APP + 91;
struct PickerOleTestDrop { const wchar_t* source; POINT point; DWORD effect = 0; DWORD keys = MK_CONTROL; DWORD allowed = DROPEFFECT_COPY; const wchar_t* second_source = nullptr; DWORD logical_effect = 0; DWORD performed_effect = 0; };
struct PickerPlacesTestPoint {
    enum class Kind { Row, Sidebar, FavoriteHeader };
    Kind kind = Kind::Row;
    const wchar_t* path = nullptr;
    POINT point{};
};
}

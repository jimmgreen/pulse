#pragma once
#include <windows.h>
namespace pulse::ui {
inline constexpr UINT kPickerPlacesTestPointMessage = WM_APP + 90;
inline constexpr UINT kPickerOleTestMessage = WM_APP + 91;
struct PickerOleTestDrop { const wchar_t* source; POINT point; DWORD effect = 0; };
struct PickerPlacesTestPoint {
    enum class Kind { Row, Sidebar, FavoriteHeader };
    Kind kind = Kind::Row;
    const wchar_t* path = nullptr;
    POINT point{};
};
}

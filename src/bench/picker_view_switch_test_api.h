#pragma once
#include <windows.h>
#include <array>

namespace pulse::ui {
inline constexpr UINT kPickerViewSwitchTestMessage = WM_APP + 92;
struct PickerViewSwitchTest {
    bool begin = false;
    bool timer = false;
    int mode = 0;
    bool ready = false;
    size_t count = 0;
    std::array<LONGLONG, 256> frames{};
};
}

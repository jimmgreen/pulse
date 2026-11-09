#pragma once
#include <windows.h>
#include <string>
#include <vector>
namespace pulse::ui {
inline constexpr UINT kPickerInteractionTestMessage = WM_APP + 93;
struct PickerInteractionTest {
    UINT key = 0;
    int command = 0;
    int edit_id = 0;
    bool ctrl = false, shift = false;
    bool folder = false;
    bool ready = false, visible = false, listing_ready = false;
    int mode = 0;
    float dip = 0;
    POINT item{}, screen_item{}, blank{};
    bool blank_ready = false;
    std::wstring current;
    std::vector<std::wstring> selected_paths;
    bool selected_visible = false;
    HWND rename_edit = nullptr;
    RECT card{}, footer{};
};
}

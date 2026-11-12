#pragma once
#include <windows.h>
#include <cstdint>

namespace pulse::ui {
inline constexpr UINT kPickerJumpUiTestMessage = WM_APP + 96;
struct PickerJumpUiTest {
    UINT size = sizeof(PickerJumpUiTest);
    UINT version = 1;
    bool ready = false;
    RECT options[4]{};
    unsigned option_count = 0;
    RECT more{};
    POINT row_checks[2]{};
    unsigned row_check_count = 0;
    RECT remove_selection[2]{};
    unsigned remove_count = 0;
    int last_down_hit = -1, last_up_hit = -1, last_activate = -1, current_pressed = -1;
    const wchar_t* jump_path = nullptr;
    bool jump_file = false;
    RECT context_filters[2]{};
    unsigned context_filter_count = 0;
    RECT new_folder{};
    RECT overwrite_replace{}, overwrite_rename{};
    bool primary_enabled = false;
    const wchar_t* recent_path = nullptr;
    DWORD recent_attributes = 0;
    uint64_t recent_modified = 0, recent_size = 0;
    unsigned recent_bitmap_draws = 0;
    RECT footer_tool{};
};
}

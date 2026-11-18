#pragma once
#include <windows.h>
#include <array>
#include <string>
#include <vector>
namespace pulse::picker_layout {
struct State {
    int view_mode = 0, sort = 0;
    bool descending = false, maximized = false;
    float sidebar_width = 196;
    float icon_dip = 0;
    unsigned collapsed_mask = 0;
    std::array<float, 5> column_widths{};
    std::vector<std::wstring> hidden_places;
    bool has_placement = false;
    wchar_t monitor[32]{};
    float x = 0, y = 0, width = 900, height = 650;
};
bool Load(int mode, State& state);
bool Save(int mode, const State& state);
bool LoadForIdentity(const std::wstring& root, const std::wstring& identity, int mode, State& state);
bool SaveForIdentity(const std::wstring& root, const std::wstring& identity, int mode, const State& state);
bool RestorePlacement(HWND window, HWND owner, const State& state);
void CapturePlacement(HWND window, State& state);
}


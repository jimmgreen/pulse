#pragma once
#include <windows.h>
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace pulse::picker_layout {
struct State {
    int view_mode = 0, sort = 0;
    bool descending = false, maximized = false;
    float sidebar_width = 208;
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
// Save types this program last saved with (their filter patterns, most recent
// first, at most kMaxRecentFormats), so the format segments keep them in view.
inline constexpr size_t kMaxRecentFormats = 4;
std::vector<std::wstring> LoadRecentFormats(int mode);
bool SaveRecentFormats(int mode, const std::vector<std::wstring>& patterns);
std::vector<std::wstring> LoadRecentFormatsForIdentity(const std::wstring& root, const std::wstring& identity, int mode);
bool SaveRecentFormatsForIdentity(const std::wstring& root, const std::wstring& identity, int mode, const std::vector<std::wstring>& patterns);
// When this program last opened the dialog in `mode` (FILETIME ticks, UTC; 0: never),
// so the open filter bar can offer the files that appeared since (新文件).
// Per-program view switches (bit flags), e.g. the preview pane.
inline constexpr uint32_t kFlagPreviewPane = 1;
inline constexpr uint32_t kFlagRemoveTrayAfterOpen = 2;
inline constexpr uint32_t kFlagShowAfterSave = 4;
inline constexpr uint32_t kFlagCopyAfterSave = 8;
inline constexpr uint32_t kFlagPreviewPaneHidden = 16;
uint32_t LoadFlagsForIdentity(const std::wstring& root, const std::wstring& identity, int mode);
bool SaveFlagsForIdentity(const std::wstring& root, const std::wstring& identity, int mode, uint32_t flags);
uint32_t LoadFlags(int mode);
bool SaveFlags(int mode, uint32_t flags);
uint64_t LoadLastOpened(int mode);
bool SaveLastOpened(int mode, uint64_t ticks);
inline constexpr size_t kMaxAppPinnedPlaces = 16;
std::vector<std::wstring> LoadAppPinnedForIdentity(const std::wstring& root, const std::wstring& identity);
bool SaveAppPinnedForIdentity(const std::wstring& root, const std::wstring& identity, const std::vector<std::wstring>& paths);
std::vector<std::wstring> LoadAppPinned();
bool SaveAppPinned(const std::vector<std::wstring>& paths);
bool RestorePlacement(HWND window, HWND owner, const State& state);
void CapturePlacement(HWND window, State& state);
}


// app_prefs.h — General app settings (startup, close-to-tray).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace pulse::app {

struct AppPrefs {
    bool persist = true;
    bool launch_on_startup = false;
    bool keep_running_on_close = false;
    bool open_folders_in_pulse = false;
    bool verify_copies = false;
    bool show_status_performance = false;
    bool show_pinned_tab_names = true;
    bool show_hidden_files = false;
    // system / zh-CN / en-US
    std::wstring language = L"system";
    // none / acrylic-material / mica / mica-alt  (legacy dwm-blur → acrylic)
    std::wstring window_effect = L"mica-alt";
    std::wstring background_image;
    int row_height = 34; // file-list row height in DIPs (24..48)
    int tray_icon_size = 48; // staging-tray deck icon edge in DIPs (32..64)
    // Empty = follow Windows accent; otherwise "RRGGBB".
    std::wstring accent_rgb;
    // Tag colors the user added via the custom color dialog (0xRRGGBB),
    // appended after the seven Finder defaults in the swatch strip.
    std::vector<uint32_t> custom_tag_colors;
    int duplicate_scan_scope = 0; // 0 folder, 1 drive, 2 all local disks
    std::wstring duplicate_scan_folder;
    std::wstring duplicate_scan_drive;

    void ResetToDefaults();
    bool Load();
    bool Save() const;
    std::wstring ToJson() const;
    bool FromJson(const std::wstring& json);

    // HKCU Run key is the source of truth; call after Load() and on toggle.
    bool ReadLaunchOnStartup() const;
    bool ApplyLaunchOnStartup(bool on);

    // HKCU Directory/Drive open verbs; call after Load() and on toggle.
    bool ReadFolderOpen() const;
    bool ApplyFolderOpen(bool on);

    bool StoreBackgroundImage(const std::wstring& source_path);
    void ClearBackgroundImage();
};

std::wstring FolderOpenCommandLine(const std::wstring& exe);
bool FolderOpenCommandIsOurs(const std::wstring& command, const std::wstring& exe);
bool ParseAccentRgb(const std::wstring& text, uint32_t& rgb) noexcept;

} // namespace pulse::app

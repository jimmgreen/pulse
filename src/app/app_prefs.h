// app_prefs.h — General app settings (startup, close-to-tray).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace pulse::app {

// Everything app.json stores. Split out of AppPrefs so the object can also hold
// the state that last agreed with the file and compare the two field by field.
// A new setting goes here and into AppPrefs::MergedWithDisk() in app_prefs.cpp.
struct AppPrefsValues {
    bool launch_on_startup = false;
    bool keep_running_on_close = false;
    bool open_folders_in_pulse = false;
    bool verify_copies = false;
    bool show_status_performance = false;
    bool show_pinned_tab_names = true;
    bool search_pinyin = true;
    bool global_search_enabled = false;
    uint32_t global_search_modifiers = 1; // MOD_ALT
    uint32_t global_search_key = 32; // VK_SPACE
    bool show_hidden_files = false;
    // Hidden + system attributes; File Explorer keeps these behind a separate option.
    bool show_protected_os_files = false;
    bool blank_click_go_back = false;
    bool change_tracking_enabled = false;
    int change_tracking_days = 7;
    // system / zh-CN / en-US
    int theme_mode = -1; // legacy session theme, or 0 system / 1 light / 2 dark
    std::wstring language = L"system";
    // none / acrylic-material / mica / mica-alt  (legacy dwm-blur → acrylic)
    std::wstring window_effect = L"mica-alt";
    std::wstring background_image;
    int row_height = 34; // file-list row height in DIPs (24..48)
    int sidebar_width = 224; // DIPs
    bool address_search_current = false;
    bool address_search_content = false;
    int tray_icon_size = 48; // staging-tray deck icon edge in DIPs (32..64)
    // Empty = follow Windows accent; otherwise "RRGGBB".
    std::wstring accent_rgb;
    // Tag colors the user added via the custom color dialog (0xRRGGBB),
    // appended after the seven Finder defaults in the swatch strip.
    std::vector<uint32_t> custom_tag_colors;
    int duplicate_scan_scope = 0; // 0 folder, 1 drive, 2 all local disks
    std::wstring duplicate_scan_folder;
    std::wstring duplicate_scan_drive;
};

struct AppPrefs : AppPrefsValues {
    bool persist = true;

    void ResetToDefaults();
    bool Load();
    bool Save() const;
    std::wstring ToJson() const;
    bool FromJson(const std::wstring& json);

    // True when the last Load() found a usable state in app.json or its backup.
    bool loaded_from_file() const noexcept { return loaded_from_file_; }

    // app.json holds the intent; the HKCU Run key is only a projection of it that
    // ReconcileRegistryWithFile() keeps in step. ReadLaunchOnStartup() reports the
    // machine's current state, which Load() consults only for the first migration
    // (when there is no usable file). The settings toggle goes through
    // ApplyLaunchOnStartup(), which also writes the key.
    bool ReadLaunchOnStartup() const;
    bool ApplyLaunchOnStartup(bool on);

    // Same split for the HKCU folder-open verbs (Directory and Drive, plus the Folder
    // class's open and explore): app.json holds the intent, ReadFolderOpen() reports
    // the current state, Load() consults it only when no usable file exists, and the
    // toggle goes through ApplyFolderOpen(). Explorer's Win+E entry point (a CLSID of
    // its own) is deliberately left alone.
    bool ReadFolderOpen() const;
    bool ApplyFolderOpen(bool on);

    bool StoreBackgroundImage(const std::wstring& source_path);
    void ClearBackgroundImage();

private:
    // The values the file held at the last Load() or Save(). Save() merges against
    // them, so a window that has been open for a while cannot roll back what
    // another Pulse window wrote in the meantime. mutable: Save() stays const.
    mutable AppPrefsValues disk_state_;
    bool loaded_from_file_ = false;

    // Reads the state app.json holds, falling back to app.json.bak. |main_exists|
    // reports whether app.json itself is there, which is what tells Save() an
    // unreadable file apart from a missing one; |used_backup| (optional) reports that
    // the main file was unusable, so its bytes must not overwrite the backup.
    bool ReadDiskState(AppPrefsValues& values, bool& main_exists,
                       bool* used_backup = nullptr) const;
    // Fields that changed here since disk_state_ keep the local value; every other
    // field takes the value found on disk. Fields missing from this function keep
    // the old behaviour (local value always wins), never a lost setting.
    AppPrefsValues MergedWithDisk(const AppPrefsValues& disk) const;
    // Applies the file's intent to the two registry-backed toggles: writes the key
    // an install lost, repoints one that still names an older Pulse, clears what the
    // file no longer wants, and never touches a verb another program owns. Returns
    // true when a value had to be adopted back from the registry, which means the
    // file is out of date and has to be written again.
    bool ReconcileRegistryWithFile();
};

std::wstring FolderOpenCommandLine(const std::wstring& exe);
bool FolderOpenCommandIsOurs(const std::wstring& command, const std::wstring& exe);
bool ParseAccentRgb(const std::wstring& text, uint32_t& rgb) noexcept;

} // namespace pulse::app

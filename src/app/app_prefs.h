// app_prefs.h — General app settings (startup, close-to-tray).
#pragma once
#include "folder_view_prefs.h"
#include "folder_sort_prefs.h"
#include "entry_group.h"
#include "../ui/details_column_set.h"
#include <cstdint>
#include <string>
#include <vector>

namespace pulse::app {

struct AppPrefs {
    bool persist = true;
    bool launch_on_startup = false;
    bool keep_running_on_close = false;
    // notify_icon_mode: the notification-area icon while closing keeps Pulse
    // running: 0 always, 1 only while the window is closed to it, 2 never
    // (a second launch brings the window back; #57).
    int notify_icon_mode = 0;
    bool open_folders_in_pulse = false;
    bool take_over_win_e = false;
    bool take_over_this_pc = false; // HKCU This PC open verb (default_file_manager.h)
    bool take_over_recycle_bin = false; // HKCU Recycle Bin open verb (default_file_manager.h)
    bool integration_enabled = false;
    bool integration_folders = true;
    bool integration_win_e = true;
    bool integration_this_pc = true;
    // New installs take the Recycle Bin over with the rest; preferences saved
    // before it existed keep it unselected (no false "changed" warning).
    bool integration_recycle_bin = true;
    bool integration_configured = false; // runtime: new-format preferences were loaded
    bool integration_residual = false; // runtime: some association still belongs to Pulse
    bool integration_incomplete = false;
    std::wstring integration_other_exe; // runtime: another Pulse copy owns the associations
    bool take_over_explorer_windows = false; // experimental choice; runtime is gated by integration_enabled
    bool shell_tag_menu = false;    // File Explorer "Pulse tags" submenu (shell_tag_menu.cpp syncs HKCU)   // registry is the source of truth (not in app.json)
    bool verify_copies = false;
    bool show_status_performance = false;
    bool show_pinned_tab_names = true;
    // Details list presentation.
    bool list_smart_date = true;
    bool list_zebra_rows = true;
    bool list_size_bar = false;
    bool list_tag_name_color = false; // tint tagged names with their first tag's color
    bool list_selection_outline = false; // accent outline around selected items (#78)
    // Grid thumbnails: default-program badge, plus playing time on videos.
    bool list_thumbnail_badges = true;
    // Item selection checkboxes: "auto" (default), "always", "never"
    std::wstring item_checkboxes = L"auto";
    // Touch UI density mode: "auto" (default), "compact", "touch"
    std::wstring touch_ui_mode = L"auto";
    bool vertical_tabs = false;       // tabs as the first sidebar section
    bool sidebar_collapsed = false;   // sidebar folded to its icon rail (Ctrl+B)
    // 0 folders first, 1 follow the sort direction, 2 mixed with files
    int folder_sort_mode = 0;
    // Optional Details columns for every folder (ui/details_column_set.h bits).
    uint32_t details_columns = ui::kDetailsColumnsDefault;
    // Where Pulse opens. home_folder is the default location; empty means This PC.
    // startup_open: 0 restore the last tabs, 1 open the default location.
    // new_tab_open: 0 the current folder, 1 the default location.
    int startup_open = 0;
    int new_tab_open = 0;
    // external_open: where a folder opened from another program goes
    // (multi-window M7, plan §6): 0 smart, 1 always a tab, 2 always a new window.
    int external_open = 0;
    // Closing the only tab closes the window (app/last_tab_close.h). New installs
    // follow File Explorer (Ctrl+W on the only tab closes the window); a saved file
    // without the key predates the setting and keeps the window (see FromJson).
    bool close_window_with_last_tab = true;
    // Double-clicking a tab closes it, like its close button (app/last_tab_close.h).
    bool close_tab_on_double_click = false;
    bool confirm_recycle_delete = false; // ask before Delete moves items to the Recycle Bin
    // Sign-in launches (Run value with --startup) stay hidden behind the tray icon.
    bool start_in_tray = false;
    std::wstring home_folder;
    // Text rendering: 0 auto (LumaText), 1 sharp (pixel-snapped DirectWrite), 2 smooth
    int text_render = 0;
    // Interface font size in percent of the built-in sizes: 90 / 100 / 112 / 125.
    int ui_font_scale = 100;
    FolderViewPrefs folder_views;
    FolderSortPrefs folder_sorts;
    FolderGroupPrefs folder_groups;
    bool search_pinyin = true;
    bool global_search_enabled = false;
    uint32_t global_search_modifiers = 1; // MOD_ALT
    uint32_t global_search_key = 32; // VK_SPACE
    bool show_hidden_files = false;
    // Hidden + system attributes; File Explorer keeps these behind a separate option.
    bool show_protected_os_files = false;
    // Double click on empty list space: 0 nothing, 1 back, 2 up (blank_pane_click.h).
    int blank_click_action = 0;
    // Quick look trigger (Settings > 快速预览): 0 a card appears after the
    // pointer rests on a file (hover_preview_delay_ms), Space still opens the
    // full preview; 1 only Space previews. See kPreviewTrigger*.
    int preview_trigger = 0;
    int hover_preview_delay_ms = 500; // 300 / 500 / 800
    // The card's size step: 0 small, 1 medium, 2 large (kHoverCardSize*). The
    // card takes the content's shape within it; Ctrl enlarges it while held.
    int hover_card_size = 1;
    bool change_tracking_enabled = false;
    int change_tracking_days = 7;
    // system / zh-CN / en-US
    int theme_mode = -1; // legacy session theme, or 0 system / 1 light / 2 dark
    std::wstring language = L"system";
    // none / acrylic-material / mica / mica-alt  (legacy dwm-blur → acrylic)
    std::wstring window_effect = L"mica-alt";
    // Menus blur the window under them (WinUI in-app acrylic); see FluentMenu.
    bool menu_acrylic = true;
    std::wstring background_image;
    // Interface transparency 0..90 (json panel_transparency). 25/50/75 match the
    // former subtle/balanced/vivid levels; applies to image, Acrylic and Mica.
    int wallpaper_look = 50;
    int wallpaper_blur = 14; // wallpaper blur in DIPs 0..40 (json wallpaper_blur_px)
    int row_height = 34; // file-list row height in DIPs (24..48)
    int sidebar_width = 224; // DIPs
    bool address_search_current = false;
    bool address_search_content = false;
    int tray_icon_size = 48; // staging-tray deck icon edge in DIPs (32..64)
    // Interaction hints: status-bar context hints + one-time teaching tips.
    bool show_hints = true;
    // Background update checks and their "update available" toasts (Settings > About).
    bool auto_check_updates = true;
    uint32_t tips_seen = 0; // bit per app::TeachTip already shown or dismissed
    // 自定义工具栏 (toolbar redesign note 5): the New button's main part makes
    // 0 a folder, 1 the type made last (toolbar_last_new: 0 folder, 1 text
    // document), 2 nothing: the whole button opens the New menu. toolbar_pins
    // has a bit per pinnable command (kToolbarPins in app_commands.cpp).
    int toolbar_new_primary = 0;
    int toolbar_last_new = 0;
    uint32_t toolbar_pins = 0;
    // Programs pinned to the toolbar (自定义工具栏 → 固定的软件), in toolbar
    // order: .exe paths or shortcuts to them, at most app::kMaxToolbarApps.
    std::vector<std::wstring> toolbar_apps;
    // Empty = theme default, or Windows when explicitly selected.
    std::wstring accent_rgb;
    bool accent_follow_system = false;
    // Tag colors the user added via the custom color dialog (0xRRGGBB),
    // appended after the seven Finder defaults in the swatch strip.
    std::vector<uint32_t> custom_tag_colors;
    int duplicate_scan_scope = 0; // 0 folder, 1 drive, 2 all local disks
    std::wstring duplicate_scan_folder;
    std::wstring duplicate_scan_drive;
    // Version that last ran with these prefs; drives the one-time "updated" toast.
    std::wstring last_seen_version;
    std::wstring tray_dests; // staging tray: recent drop folders, newest first, '|'-joined (max 3)
    bool had_file = false; // runtime only: app.json existed when Load() ran
    bool load_failed = false; // blocks saving defaults over an unreadable/damaged existing file

    void ResetToDefaults();
    bool Load();
    bool Save() const;
    std::wstring ToJson() const;
    bool FromJson(const std::wstring& json);
    void MigrateIntegration();
    bool ReadIntegrationResidual() const;
    bool ReadIntegrationIncomplete() const;
    std::wstring ReadIntegrationOtherExe() const;

    // HKCU Run key is the source of truth; call after Load() and on toggle.
    bool ReadLaunchOnStartup() const;
    bool ApplyLaunchOnStartup(bool on);

    // HKCU Directory/Drive open verbs; call after Load() and on toggle.
    bool ReadFolderOpen() const;
    bool ApplyFolderOpen(bool on);

    // HKCU "File Explorer" launch verb used by Win+E and the taskbar Explorer
    // pin ({52205fd8-...}\shell\opennewwindow). Original values are kept by
    // shell_integration_registry and restored only while Pulse still owns them.
    bool ReadWinE() const;
    bool ApplyWinE(bool on);

    bool StoreBackgroundImage(const std::wstring& source_path);
    void ClearBackgroundImage();

    // Closing the last window hides it to the tray instead of exiting. Only
    // "keep running after close" decides this: features that need a running
    // Pulse (global search, Explorer window takeover) work while it runs and
    // stop with it, so turning the switch off really ends Pulse on close.
    bool KeepsRunningInBackground() const {
        return keep_running_on_close;
    }
};

constexpr int kPreviewTriggerHover = 0;
constexpr int kPreviewTriggerSpace = 1;
inline int NormalizePreviewTrigger(int value) noexcept {
    return value == kPreviewTriggerSpace ? kPreviewTriggerSpace : kPreviewTriggerHover;
}
inline int NormalizeHoverPreviewDelay(int ms) noexcept {
    return ms == 300 || ms == 800 ? ms : 500;
}
constexpr int kHoverCardSmall = 0;
constexpr int kHoverCardMedium = 1;
constexpr int kHoverCardLarge = 2;
inline int NormalizeHoverCardSize(int value) noexcept {
    return value == kHoverCardSmall || value == kHoverCardLarge ? value : kHoverCardMedium;
}

std::wstring FolderOpenCommandLine(const std::wstring& exe);
bool FolderOpenCommandIsOurs(const std::wstring& command, const std::wstring& exe);
bool ParseAccentRgb(const std::wstring& text, uint32_t& rgb) noexcept;
// Menu row height for a file-list row height: 2 DIPs taller, kept within
// 28..40, so 紧凑/标准/宽松 (28/34/40) give 30/36/40 (#27).
int MenuRowHeightDip(int list_row_height) noexcept;
// Interface font size: only 90/100/112/125 are kept, anything else is 100.
int NormalizeUiFontScale(int percent) noexcept;
// List row height actually used: the chosen density, raised so larger
// interface fonts still fit (112% -> at least 32, 125% -> at least 35).
int EffectiveRowHeightDip(int list_row_height, int ui_font_scale) noexcept;

} // namespace pulse::app

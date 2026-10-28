// app_prefs.h — General app settings (startup, close-to-tray).
#pragma once
#include "folder_view_prefs.h"
#include "folder_sort_prefs.h"
#include "entry_group.h"
#include "../ui/details_column_set.h"
#include "toolbar_selection.h"
#include "toolbar_tools.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::app {

// Remembered folder-picker entry points (AppPrefs::picker_folders).
inline constexpr size_t kPickerFolderLimit = 32;

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
    // 兼容打开方式: leave Directory/Drive open alone and let Explorer open the
    // folder, then take its window over. For machines where the custom default
    // verb never reaches Pulse (a context-menu extension stalls the lookup).
    bool integration_folders_compat = false;
    bool shell_tag_menu = false;    // File Explorer "Pulse tags" submenu (shell_tag_menu.cpp syncs HKCU)   // registry is the source of truth (not in app.json)
    bool verify_copies = false;
    bool show_status_performance = false;
    bool show_pinned_tab_names = true;
    // Details list presentation.
    bool list_smart_date = true;
    bool list_zebra_rows = true;
    bool list_tree_sticky = true;      // 滚动时固定上级: details tree folders stay pinned
    bool list_size_bar = false;
    // Folders in the list get their size worked out on their own; off, the
    // size cell offers it per folder (a size sort still asks for them all).
    bool folder_size_auto = true;
    bool photo_wall_hint = true;      // suggest the photo wall in photo folders (once per folder)
    bool list_tag_name_color = false; // tint tagged names with their first tag's color
    bool list_selection_outline = false; // accent outline around selected items (#78)
    // Grid thumbnails: default-program badge, plus playing time on videos.
    bool list_thumbnail_badges = true;
    // 修改时间提示 (ui/mtime_hint.h): 0 off, 1 dots, 2 bars, 3 text only.
    int list_mtime_hint = 1;
    int list_mtime_range = 7;         // days: 1, 3, 7, 30
    int list_mtime_palette = 0;       // 0 accent, then 冷暖 / 青碧 / 琥珀 / 极光 / 石墨
    bool list_mtime_fade = true;
    bool list_mtime_tree = true;      // folders follow change tracking's newest change inside
    bool list_mtime_other_views = true;
    bool list_mtime_bold = true;
    bool list_mtime_flash = true;
    // 鼠标与选择: single-click opens items (0 = double-click), the name band
    // that hints it (0 on hover, 1 always), hover selection, item check boxes.
    bool single_click_open = false;
    int name_highlight = 0;
    bool hover_select = false;
    bool item_checkboxes = false;
    // 同步状态: OneDrive placeholders and SVN working copies.
    bool sync_onedrive = true;
    bool sync_svn = true;
    bool vertical_tabs = false;       // tabs as the first sidebar section
    bool tray_hide_when_empty = false; // staging tray folds away while empty (drags still show it)
    // 网络位置 (Settings → 文件操作): machine-local, not in settings export.
    bool lan_discovery = true;           // look for LAN devices when the network page opens
    bool network_recent_enabled = true;  // remember and list 最近访问
    bool show_hidden_shares = false;     // list C$ / ADMIN$ in a server's shares
    int lan_cache_mode = 0;              // keep found devices: 0 10 min, 1 1 h, 2 this session
    bool sidebar_collapsed = false;   // sidebar folded to its icon rail (Ctrl+B)
    bool favorites_bar = false;       // 收藏栏 under the command row (Ctrl+Shift+B)
    bool favorites_bar_icon_only = false;
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
    // Win+E (any empty-path launch) while Pulse runs only brings the most
    // recent window forward instead of opening a new one (external_open rule 1.5).
    bool win_e_activate_existing = false;
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
    // Search master switch: index service, search box, global and content search.
    // Off leaves only the folder filter; the service exits at start until it is on.
    bool search_enabled = true;
    uint32_t global_search_modifiers = 1; // MOD_ALT
    uint32_t global_search_key = 32; // VK_SPACE
    // Quick search popup size in DIP after the user resized it; 0 = default.
    int global_search_width = 0;
    int global_search_height = 0;
    // Two clean Ctrl taps summon the quick search popup too (with global search on).
    bool global_search_double_ctrl = true;
    int global_search_double_ctrl_ms = 300; // longest gap between the taps: 200 / 300 / 400
    bool dialog_jump_bar = true;             // jump bar under other programs' Open / Save dialogs
    bool dialog_jump_auto = false;           // a new dialog goes to the current Pulse folder once
    bool jump_history = true;                // frequent places first (jump_history.tsv)
    bool global_search_apps = true;          // Start menu programs in quick search results
    bool show_hidden_files = false;
    // 文件扩展名 (a toolbar switch): off draws files of a registered type
    // without their extension, as File Explorer can (ui/known_extensions.h).
    bool show_file_extensions = true;
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
    // 侧边栏与标题栏 0..100 under Acrylic / Mica (json frame_transparency):
    // 0 solid, 10 the look from before the setting, 100 the material itself.
    int frame_transparency = 10;
    int row_height = 34; // file-list row height in DIPs (24..48)
    int sidebar_width = 224; // DIPs
    bool address_search_current = false;
    bool address_search_content = false;
    // Unscoped content searches (content_scope_mode.h): 0 not chosen yet, which
    // searches the user folders and asks once; 1 Desktop/Documents/Downloads;
    // 2 all drives.
    int content_scope = 0;
    int tray_icon_size = 48; // staging-tray deck icon edge in DIPs (32..64)
    // Interaction hints: status-bar context hints + one-time teaching tips.
    bool show_hints = true;
    // Background update checks and their "update available" toasts (Settings > About).
    bool auto_check_updates = true;
    uint32_t tips_seen = 0; // bit per app::TeachTip already shown or dismissed
    // 自定义工具栏 (toolbar redesign note 5): the New button's main part makes
    // 0 a folder, 1 the type made last (toolbar_last_new: 0 folder, 1 text
    // document), 2 nothing: the whole button opens the New menu.
    int toolbar_new_primary = 0;
    int toolbar_last_new = 0;
    // Tools pinned to the toolbar, in toolbar order, by id (toolbar_tools.h).
    // Saved as json toolbar_tools (+ toolbar_tools_v); older prefs had the
    // toolbar_pins mask. A fresh install gets the prototype's default.
    std::vector<std::wstring> toolbar_tools = DefaultToolbarTools();
    // 排序 / 筛选 / 分组 at the toolbar's right (自定义工具栏 → 排序与筛选).
    // An active filter or grouping shows its control even when hidden.
    bool toolbar_show_sort = true;
    bool toolbar_show_filter = true;
    bool toolbar_show_group = true;
    // Actions the selection chip may show, in chip order, by id
    // (toolbar_selection.h). Without the json key: all, default order.
    std::vector<std::wstring> toolbar_selection = DefaultToolbarSelection();
    // Programs pinned to the toolbar (自定义工具栏 → 固定的软件), in toolbar
    // order: .exe paths or shortcuts to them, at most app::kMaxToolbarApps.
    std::vector<std::wstring> toolbar_apps;
    // Pinned apps followed by a separator (paths from toolbar_apps); the
    // separator moves with its app and goes when the app is unpinned.
    std::vector<std::wstring> toolbar_app_breaks;
    // Empty = theme default, or Windows when explicitly selected.
    std::wstring accent_rgb;
    bool accent_follow_system = false;
    // Tag colors the user added via the custom color dialog (0xRRGGBB),
    // appended after the seven Finder defaults in the swatch strip.
    std::vector<uint32_t> custom_tag_colors;
    // Tag ids the tag panel lists under 最近用过, newest first (at most 5).
    std::vector<std::wstring> recent_tags;
    // Pulse's folder picker, per entry point (#58-9): one "key|folder" line
    // each, newest first, so the tray, duplicate scan, quick access... each
    // open where they were last used instead of overwriting one another.
    std::vector<std::wstring> picker_folders;
    // Settings > 快捷键: one "Id=Ctrl+Shift+C|F8" line per changed action
    // (app/keymap.h); defaults are not stored.
    std::vector<std::wstring> shortcuts;
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
    // The folder last picked from `key`; empty when none is remembered.
    std::wstring PickerFolder(std::wstring_view key) const;
    // Remembers `folder` for `key`; true when picker_folders changed.
    bool RememberPickerFolder(std::wstring_view key, const std::wstring& folder);
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
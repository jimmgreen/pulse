// Display-only extension store contract. App owns fetching, authorization and installation.
#pragma once
#include <cstdint>
#include "store_icons.h"
#include <string>
#include <vector>

namespace pulse::ui {
struct ExtensionStoreRelease {
    std::wstring version, released, notes, min_pulse;
};
struct ExtensionStoreItem {
    std::wstring name, summary, publisher, version, status, button_label;
    bool action_enabled = false;
    bool official = false; // listed under "Pulse official" instead of the community section
    StoreIcon icon = StoreIcon::None;
    std::wstring size; // size: "1.0 MB"
    uint32_t icon_color = 0x527D70; // 0xRRGGBB tile color
    // Installed, enabled package of this extension (runtime index); -1 = none.
    // Its card leads the section and offers "设置" (the package's settings page).
    int settings_package = -1;
    bool enabled = false; // installed and enabled (badge on the feature card)
    std::wstring category;   // catalog category ("archive", "preview", ...), for the chips
    std::string id;          // catalog id ("pulse.7zip")
    std::wstring license, source, released; // the newest release, for 关于
    std::vector<ExtensionStoreRelease> releases; // verified catalog metadata, no synthetic release history
    std::vector<std::wstring> history;      // "26.00 · 2026-10-21", newest first
    uint32_t custom_icon = 0;               // resolved custom package icon id (>= kCustomExtensionIconMin)
    std::wstring description, engine, homepage;
    std::vector<std::wstring> tags, highlights, permissions;
    bool installed = false;
    int installed_index = -1;
    bool has_menu_tab = false;
    std::vector<std::wstring> plugin_tabs; // the package's own extra settings tabs (支持格式, ...)
};
// One row of a plugin's context-menu preview (manifest quickSettings menu preview).
struct SevenZipMenuRow {
    std::wstring title, detail;
    uint32_t icon = 0; // PULSE_COMMAND_ICON_*
    StoreIcon vector_icon = StoreIcon::File; // its drawn icon (StoreCommandIconFor)
};
struct ExtensionQuickControlView {
    int kind = 0; // 0 = switch, 1 = select, 2 = button
    int action = -1;
    std::wstring label;
    bool checked = false;
    bool disabled = false;
    uint32_t width_dip = 0;
    std::vector<std::wstring> option_labels;
    int selected_option = 0;
};
struct ExtensionQuickRowView {
    std::wstring title;
    std::wstring description;
    std::vector<ExtensionQuickControlView> controls;
};
struct ExtensionQuickCardView {
    int item_index = -1;
    int package_index = -1;
    std::string package_id;
    std::wstring title;
    std::wstring status;
    bool available = false;
    std::vector<ExtensionQuickRowView> rows;
    bool has_menu_preview = false;
    bool preview_submenu = false;
    std::wstring preview_caption;
    std::vector<SevenZipMenuRow> preview_rows;
};
// Registry snapshot, including disabled packages; never derived from the running runtime.
struct InstalledExtensionItem {
    std::wstring name, id, version, publisher, status, notice;
    std::string revision; // manifest SHA-256 of the active version
    bool enabled = false;
    bool native_code = false;
    bool can_enable = false, can_disable = false, can_uninstall = false, can_rollback = false;
    // >= 0: a developer folder row (devmode registry index), not a store registry entry.
    // Its 更多 menu offers 重新加载 / 移除 / 打开文件夹; it has no enable switch.
    int development_index = -1;
};
struct ExtensionStoreView {
    enum class State { Idle, Loading, Failed, Offline, Online };
    enum class NoticeKind { Information, Success, Warning, Error };
    State state = State::Idle;
    std::wstring notice;
    NoticeKind notice_kind = NoticeKind::Information;
    bool install_completed = false;
    bool pending_restart = false;
    bool developer_mode = false; // the saved setting (takes effect after a restart)
    bool busy = false; // Shows StoreCancel; app decides whether cancellation is available.
    std::vector<ExtensionStoreItem> items;
    std::vector<InstalledExtensionItem> installed;
    bool installed_loading = false;
    bool installed_busy = false; // Local mutation/review only; independent of catalog network loading.
    std::wstring installed_error;
    // Browse filters and the extension detail page.
    int category = 0;          // chip: 0 all, 1 archives, 2 preview, 3 search, 4 AI
    std::wstring search;       // the search box's text
    bool detail = false;       // an extension detail page replaces browse
    int detail_item = -1;      // index in items
    int detail_tab = 0;        // 0 overview, 1 about
    bool saved = false;        // "已保存" flashes after an edit
    std::vector<ExtensionQuickCardView> quick_cards;
};
} // namespace pulse::ui
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
};
// The official 7-Zip extension as the store shows it: read from, and edited through,
// its own settings page states (the plugin saves and re-registers its commands).
struct SevenZipMenuRow {
    std::wstring title, detail;
    uint32_t icon = 0; // PULSE_COMMAND_ICON_*
    StoreIcon vector_icon = StoreIcon::File; // its drawn icon (StoreCommandIconFor)
};
struct SevenZipView {
    bool available = false; // the package runs and its page answers
    bool needs_restart = false; // installed/toggled on disk, awaiting full Pulse restart
    bool needs_update = false;  // running DLL predates settings page support
    bool menu_available = false; // older packages do not publish menu settings
    bool formats_available = false;
    bool TabAvailable(int tab) const {
        return available && (tab != 1 || menu_available) && (tab != 2 || formats_available);
    }
    bool installed = false; // the catalog item is installed (may be loading/off)
    int item = -1;          // index in ExtensionStoreView::items
    int installed_index = -1; // index in ExtensionStoreView::installed
    int mode = 0, location = 0, format = 0, level = 3;
    bool remember = true, submenu = false;
    uint32_t commands = 0x3f; // bit per command, plugin order
    std::wstring folder;
    std::wstring status;      // the page's own save line ("已保存。", errors)
    std::vector<std::wstring> formats_rw, formats_ro;
    std::vector<SevenZipMenuRow> archive_rows, file_rows; // the live menu for sample selections
};
// Registry snapshot, including disabled packages; never derived from the running runtime.
struct InstalledExtensionItem {
    std::wstring name, id, version, publisher, status, notice;
    std::string revision; // manifest SHA-256 of the active version
    bool enabled = false;
    bool native_code = false;
    bool can_enable = false, can_disable = false, can_uninstall = false, can_rollback = false;
};
struct ExtensionStoreView {
    enum class State { Idle, Loading, Failed, Offline, Online };
    State state = State::Idle;
    std::wstring notice;
    bool pending_restart = false;
    bool busy = false; // Shows StoreCancel; app decides whether cancellation is available.
    std::vector<ExtensionStoreItem> items;
    std::vector<InstalledExtensionItem> installed;
    bool installed_loading = false;
    bool installed_busy = false; // Local mutation/review only; independent of catalog network loading.
    std::wstring installed_error;
    // Browse filters and the 7-Zip views (feature card, detail page).
    int category = 0;          // chip: 0 all, 1 archives, 2 preview, 3 search, 4 AI
    std::wstring search;       // the search box's text
    bool detail = false;       // the 7-Zip detail page replaces browse
    int detail_tab = 0;        // 常用设置 / 右键菜单 / 支持格式 / 关于
    int preview_kind = 0;      // menu preview: 0 archive, 1 files
    bool saved = false;        // "已保存" flashes after an edit
    float level_drag = -1.0f;
    SevenZipView seven_zip;
};
} // namespace pulse::ui

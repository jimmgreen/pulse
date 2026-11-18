// context_menu_prefs.h — Explorer fusion-zone defaults + user overrides.
//
// Factory denylist lives in the category switches; per-item overrides and the
// seen catalog (grown from real right-clicks) persist to context_menu.json
// next to places.json. Host still returns the full COM list; the UI process
// applies these prefs so turning 「发送到」 back on actually works.
#pragma once
#include "../ipc/ctx_menu_util.h"
#include "builtin_menu_items.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace pulse::app {

struct SeenMenuItem {
    std::wstring key;
    std::wstring text;
    bool flyout = false;
    bool from_com = false;
    ipc::CtxMenuCategory category = ipc::CtxMenuCategory::Software;
};

struct SlowComExt {
    uint32_t last_ms = 0;
    uint32_t slow_hits = 0;   // times >= 500ms
    uint32_t timeout_hits = 0; // times >= 1000ms
    bool deferred = false;     // still query async, never block first frame
    bool disabled = false;     // skip COM query entirely
};

struct ContextMenuPrefs {
    bool persist = true;
    bool load_failed = false;
    // FromJson rewrote state an older build persisted (see kPrefsVersion);
    // Load saves the cleaned file once.
    bool migrated = false;

    // Every group Explorer's classic menu shows is on by default, so Pulse's
    // menu is never shorter than Explorer's; the settings page and the menu
    // presets turn groups off; explicit per-item disables remain authoritative.
    bool software = true;
    bool share = true;
    bool wallpaper = true;
    bool rotate = true;
    bool shortcut = true;
    bool open_with = true;
    bool open_with_com = true;
    bool system_extra = true;
    bool print = true;

    int open_with_mru = -1; // -1 keeps every available recommendation.

    std::unordered_map<std::wstring, bool> item_enabled;
    std::vector<SeenMenuItem> seen;
    std::unordered_map<std::wstring, SlowComExt> slow_ext;
    // Pulse's own menu rows / row buttons the user turned off (BuiltinMenuBit).
    uint64_t builtin_hidden = 0;
    // Their order per BuiltinMenuSurface; empty = table order.
    BuiltinMenuOrder builtin_order[static_cast<size_t>(BuiltinMenuSurface::Count)];

    void ResetToDefaults();
    // Re-keys the seen catalog through CatalogKey and carries the per-item
    // overrides onto the new keys; run whenever the catalog is loaded.
    void MigrateSeenKeys();
    bool CategoryEnabled(ipc::CtxMenuCategory c) const;
    bool GroupEnabled(ipc::CtxMenuGroup g) const;
    void SetGroupEnabled(ipc::CtxMenuGroup g, bool on);
    bool ItemEnabled(const std::wstring& key, ipc::CtxMenuCategory c, bool from_com) const;
    // Settings-page view of a row (#77): a handler the slow-extension
    // auto-disable turned off must not display as on just because no explicit
    // override exists. Toggling such a row on also clears the disable.
    bool RowEnabled(const std::wstring& key, ipc::CtxMenuCategory c, bool from_com) const;
    void SetItemEnabled(const std::wstring& key, bool on);
    bool HandlerEnabled(const std::wstring& clsid) const;
    // Settings > 系统右键菜单 (只在 Pulse 隐藏): whether Pulse shows the entry with
    // catalog key `key`. Seen rows follow RowEnabled; unseen ones show unless
    // switched off. HiddenCatalogKeys: every key that is off, sorted.
    bool CatalogItemVisible(const std::wstring& key) const;
    std::vector<std::wstring> HiddenCatalogKeys() const;
    std::vector<std::wstring> DisabledHandlerClsids() const;
    bool RecordSeen(const std::wstring& key, const std::wstring& text, bool flyout,
                    ipc::CtxMenuCategory category, bool from_com = false);
    // key: "h:{CLSID}" of a handler the host reported hung. Returns true if
    // prefs changed.
    bool RecordComTiming(const std::wstring& key, uint32_t elapsed_ms);
    bool ComDeferred(const std::wstring& key) const;
    bool ComDisabled(const std::wstring& key) const;
    void SetComDisabled(const std::wstring& key, bool on);
    bool BuiltinVisible(BuiltinMenuItem item) const {
        return (builtin_hidden & BuiltinMenuBit(item)) == 0;
    }
    void SetBuiltinVisible(BuiltinMenuItem item, bool on);
    // Normalized order of a surface (the table order unless the user moved rows).
    BuiltinMenuOrder BuiltinOrder(BuiltinMenuSurface surface) const;
    bool BuiltinOrderCustom(BuiltinMenuSurface surface) const;
    // Stores `order`; the table order is stored as empty.
    void SetBuiltinOrder(BuiltinMenuSurface surface, const BuiltinMenuOrder& order);
    void ResetBuiltinOrder();

    std::wstring ToJson() const;
    bool FromJson(const std::wstring& json);
    bool Load();
    bool Save() const;

private:
    void CoalesceCompressCatalog();
};

} // namespace pulse::app

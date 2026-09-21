// context_menu_prefs.h — Explorer fusion-zone defaults + user overrides.
//
// Factory denylist lives in the category switches; per-item overrides and the
// seen catalog (grown from real right-clicks) persist to context_menu.json
// next to places.json. Host still returns the full COM list; the UI process
// applies these prefs so turning 「发送到」 back on actually works.
#pragma once
#include "../ipc/ctx_menu_util.h"
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

    // Lets ContextMenuPrefs::Save() tell a local edit from another window's write.
    bool operator==(const SeenMenuItem& other) const {
        return key == other.key && text == other.text && flyout == other.flyout &&
               from_com == other.from_com && category == other.category;
    }
};

struct SlowComExt {
    uint32_t last_ms = 0;
    uint32_t slow_hits = 0;   // times >= 500ms
    uint32_t timeout_hits = 0; // times >= 1000ms
    bool deferred = false;     // still query async, never block first frame
    bool disabled = false;     // skip COM query entirely

    bool operator==(const SlowComExt& other) const {
        return last_ms == other.last_ms && slow_hits == other.slow_hits &&
               timeout_hits == other.timeout_hits && deferred == other.deferred &&
               disabled == other.disabled;
    }
};

// Everything context_menu.json stores. Split out so the object can also keep the
// state that last agreed with the file and compare the two field by field.
struct ContextMenuPrefsValues {
    // Explorer shows 软件功能 / 打开方式 / 打印 and leaves 发送到 plus the image
    // and system verbs out unless the type registers them; those two groups stay
    // off by default and the settings page turns them on. Only the COM-sourced
    // 打开方式 rows stay off — our own 打开方式… already covers them.
    bool software = true;
    bool share = false;
    bool wallpaper = false;
    bool rotate = false;
    bool shortcut = false;
    bool open_with = true;
    bool open_with_com = false;
    bool system_extra = false;
    bool print = true;

    int explorer_cap = ipc::kDefaultExplorerCap;
    int open_with_mru = 2;

    std::unordered_map<std::wstring, bool> item_enabled;
    std::vector<SeenMenuItem> seen;
    std::unordered_map<std::wstring, SlowComExt> slow_ext;
};

struct ContextMenuPrefs : ContextMenuPrefsValues {
    bool persist = true;

    void ResetToDefaults();
    // Re-keys the seen catalog through CatalogKey and carries the per-item
    // overrides onto the new keys; run whenever the catalog is loaded.
    void MigrateSeenKeys();
    bool CategoryEnabled(ipc::CtxMenuCategory c) const;
    bool GroupEnabled(ipc::CtxMenuGroup g) const;
    void SetGroupEnabled(ipc::CtxMenuGroup g, bool on);
    bool ItemEnabled(const std::wstring& key, ipc::CtxMenuCategory c, bool from_com) const;
    void SetItemEnabled(const std::wstring& key, bool on);
    bool HandlerEnabled(const std::wstring& clsid) const;
    std::vector<std::wstring> DisabledHandlerClsids() const;
    bool RecordSeen(const std::wstring& key, const std::wstring& text, bool flyout,
                    ipc::CtxMenuCategory category, bool from_com = false);
    // key: ".dwg" / ":folder" / ":drive" / ":bg" / ":file". Returns true if prefs changed.
    bool RecordComTiming(const std::wstring& key, uint32_t elapsed_ms);
    bool ComDeferred(const std::wstring& key) const;
    bool ComDisabled(const std::wstring& key) const;
    void SetComDisabled(const std::wstring& key, bool on);

    std::wstring ToJson() const;
    bool FromJson(const std::wstring& json);
    bool Load();
    bool Save() const;

    // True when the last Load() found a usable state in context_menu.json or its
    // backup.
    bool loaded_from_file() const noexcept { return loaded_from_file_; }

private:
    void CoalesceCompressCatalog();

    // The values the file held at the last Load() or Save(). Save() merges against
    // them, so a second Pulse window cannot roll back what another one wrote.
    // mutable: Save() is const, like AppPrefs::Save().
    mutable ContextMenuPrefsValues disk_state_;
    bool loaded_from_file_ = false;

    // Reads context_menu.json, falling back to context_menu.json.bak. |main_exists|
    // separates "no file" from "a file nothing can read"; |used_backup| (optional)
    // reports that the main file was unusable.
    bool ReadDiskState(ContextMenuPrefsValues& values, bool& main_exists,
                       bool* used_backup = nullptr) const;
    // Fields changed here since disk_state_ keep the local value; the rest take what
    // the file holds now. A field missing from MergedWithDisk() keeps the local
    // value, so forgetting one is never a lost setting.
    ContextMenuPrefsValues MergedWithDisk(const ContextMenuPrefsValues& disk) const;
};

} // namespace pulse::app

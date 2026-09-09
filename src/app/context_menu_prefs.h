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

    void ResetToDefaults();
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

private:
    void CoalesceCompressCatalog();
};

} // namespace pulse::app

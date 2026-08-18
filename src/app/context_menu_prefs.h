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

    int explorer_cap = 12;
    int open_with_mru = 2;

    std::unordered_map<std::wstring, bool> item_enabled;
    std::vector<SeenMenuItem> seen;

    void ResetToDefaults();
    bool CategoryEnabled(ipc::CtxMenuCategory c) const;
    void SetCategoryEnabled(ipc::CtxMenuCategory c, bool on);
    bool GroupEnabled(ipc::CtxMenuGroup g) const;
    void SetGroupEnabled(ipc::CtxMenuGroup g, bool on);
    bool ItemEnabled(const std::wstring& key, ipc::CtxMenuCategory c, bool from_com) const;
    void SetItemEnabled(const std::wstring& key, bool on);
    bool RecordSeen(const std::wstring& key, const std::wstring& text, bool flyout,
                    ipc::CtxMenuCategory category, bool from_com = false);

    std::wstring ToJson() const;
    bool FromJson(const std::wstring& json);
    bool Load();
    bool Save() const;
};

} // namespace pulse::app

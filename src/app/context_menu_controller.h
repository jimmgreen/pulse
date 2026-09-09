#pragma once

#include "context_menu.h"
#include "context_menu_prefs.h"
#include "shell_verbs.h"
#include "../ops/ops_manager.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pulse::app {

class ContextMenuController {
public:
    struct ShellOperations {
        std::function<uint32_t(std::vector<std::wstring>, HWND, bool, bool,
                               std::vector<std::wstring>)> query;
        std::function<void(uint32_t)> close;
        std::function<void(uint32_t, uint32_t, std::wstring, std::wstring)> invoke;
        std::function<void(const std::wstring&, const std::wstring&)> execute_verb;
        std::function<void(const std::wstring&, const std::wstring&)> open_with_app;
        std::function<void(const std::wstring&, const std::wstring&)> execute_command;
    };
    struct QueryCompletion {
        bool accepted = false;
        bool partial = false;
        uint32_t elapsed_ms = 0;
        std::wstring cache_key;
    };

    static std::wstring CacheKey(bool background, const std::wstring& extension);

    void SetShellOperations(ShellOperations operations) { operations_ = std::move(operations); }
    void StartQuery(ContextMenuPrefs& prefs, HWND hwnd,
                    std::vector<std::wstring> paths, bool background,
                    std::wstring extension, bool extended,
                    const std::function<std::wstring(const std::wstring&)>& normalize_path,
                    const std::function<void(const std::wstring&)>& prefetch_static);
    void Close();
    void WaitUntilReady(HWND hwnd, const std::function<void()>& pump_messages,
                        uint32_t timeout_ms = 15000);
    std::vector<ui::FluentMenuItem> BuildDisplay(ContextMenuPrefs& prefs,
        const std::vector<ui::FluentMenuItem>& base_items, bool& prefs_changed) const;
    bool ExecuteShellCommand(int command,
        const std::function<void()>& wait_until_ready,
        const std::function<void()>& refresh_folder);

    bool RequestStaticPrefetch(const std::wstring& extension);
    void MergeStaticCache(
        std::unordered_map<std::wstring, std::vector<StaticVerb>> entries);
    bool CompleteStaticVerbs(const std::wstring& extension,
                             std::vector<StaticVerb> verbs);
    void InvalidateCaches();

    bool SeedComItemsFromCache();
    QueryCompletion CompleteComQuery(uint32_t token,
                                     std::vector<ops::ShellMenuItem> items,
                                     uint64_t completed_at, bool partial = false);

    void OpenMenu(std::vector<ui::FluentMenuItem> base_items);
    void NotePatchedDisplay() { menu_com_items_ = com_items_; }
    void CloseMenu() noexcept { menu_open_ = false; }
    bool menu_open() const noexcept { return menu_open_; }
    const std::vector<ui::FluentMenuItem>& base_items() const noexcept {
        return base_items_;
    }

    void ScheduleFolderRefresh(uint64_t now) noexcept;
    int ConsumeDueRefreshes(uint64_t now) noexcept;

private:
    std::vector<ShellMenuEntry> ComposeEntries(ContextMenuPrefs& prefs,
                                               bool& prefs_changed) const;
    const ops::ShellMenuItem* FindComItem(uint32_t id) const;
    const ops::ShellMenuItem* FindComItemIn(const std::vector<ops::ShellMenuItem>& items,
                                            uint32_t id) const;
    uint32_t FindLiveComId(const std::wstring& text) const;
    uint32_t FindLiveComId(const std::wstring& verb, const std::wstring& text) const;

    uint32_t token_ = 0;
    std::vector<std::wstring> paths_;
    bool background_ = false;
    bool menu_open_ = false;
    bool com_ready_ = false;
    std::vector<ops::ShellMenuItem> com_items_;
    std::vector<ops::ShellMenuItem> menu_com_items_;
    std::vector<StaticVerb> static_verbs_;
    std::wstring extension_;
    std::vector<ui::FluentMenuItem> base_items_;
    std::unordered_map<std::wstring, std::vector<StaticVerb>> static_cache_;
    std::unordered_map<std::wstring, std::vector<ops::ShellMenuItem>> com_cache_;
    std::unordered_set<std::wstring> static_pending_;
    uint64_t query_started_at_ = 0;
    uint64_t refresh_at_ = 0;
    uint64_t refresh_again_at_ = 0;
    ShellOperations operations_;
};

} // namespace pulse::app

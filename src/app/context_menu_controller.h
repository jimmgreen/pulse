#pragma once

#include "context_menu.h"
#include "context_menu_prefs.h"
#include "ctx_layout_cache.h"
#include "ctx_menu_timing.h"
#include "shell_verbs.h"
#include "../ops/ops_manager.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pulse::app {

// What one reconciliation did to an open menu (ReconcileCtxDisplay).
struct CtxReconcileStats {
    uint32_t late = 0;      // rows nobody predicted, appended below the shown ones
    uint32_t greyed = 0;    // shown rows the answer no longer has, greyed in place
    uint32_t withheld = 0;  // late rows held back for one growth step (append_late off)
};

// Maps a freshly built display onto the rows on screen, so no row of an open
// menu ever moves (右键菜单优化方案 §4.1 L3). Rows match by label (first
// unused, case-insensitive): a match takes the fresh command, state and
// flyout; a shown row without one stays where it is, greyed. Fresh rows nobody
// showed are appended, flat and with their own flyouts, as a last section
// below everything shown: the menu only grows downwards, and the stored
// layout puts them in place next time. No 「更多」 flyout: hiding rows is the
// user's call (管理右键项). The shown rows keep their separators, except that
// the last one gains one before appended rows. With `append_late` off the
// late rows are only counted (withheld) and the shown rows just patched.
std::vector<ui::FluentMenuItem> ReconcileCtxDisplay(
    const std::vector<ui::FluentMenuItem>& shown, const std::vector<ui::FluentMenuItem>& fresh,
    CtxReconcileStats* stats = nullptr, bool append_late = true);

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
        std::function<void(const std::vector<std::wstring>&, const std::wstring&, bool)> execute_shell_verb;
    };
    struct QueryCompletion {
        bool accepted = false;
        bool partial = false;
        uint32_t elapsed_ms = 0;
        std::wstring cache_key;
    };

    // ".docx", ":folder", ...; several selected items add "|m".
    static std::wstring CacheKey(bool background, const std::wstring& extension,
                                 bool multi = false);

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
    // First frame of a menu: BuildDisplay. Keeps what is on screen; while the
    // menu is open, updates go through ReconcileDisplay so no row moves.
    std::vector<ui::FluentMenuItem> BuildOpenDisplay(ContextMenuPrefs& prefs,
        const std::vector<ui::FluentMenuItem>& base_items, bool& prefs_changed);
    // The current rows mapped onto the open menu (ReconcileCtxDisplay). Pass
    // the result to CommitDisplay once the menu took it. Late rows wait for
    // the complete answer or kLateBatchMs after the first of them, so a cold
    // menu grows in one step rather than once per partial answer.
    static constexpr uint64_t kLateBatchMs = 350;
    std::vector<ui::FluentMenuItem> ReconcileDisplay(ContextMenuPrefs& prefs, bool& prefs_changed,
                                                     uint64_t now);
    void CommitDisplay(std::vector<ui::FluentMenuItem> shown);
    bool TryReplaceBaseFlyout(const std::wstring& text,
        std::vector<ui::FluentMenuItem> children, ContextMenuPrefs& prefs,
        const std::function<bool(std::vector<ui::FluentMenuItem>)>& accept);
    // True while shell rows can still arrive that the first frame could not
    // predict: no stored layout for the key, a handler that never finished
    // under it, or static verbs still being read for the first time. Such
    // rows are appended at the bottom (the timing record's may_grow).
    bool MayGrowAfterOpen() const;
    // Everything this menu waits for has arrived.
    bool AnswersComplete() const;
    bool may_grow_at_open() const noexcept { return may_grow_at_open_; }
    const CtxReconcileStats& reconcile_stats() const noexcept { return reconcile_stats_; }
    bool ExecuteShellCommand(int command,
        const std::function<void()>& wait_until_ready,
        const std::function<void()>& refresh_folder);

    bool RequestStaticPrefetch(const std::wstring& extension);
    // A cached type is read again at most every kStaticRevalidateMs: verbs a
    // ProgID defines itself are outside the registry watch. True = enumerate
    // now and pass the result to CompleteStaticVerbs as usual.
    static constexpr uint64_t kStaticRevalidateMs = 5000;
    bool RequestStaticRevalidate(const std::wstring& extension, uint64_t now);
    void MergeStaticCache(
        std::unordered_map<std::wstring, std::vector<StaticVerb>> entries);
    // `generation` is cache_generation() when the enumeration started; results
    // that predate InvalidateCaches() are dropped. True when the current menu's
    // rows changed (a re-read that found the same verbs changes nothing).
    bool CompleteStaticVerbs(const std::wstring& extension,
                             std::vector<StaticVerb> verbs, uint32_t generation);
    void InvalidateCaches();
    uint32_t cache_generation() const { return cache_generation_; }
    bool HasCachedStaticVerbs(const std::wstring& extension) const {
        return static_cache_.contains(extension);
    }

    // Rows of the last complete answer for this type, painted before the live
    // one arrives; StartQuery seeds them at once, so a partial answer that beats
    // the popup lands on them too. They get ids from kPredictedIdFirst up (never a host id:
    // those come from the previous menu), and partial answers only patch them
    // (see CompleteComQuery), so a menu painted from the cache never shrinks
    // to a partial list and grows back.
    static constexpr uint32_t kPredictedIdFirst = 0x7F00;
    static constexpr uint32_t kPredictedIdLast = 0x7FFF;
    bool SeedComItemsFromCache();
    // Fills com_items_ for a file type with no layout yet from the rows every
    // file shows (an extensionless file's layout, or what all types share).
    bool BorrowComRows(bool multi);

    // Persistent layout (ctx_layout_cache.h). Rows are kept with the item
    // names templated; SeedComItemsFromCache fills in the current names.
    // MergeComLayout adds keys read from disk that this session has not
    // answered yet and marks the layout loaded: only a loaded layout is saved,
    // so a save never drops the keys still being read after a flush.
    size_t MergeComLayout(CtxLayoutMap layout);
    bool com_layout_loaded() const noexcept { return com_layout_loaded_; }
    // True once after a complete answer changed the stored rows.
    bool TakeComLayoutDirty() noexcept {
        const bool dirty = com_layout_dirty_;
        com_layout_dirty_ = false;
        return dirty;
    }
    // The most recently used keys, at most kCtxLayoutMaxKeys.
    CtxLayoutMap ComLayoutSnapshot() const;
    const std::vector<ops::ShellMenuItem>& com_items() const noexcept { return com_items_; }
    bool com_predicted() const noexcept { return com_predicted_; }
    // Also takes the complete answer of a closed menu's session (accepted =
    // false: the caller closes that session; the rows only feed the layout).
    // `handlers` (host order, C4b): with them a partial answer replaces the
    // rows of the handlers that finished and keeps the others' rows, on screen
    // and in the layout; a complete answer keeps a stuck handler's last rows
    // in the layout only (its session is gone, so they could not be invoked).
    QueryCompletion CompleteComQuery(uint32_t token,
                                     std::vector<ops::ShellMenuItem> items,
                                     uint64_t completed_at, bool partial = false,
                                     std::vector<ops::ShellMenuHandlerStatus> handlers = {});
    // Per-handler times and row counts stored for a key (nullptr: none).
    const CtxHandlerStats* ComHandlerStats(const std::wstring& key) const;
    // Every handler the stored layouts measured, each CLSID once with its
    // slowest time (one never seen finishing: the longest wait). Feeds the
    // 概览 of Settings > 系统右键菜单.
    std::vector<std::pair<std::wstring, uint32_t>> ComHandlerTimes() const;
    // The top-level rows each handler (CLSID) gave the stored layouts, names
    // shown as "…" (压缩到 "….zip"): Settings > 系统右键菜单 names a shell
    // extension by what it adds.
    std::map<std::wstring, std::vector<std::wstring>> ComHandlerLabels() const;

    void OpenMenu(std::vector<ui::FluentMenuItem> base_items);
    // A deferred provider may add rows, but may not reposition existing rows.
    void DeferBaseItems(std::function<std::vector<ui::FluentMenuItem>()> resolve);
    bool AppendDeferredBaseItems();
    bool DeferredBaseItemsPending() const noexcept { return deferred_base_items_pending_; }
    void NotePatchedDisplay() { menu_com_items_ = com_items_; menu_static_verbs_ = static_verbs_; }
    void CloseMenu() noexcept {
        menu_open_ = false;
        shown_.clear();
        may_grow_at_open_ = false;
        deferred_base_items_ = {};
        deferred_base_items_pending_ = false;
    }
    bool menu_open() const noexcept { return menu_open_; }
    const std::vector<ui::FluentMenuItem>& base_items() const noexcept {
        return base_items_;
    }

    // Timing record of the menu on screen (ctx_menu_timing.h): begun right
    // before the popup is tracked, taken once it closed.
    void BeginTiming(uint32_t input_to_open_ms, size_t rows, bool seeded, uint64_t now);
    void NoteDisplayUpdate(size_t rows, uint64_t now);
    bool TakeTiming(bool shell_command, CtxMenuTiming& out);

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
    uint32_t ResolveLiveComId(const ops::ShellMenuItem& snapshot,
                              const std::wstring& parent_key) const;

    uint32_t token_ = 0;
    std::vector<std::wstring> paths_;
    bool background_ = false;
    bool extended_ = false;
    std::vector<std::wstring> disabled_handlers_;
    bool menu_open_ = false;
    bool com_ready_ = false;
    bool com_predicted_ = false;  // com_items_ were seeded from com_cache_
    std::vector<ops::ShellMenuItem> com_items_;
    std::vector<ops::ShellMenuItem> menu_com_items_;
    std::vector<StaticVerb> static_verbs_;
    std::vector<StaticVerb> menu_static_verbs_;
    bool display_snapshot_valid_ = false;
    std::wstring extension_;
    std::vector<ui::FluentMenuItem> base_items_;
    std::function<std::vector<ui::FluentMenuItem>()> deferred_base_items_;
    bool deferred_base_items_pending_ = false;
    std::vector<ui::FluentMenuItem> shown_;  // rows on screen (BuildOpenDisplay)
    bool may_grow_at_open_ = false;          // MayGrowAfterOpen at the first frame
    uint64_t late_since_ = 0;                // first withheld late row (0 = none)
    CtxReconcileStats reconcile_stats_;      // committed: late rows so far, greyed now
    CtxReconcileStats pending_stats_;        // ReconcileDisplay result awaiting commit
    std::unordered_map<std::wstring, std::vector<StaticVerb>> static_cache_;
    // Extensions whose rows came from the on-disk cache: they still re-enumerate
    // once, so a cache written by an older build heals itself.
    std::unordered_set<std::wstring> static_seeded_;
    struct ComLayout {
        std::vector<ops::ShellMenuItem> rows;  // labels templated (CtxNames)
        uint64_t used = 0;                     // com_use_clock_ at last seed/store
        CtxHandlerStats handlers;              // menu order
    };
    std::unordered_map<std::wstring, ComLayout> com_cache_;
    uint64_t com_use_clock_ = 0;
    bool com_layout_loaded_ = false;
    bool com_layout_dirty_ = false;
    CtxNames names_;  // names of the items the current query is for
    // A menu closed before its complete answer leaves the host session running
    // until that answer arrives (it is only stored, never shown), so a slow
    // machine still learns the layout. One at a time; the next one, a registry
    // flush or kDrainMs closes it.
    static constexpr uint64_t kDrainMs = 15000;
    struct Draining {
        uint32_t token = 0;
        std::wstring key;
        CtxNames names;
        uint32_t generation = 0;
        uint64_t since = 0;
    };
    Draining draining_;
    void StoreLayout(const std::wstring& key, const std::vector<ops::ShellMenuItem>& items,
                     const CtxNames& names,
                     const std::vector<ops::ShellMenuHandlerStatus>& handlers = {});
    std::unordered_set<std::wstring> static_pending_;
    std::unordered_map<std::wstring, uint64_t> static_read_at_; // last enumeration request
    uint32_t cache_generation_ = 0;  // bumped by InvalidateCaches()
    uint64_t query_started_at_ = 0;
    uint32_t query_cache_generation_ = 0;
    CtxMenuTiming timing_;
    bool timing_active_ = false;
    uint64_t opened_at_ = 0;
    size_t display_rows_ = 0;
    uint64_t refresh_at_ = 0;
    uint64_t refresh_again_at_ = 0;
    ShellOperations operations_;
};

} // namespace pulse::app

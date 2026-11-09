// action_state.h - What the AI sees of a window, what it selects, and what it
// waits for.
//
//   StateSnapshot   get_state: tabs, panes in screen order, selection, tray,
//                   preview, a modal dialog, panels, the running file task
//   Selector        {"pane":2,"extensions":["pdf"],"modified":"today"}: the
//                   rows of one pane that meet every condition; only rows the
//                   user sees (filter box, hidden files) are candidates
//   WaitFor         listing_loaded / op_finished / preview_open, polled on the
//                   UI thread (never blocks); a timeout returns the state so far
//
// All of it runs on the UI thread of the window it reads.
#pragma once
#include "../../common/json_value.h"
#include "../../index/search_kinds.h"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pulse {
struct AppState;
}

namespace pulse::app {
struct Pane;
struct LayoutTab;
struct Tab;
}

namespace pulse::app::actions {

// The focused pane lists up to this many selected paths; other panes names.
inline constexpr size_t kStateFocusedPaths = 200;
inline constexpr size_t kStateOtherNames = 20;
inline constexpr size_t kStateTrayNames = 10;
inline constexpr size_t kSelectionNames = 20;

// get_state. Paths are shown without the \\?\ prefix.
json::Value StateSnapshot(AppState& s);

// The visible panes of a layout tab numbered the way the user names them:
// left to right, then top to bottom (pane 1 is the first).
std::vector<Pane*> PanesOnScreen(const LayoutTab& layout);
// What a tab is called on its tab strip (get_state's tab titles).
std::wstring TabStripTitle(const LayoutTab& layout);
// A "pane" argument: absent or "focused", 1-4 (screen order), "target" or
// "all" (every pane on screen, in screen order). False with a short English
// `error` when it names no pane.
bool PanesForArg(AppState& s, const json::Value* pane, std::vector<Pane*>& out, std::string& error);

// ---- Selector -------------------------------------------------------------

enum class SelectMode { Replace, Add, Remove };

struct Selector {
    int pane = 0;                          // 1-based screen order; 0 focused; -1 target
    std::vector<std::wstring> names;       // folded; with * ? a wildcard, else "contains"
    index::SearchKind kind = index::SearchKind::Any;
    std::vector<std::wstring> extensions;  // folded, no dot
    bool has_modified = false;
    uint64_t modified_from = 0;            // UTC FILETIME ticks, inclusive
    uint64_t modified_to = UINT64_MAX;     // exclusive
    uint64_t larger_than = 0;              // bytes; 0 = no bound
    uint64_t smaller_than = 0;             // bytes; 0 = no bound
    std::wstring tag;                      // a tag's name
    std::vector<std::wstring> items;       // exact names, folded
    bool within_selection = false;
    SelectMode mode = SelectMode::Replace;
};

// `now_utc` (FILETIME ticks) anchors today / this_week / ...; weeks start on
// Monday, days at local midnight. False with a short English `error`.
bool ParseSelector(const json::Value& args, uint64_t now_utc, Selector& out, std::string& error);

struct SelectorMatch {
    Pane* pane = nullptr;
    int pane_number = 0;          // screen order, 1-based
    std::vector<int> matched;     // entry indices meeting every condition
    std::vector<int> result;      // the selection the mode leads to
    size_t hidden_by_filter = 0;  // matches the filter box hides
    uint64_t bytes = 0;           // matched files
    size_t folders = 0;           // the folders the matches are in
    bool results = false;         // the pane shows search results (P4d)
    // Empty, or why nothing matched: no_pane, loading, unknown_tag,
    // tag_in_results, filter_hides, no_match, changed.
    std::string reason;
};

// Only computes; the selection is left alone.
SelectorMatch ResolveSelector(AppState& s, const Selector& selector);
// Selects `match.result` like the user's own click (not an undo step) and
// scrolls the first match into view. Nothing changes when nothing matched.
// Returns the report the AI gets back.
json::Value ApplySelection(AppState& s, const SelectorMatch& match);
// A content search keeps its results in a store off the UI thread, so
// matching them takes a moment. False when the pane shows no such results
// (use ResolveSelector); else `done` runs once on the UI thread with the
// match, or never when the window goes away. Tags are not matched there
// (tag_in_results): the tag list lives on the UI thread.
bool ResolveSelectorAsync(AppState& s, const Selector& selector,
                          std::function<void(AppState&, SelectorMatch)> done);
// The tab is a search page (name or content search).
bool IsSearchTab(const Tab& tab);
// The pane (as Selector::pane) shows a search page.
bool PaneShowsSearch(AppState& s, int pane);

// ---- Waiting ---------------------------------------------------------------

// SearchDone: a search page has every result (a name search loads the
// pages it has not shown yet; P4d).
enum class WaitKind { ListingLoaded, OpFinished, PreviewOpen, SearchDone };

struct WaitSpec {
    WaitKind kind = WaitKind::ListingLoaded;
    int pane = 0;           // listing_loaded, search_done: as Selector::pane
    uint64_t task = 0;      // op_finished: 0 = whatever runs now
    uint32_t timeout_ms = 5000;
};

// {"wait":"listing_loaded","pane":2,"timeout_ms":8000}. Timeouts default to
// 5 s / 10 s / 3 s / 10 s and are capped at 30 s / 60 s / 10 s / 30 s
// (listing_loaded, op_finished, preview_open, search_done).
bool ParseWait(const json::Value& args, WaitSpec& out, std::string& error);
// True when waiting is over (the state is there, or it cannot come: a
// conflict dialog, an unknown task); `out` says which.
bool WaitReady(AppState& s, const WaitSpec& spec, json::Value& out);
// Calls `done` once on the UI thread: at once when ready, else from a 50 ms
// thread timer, with {"ok":false,"timed_out":true,...} at the deadline.
// Dropped without a call when the window goes away or `cancelled` says so.
void WaitFor(AppState& s, const WaitSpec& spec, std::function<bool()> cancelled,
             std::function<void(AppState&, json::Value)> done);

// ---- Shared helpers (also used by the tests) --------------------------------

uint64_t FileTimeTicks(const FILETIME& ft) noexcept;
// UTC ticks of local midnight `days` days after the local date of `utc`.
uint64_t LocalMidnightUtc(uint64_t utc, int days);
// "100MB", "1.5 GB", "500k", "2048" (bytes; 1024 based). False when invalid.
bool ParseByteSize(std::wstring_view text, uint64_t& out);
// Case-insensitive * and ? match of a whole name.
bool WildcardMatch(std::wstring_view name, std::wstring_view pattern);

} // namespace pulse::app::actions

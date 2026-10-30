// external_open.h - Where a folder opened from outside Pulse goes (a second
// launch, Win+E, "show in folder", Explorer takeover): multi-window M7a,
// plan §6. Pure policy, no windows, so every rule is unit-tested
// (pulse_external_open_test); open_routing.cpp applies it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pulse::app {

enum class ExternalOpenPolicy : uint8_t {
    Smart = 0,            // default (plan §6 rule 5)
    AlwaysTab = 1,        // a tab of the most recently used window
    AlwaysWindow = 2,     // a new window
    AlwaysActivate = 3,   // 只呼出: raise the window, open nothing (Win+E without a folder)
};

// Hidden tuning (plan §6: unvalidated, fixed after the P3 beta).
struct ExternalOpenTuning {
    uint64_t recent_input_ms = 8000;  // smart: input this recent means "busy"
    uint64_t batch_ms = 500;          // requests this close together form one batch
};

// One Pulse window, most recently used first.
struct ExternalOpenWindow {
    bool shows_folder = false;  // a visible pane of one of its tabs shows the folder
    bool minimized = false;
    bool hidden = false;        // closed to the tray
    bool busy = false;          // renaming, dragging, a menu or a modal dialog
    uint64_t last_input_tick = 0;
    bool batch_window = false;  // the window the current batch landed in
};

// The requests that arrived together so far (rule 3). Kept by the caller
// between requests; NextOpenBatch updates it.
struct ExternalOpenBatch {
    uint64_t tick = 0;           // last request of the batch (0 = none)
    size_t count = 0;            // requests in it
    bool window_is_new = false;  // its window was opened for the batch
    // Its window is an existing one and the batch's first tab there can still
    // move out (it exists and is not that window's only tab).
    bool tab_movable = false;
};

struct ExternalOpenRequest {
    bool can_dedupe = true;   // false for This PC: every request opens it
    bool new_window = false;  // explicit (reserved: Shift+open, jump list)
    uint64_t tick = 0;
};

enum class ExternalOpenAction : uint8_t {
    Activate,      // `window` already shows the folder: bring that tab forward
    Tab,           // a new tab in `window` (restored first when `restore`)
    NewWindow,     // a new window
    JoinBatch,     // a new tab in the batch's window `window`
    PromoteBatch,  // a new window takes the batch's tab out of `window`, then this one
};

enum class ExternalOpenReason : uint8_t {
    Duplicate,    // rule 1
    Explicit,     // rule 2
    Batch,        // rule 3
    Policy,       // rule 4
    Hidden,       // rule 5: minimized or in the tray, so nothing is interrupted
    Busy,         // rule 5: renaming, dragging, a menu or a dialog
    RecentInput,  // rule 5: used within recent_input_ms
    Idle,         // rule 5: otherwise
};

struct ExternalOpenDecision {
    ExternalOpenAction action = ExternalOpenAction::Tab;
    ExternalOpenReason reason = ExternalOpenReason::Idle;
    size_t window = 0;     // index into the windows passed in
    bool restore = false;  // the target window is minimized or hidden
};

// Plan §6 rules 1-5 in order, the first that applies wins. `windows` is
// never empty in Pulse (the first window lives until exit); an empty list
// yields NewWindow. Two refinements of the plan: a minimized or hidden window
// is checked before recent input (nothing visible can be interrupted), and
// "always tab" keeps a batch in its window instead of opening a new one.
ExternalOpenDecision DecideOpenTarget(const ExternalOpenRequest& request,
                                      const std::vector<ExternalOpenWindow>& windows,
                                      const ExternalOpenBatch& batch, ExternalOpenPolicy policy,
                                      const ExternalOpenTuning& tuning = {});

// The batch after a request was opened as `decision`. `opened_tab` is false
// when nothing new opened (Activate, or an open that failed); `tab_movable`
// whether the tab it opened in an existing window can move out later.
ExternalOpenBatch NextOpenBatch(const ExternalOpenBatch& batch, const ExternalOpenRequest& request,
                                const ExternalOpenDecision& decision, bool opened_tab, bool tab_movable,
                                const ExternalOpenTuning& tuning = {});

// Whether `request` continues `batch` (rule 3's time window).
bool ContinuesOpenBatch(const ExternalOpenBatch& batch, const ExternalOpenRequest& request,
                        const ExternalOpenTuning& tuning = {});

// Plan §6 rule 7 (M7b): the toast after an open offers the other way.
enum class OpenConversion : uint8_t {
    None,      // no toast
    ToTab,     // opened in a new window: "make it a tab" (its tabs go back)
    ToWindow,  // opened in a tab: "move it to a new window"
};

// What the toast offers for `decision`. Only the smart policy guesses, so
// only it offers anything; an already open folder offers nothing.
// `in_new_window`: the folder ended up in a window opened for it (or its
// batch); `tab_movable`: its window has another tab, so the tab can leave.
OpenConversion OfferedConversion(const ExternalOpenDecision& decision, ExternalOpenPolicy policy,
                                 bool in_new_window, bool tab_movable);

// Consecutive conversions the same way (rule 7: after 3, ask once whether
// that should be the default; never change it unasked).
struct OpenConversionStreak {
    OpenConversion direction = OpenConversion::None;
    int count = 0;
};
constexpr int kOpenConversionPromptAfter = 3;

// Records a conversion; true when the prompt is due, which starts the count
// over. The other direction starts a new streak.
bool NoteOpenConversion(OpenConversionStreak& streak, OpenConversion direction,
                        int prompt_after = kOpenConversionPromptAfter);

// The default the prompt proposes after a streak of `direction`.
ExternalOpenPolicy SuggestedOpenPolicy(OpenConversion direction);

}  // namespace pulse::app

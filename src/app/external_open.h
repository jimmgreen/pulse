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
    Smart = 0,         // default (plan §6 rule 5)
    AlwaysTab = 1,     // a tab of the most recently used window
    AlwaysWindow = 2,  // a new window
};

// Legacy source-compatible fields. Timing never influences current decisions.
struct ExternalOpenTuning {
    uint64_t recent_input_ms = 8000;  // ignored; actual busy state is explicit
    uint64_t batch_ms = 500;          // ignored; distinct requests are not grouped by time
};

// One Pulse window, most recently used first.
struct ExternalOpenWindow {
    bool shows_folder = false;  // a visible pane of one of its tabs shows the folder
    bool minimized = false;
    bool hidden = false;        // closed to the tray
    bool busy = false;          // renaming, dragging, a menu or a modal dialog
    uint64_t last_input_tick = 0; // compatibility only; recent input is not a busy signal.
    bool batch_window = false;  // legacy; ignored by current selection
};

// Legacy batch state retained for source compatibility. NextOpenBatch clears it;
// no time-based grouping or automatic promotion is performed.
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
    bool new_window = false;  // explicit new-window intent; wins even when the folder is already shown
    uint64_t tick = 0;
};

enum class ExternalOpenAction : uint8_t {
    Activate,      // `window` already shows the folder: bring that tab forward
    Tab,           // a new tab in `window` (restored first when `restore`)
    NewWindow,     // a new window
    JoinBatch,     // legacy value; no longer produced by target selection
    PromoteBatch,  // legacy value; no longer produced by target selection
    Deferred,      // no usable target; caller keeps the request pending, no UI mutation
};

enum class ExternalOpenReason : uint8_t {
    Duplicate,    // rule 1
    Explicit,     // rule 2
    Batch,        // legacy reason; no longer produced
    Policy,       // rule 4
    Hidden,       // rule 5: minimized or in the tray, so nothing is interrupted
    Busy,         // rule 5: renaming, dragging, a menu or a dialog
    RecentInput,  // legacy reason; no longer produced
    Idle,         // rule 5: otherwise
};

struct ExternalOpenDecision {
    ExternalOpenAction action = ExternalOpenAction::Tab;
    ExternalOpenReason reason = ExternalOpenReason::Idle;
    size_t window = 0;     // target index only for Activate/Tab/legacy batch actions
    bool restore = false;  // the target window is minimized or hidden
};

// Explicit new window precedes reuse. Otherwise prefer a nonbusy duplicate,
// then the fixed new-window policy or a nonbusy MRU. All-busy tab/smart targets
// yield Deferred without restoring or activating anything. Empty means first
// launch and yields NewWindow. Request IDs/deduplication remain caller-owned;
// separate explicit new-window requests must not be merged by folder path.
ExternalOpenDecision DecideOpenTarget(const ExternalOpenRequest& request,
                                      const std::vector<ExternalOpenWindow>& windows,
                                      const ExternalOpenBatch& batch, ExternalOpenPolicy policy,
                                      const ExternalOpenTuning& tuning = {});

// Dedicated launch entries bypass folder deduplication and timing. Mode 0 opens
// a window, mode 1 tabs, mode 2 only activates; modes 1/2 skip busy candidates.
// All busy => Deferred. Explicit-window and first-window creation remain valid.
ExternalOpenDecision DecideLaunchTarget(int mode, const std::vector<ExternalOpenWindow>& windows,
                                      bool explicit_window = false);

// Taskbar activation restores only the MRU window. A busy MRU defers without
// navigating, choosing another window or manufacturing a new one.
ExternalOpenDecision DecideActivationTarget(const std::vector<ExternalOpenWindow>& windows);

// Compatibility helpers: always return an empty batch / false respectively.
// Arguments remain to avoid changing unrelated routing/conversion callers.
ExternalOpenBatch NextOpenBatch(const ExternalOpenBatch& batch, const ExternalOpenRequest& request,
                                const ExternalOpenDecision& decision, bool opened_tab, bool tab_movable,
                                const ExternalOpenTuning& tuning = {});

// Timing no longer creates a logical batch.
bool ContinuesOpenBatch(const ExternalOpenBatch& batch, const ExternalOpenRequest& request,
                        const ExternalOpenTuning& tuning = {});

// Plan §6 rule 7 (M7b): the toast after an open offers the other way.
enum class OpenConversion : uint8_t {
    None,      // no toast
    ToTab,     // opened in a new window: "make it a tab" (its tabs go back)
    ToWindow,  // opened in a tab: "move it to a new window"
};

// Manual conversion remains available for completed Smart opens. Activation
// and Deferred perform no open and offer nothing.
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

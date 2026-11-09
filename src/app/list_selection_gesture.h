#pragma once
#include "marquee_selection.h"
#include <windows.h>
#include <chrono>
#include <string>
#include <memory>

namespace pulse::app {
class ListSelectionGesture {
public:
    enum class Phase { Idle, BandPending, BandActive, FilePending };

    bool PendingBand() const { return phase_ == Phase::BandPending; }
    bool BandActive() const { return phase_ == Phase::BandActive; }
    bool PendingFile() const { return phase_ == Phase::FilePending; }
    void ArmBand() { phase_ = Phase::BandPending; }
    void ActivateBand() { if (PendingBand()) phase_ = Phase::BandActive; }
    void ArmFile() { phase_ = Phase::FilePending; }
    void EndFile() {
        if (!PendingFile()) return;
        phase_ = Phase::Idle;
        file_snapshot.reset();
        ClearDeferred();
    }

    void DeferReplace(int index) { deferred_index_ = index; toggle_on_release_ = false; }
    void DeferToggle(int index) { deferred_index_ = index; toggle_on_release_ = true; }
    bool RebaseFile(const MarqueeContext& next, const std::wstring& path,
                    int pressed_index, std::vector<std::wstring> selected_keys) {
        if (!PendingFile() || context.pane != next.pane || context.tab != next.tab ||
            context.generation != next.generation || file_path != path || pressed_index < 0)
            return false;
        std::sort(selected_keys.begin(), selected_keys.end());
        if (selected_keys != file_selection) return false;
        if (deferred_index_ >= 0) deferred_index_ = pressed_index;
        context = next;
        file_index = pressed_index;
        ++file_rebases;
        return true;
    }
    void ClearDeferred() { deferred_index_ = -1; toggle_on_release_ = false; }
    int DeferredIndex() const { return deferred_index_; }
    bool ToggleOnRelease() const { return toggle_on_release_; }

    void ResetBand() {
        if (PendingBand() || BandActive()) phase_ = Phase::Idle;
        additive = toggle = row_ctrl = row_shift = before_all = revision_tracked = auto_scroll = dirty = false;
        row_index = before_index = before_anchor = -1;
        ClearMarqueeScratch(before);
        ClearMarqueeScratch(base);
        ClearMarqueeScratch(hits);
        delta.Reset();
        start = current = {};
        origin_x = origin_y = 0;
        context = {};
        revision = 0;
        scroll_last = {};
    }
    void Cancel() {
        ResetBand();
        phase_ = Phase::Idle;
        file_start = {};
        file_snapshot.reset();
        file_index = -1;
        file_path.clear();
        file_key.clear();
        file_selection.clear();
        file_rebases = 0;
        ClearDeferred();
    }

    bool additive = false, toggle = false, row_ctrl = false, row_shift = false;
    bool before_all = false, revision_tracked = false, auto_scroll = false, dirty = false;
    int row_index = -1, before_index = -1, before_anchor = -1;
    std::unordered_set<int> before, base;
    POINT start{}, current{}, file_start{};
    float origin_x = 0, origin_y = 0;
    MarqueeSelection delta;
    std::vector<int> hits;
    MarqueeContext context;
    uint64_t revision = 0;
    std::shared_ptr<const void> file_snapshot;
    int file_index = -1;
    std::wstring file_path, file_key;
    std::vector<std::wstring> file_selection;
    uint64_t file_rebases = 0;
    std::chrono::steady_clock::time_point scroll_last{};

private:
    Phase phase_ = Phase::Idle;
    int deferred_index_ = -1;
    bool toggle_on_release_ = false;
};
} // namespace pulse::app

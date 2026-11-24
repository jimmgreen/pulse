#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>

namespace pulse::app {

// Deterministic drag-shake recognizer (matches staging_shelf_mockup.html):
// counts horizontal direction reversals whose stroke amplitude exceeds the
// sensitivity threshold within a rolling time window, with a post-fire cooldown.
class DragShakeDetector {
public:
    void SetSensitivity(int level) noexcept;  // 0 = Low, 1 = Medium (default), 2 = High
    int Sensitivity() const noexcept { return sensitivity_; }

    // Feeds one pointer X sample (pixels) at `dpi_scale` and `now_ms`.
    // Returns true on the sample that completes a shake gesture.
    bool Feed(int x_px, float dpi_scale, uint64_t now_ms) noexcept;

    void Reset() noexcept;

private:
    int sensitivity_ = 1;
    bool has_anchor_ = false;
    int dir_ = 0;            // -1 left, +1 right, 0 undecided
    int anchor_ = 0;
    int extreme_ = 0;
    uint64_t reversals_[6]{};
    int rev_count_ = 0;
    uint64_t cooldown_until_ = 0;
};

// Global flags updated by Pulse's own drag sources so the background listener
// knows when Pulse is dragging files vs dragging out of the shelf itself.
void SetInternalFileDragActive(bool active) noexcept;
bool IsInternalFileDragActive() noexcept;
void SetShelfOwnDragActive(bool active) noexcept;
bool IsShelfOwnDragActive() noexcept;

// True when an OLE drag-and-drop operation is currently underway on the
// desktop (and not a window move/resize, menu, or shelf-own drag).
bool IsSystemOleDragActive() noexcept;

// Observer on Pulse's existing Raw Input listener thread. Windows supports
// only one Raw Input registration per device class in a process.
class DragShakeWatcher {
public:
    void Update(HWND target, UINT message, bool shake_enabled, int sensitivity);
    POINT LastTriggerPoint() const noexcept;
    void OnInput(const RAWINPUT& raw);
    void PollPointer();

private:
    void Notify(POINT point);
    std::atomic<HWND> target_{nullptr};
    std::atomic<UINT> message_{0};
    std::atomic<bool> shake_enabled_{true};
    std::atomic<int> sensitivity_{1};
    std::atomic<int64_t> last_pt_{0};

    bool shortcut_down_ = false;
    bool pointer_held_ = false;
    uint64_t last_sample_at_ = 0;
    DragShakeDetector detector_;    // listener thread only
};

} // namespace pulse::app

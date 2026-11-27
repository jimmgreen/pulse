#pragma once
#include "../common/drag_shake_detector.h"
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <array>
#include <thread>

namespace pulse::app {

// Global flags updated by Pulse's own drag sources so the background listener
// knows when Pulse is dragging files vs dragging out of the shelf itself.
void SetInternalFileDragActive(bool active) noexcept;
bool IsInternalFileDragActive() noexcept;
void SetShelfOwnDragActive(bool active) noexcept;
bool IsShelfOwnDragActive() noexcept;

// Best-effort OLE drag detection: Pulse's own flag is authoritative; external
// drags use capture-window/drag-image heuristics, not a system-wide OLE API.
bool IsSystemOleDragActive() noexcept;

// Recognizes an S down edge with Ctrl+Shift already held, never a later modifier
// press while S is held. Left/right modifiers are tracked independently.
class DragShortcutDetector {
public:
    void Reset() noexcept { down_.fill(false); }
    void Seed(UINT key, bool down) noexcept { if (key < down_.size()) down_[key] = down; }
    bool Key(UINT key, bool down) noexcept;
private:
    std::array<bool, 256> down_{};
};

// Independent mouse-only Raw Input sink. A non-consuming keyboard hook exists
// only while a pointer button is held, so brief S presses retain their order
// without a resident keyboard Raw Input subscription or polling keyboard edges.
class DragShakeWatcher {
public:
    DragShakeWatcher() = default;
    ~DragShakeWatcher() { Stop(); }
    DragShakeWatcher(const DragShakeWatcher&) = delete;
    DragShakeWatcher& operator=(const DragShakeWatcher&) = delete;
    void Update(HWND target, UINT message, bool shake_enabled, int sensitivity);
    void Stop();
    bool Running() const noexcept { return running_.load(); }
    POINT LastTriggerPoint() const noexcept;

private:
    void Run();
    void PollPointer();
    void WatchShortcut(bool enabled);
    void OnMouse(const RAWMOUSE& mouse);
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK KeyboardProc(int code, WPARAM wparam, LPARAM lparam);
    void Notify(POINT point);
    std::thread thread_;
    HANDLE stop_event_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> exited_{false};
    std::atomic<HWND> target_{nullptr};
    std::atomic<UINT> message_{0};
    std::atomic<bool> shake_enabled_{true};
    std::atomic<int> sensitivity_{1};
    std::atomic<int64_t> last_pt_{0};

    HWND window_ = nullptr;         // remaining fields: listener thread only
    HHOOK keyboard_hook_ = nullptr;
    DragShortcutDetector shortcut_;
    bool pointer_held_ = false;
    uint64_t last_sample_at_ = 0;
    DragShakeDetector detector_;    // listener thread only
};

} // namespace pulse::app

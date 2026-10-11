// repaint_targets.h - The windows a render cache repaints when background
// work lands (multi-window M3d).
//
// The icon and thumbnail caches are shared by every Pulse window, so a decoded
// icon must invalidate all of them, not only the window that registered last.
// Worker threads call InvalidateAll; InvalidateRect only marks the update
// region (no message is sent), so it is safe under the lock. The hidden host
// window cannot stand in for the set: hidden windows never get WM_PAINT.
#pragma once
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <vector>

namespace pulse::ui {

class RepaintTargets {
public:
    // Replaces every target (nullptr clears): caches owned by one window.
    void Set(HWND hwnd) {
        std::lock_guard lock(mutex_);
        windows_.clear();
        if (hwnd) windows_.push_back(hwnd);
    }
    void Add(HWND hwnd) {
        if (!hwnd) return;
        std::lock_guard lock(mutex_);
        if (std::find(windows_.begin(), windows_.end(), hwnd) == windows_.end()) windows_.push_back(hwnd);
    }
    void Remove(HWND hwnd) {
        std::lock_guard lock(mutex_);
        windows_.erase(std::remove(windows_.begin(), windows_.end(), hwnd), windows_.end());
    }
    size_t Count() const {
        std::lock_guard lock(mutex_);
        return windows_.size();
    }
    void InvalidateAll() const {
        std::lock_guard lock(mutex_);
        for (const HWND hwnd : windows_) InvalidateRect(hwnd, nullptr, FALSE);
    }

private:
    mutable std::mutex mutex_;
    std::vector<HWND> windows_;
};

}  // namespace pulse::ui

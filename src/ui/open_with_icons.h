// open_with_icons.h — icons of the program that opens a file type by default,
// drawn as a badge in the corner of grid thumbnails (like File Explorer), so a
// video frame no longer looks like a photo.
#pragma once
#include "ui_compositor.h"
#include "d2d_device_binding.h"
#include "repaint_targets.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pulse::ui {

class OpenWithIconCache {
public:
    OpenWithIconCache() = default;
    ~OpenWithIconCache();
    OpenWithIconCache(const OpenWithIconCache&) = delete;
    OpenWithIconCache& operator=(const OpenWithIconCache&) = delete;

    void SetDeviceContext(ID2D1DeviceContext* dc);
    void SetNotifyWindow(HWND hwnd) { repaint_.Set(hwnd); }
    void AddNotifyWindow(HWND hwnd) { repaint_.Add(hwnd); }
    void RemoveNotifyWindow(HWND hwnd) { repaint_.Remove(hwnd); }
    // Stops the worker and drops every icon (device loss, shutdown).
    void Reset();
    // Resolve every type again, retaining displayed icons until results arrive.
    void InvalidateAssociations();

    struct Badge {
        ID2D1Bitmap* bitmap = nullptr;
        // Light glyph on transparency (unplated Store logos): needs a dark plate.
        bool needs_plate = false;
        const wchar_t* format_label = nullptr; // PSD/PSB only, after icon lookup fails
    };
    // Default program icon for `name`'s extension at about `pixels` square.
    // Photos have no badge. PSD/PSB share the installed Photoshop icon, with
    // a per-format label if that editor cannot be resolved.
    // Initial requests return empty; refreshes retain the previous badge.
    Badge BadgeFor(const std::wstring& name, uint32_t pixels);

private:
    friend struct OpenWithIconCacheTest;
    struct Pixels {
        UINT width = 0;
        UINT height = 0;
        std::vector<uint8_t> data;  // 32bpp premultiplied BGRA; empty = no program
        bool needs_plate = false;
    };
    struct Request {
        std::wstring key;
        std::wstring extension;
        uint32_t pixels = 0;
        uint32_t epoch = 0;
    };
    struct Entry {
        ComPtr<ID2D1Bitmap> bitmap;
        bool needs_plate = false;
        bool missing = false;
        uint32_t epoch = 0;
    };
    void StartWorkerLocked();
    void WorkerLoop();

    D2DDeviceBinding device_binding_;
    ID2D1DeviceContext* dc_ = nullptr;  // device_binding_'s private context
    RepaintTargets repaint_;
    std::unordered_map<std::wstring, Entry> entries_;  // UI thread only

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Request> queue_;
    std::unordered_set<std::wstring> pending_;
    std::unordered_map<std::wstring, Pixels> ready_;
    std::atomic<uint32_t> epoch_{1};
    std::thread worker_;
    std::atomic<bool> running_{false};
};

} // namespace pulse::ui

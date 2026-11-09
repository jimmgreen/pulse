#pragma once
#include "thumbnail_cache.h"

namespace pulse::ui {
// UI-thread visibility scheduling; decoding and every filesystem access stay in
// a separate, low-priority preview host owned by this cache.
class FolderThumbnailCache {
public:
    struct DebugStats {
        size_t visible = 0, queued = 0, pending = 0, cached = 0, bitmaps = 0, failed = 0, blocked = 0;
        uint64_t oldest_visible_ms = 0;
        bool running = false, has_context = false;
        std::wstring error;
    };
    DebugStats ReadDebugStats();
    FolderThumbnailCache();
    void SetDeviceContext(ID2D1DeviceContext* dc) { cache_.SetDeviceContext(dc); }
    void SetNotifyWindow(HWND hwnd) { cache_.SetNotifyWindow(hwnd); }
    void AddNotifyWindow(HWND hwnd) { cache_.AddNotifyWindow(hwnd); }
    void RemoveNotifyWindow(HWND hwnd) { cache_.RemoveNotifyWindow(hwnd); }
    void Reset();
    void Refresh();
    void BeginFrame();
    void ShareUploadBudget(ThumbnailCache& other) { cache_.ShareUploadBudget(other); }
    bool TakeUploadFrameRequest() { return cache_.TakeUploadFrameRequest(); }
    void EndFrame();
    bool Tick(uint64_t now);
    PreviewDrawResult Draw(ID2D1DeviceContext* dc, const D2D1_RECT_F& dest,
        const std::wstring& path, DWORD attrs, uint32_t pixel_size, uint64_t generation,
        uint64_t modified, uint64_t size, float opacity, bool align_bottom,
        D2D1_RECT_F* artwork, bool single = false);
private:
    friend struct FolderThumbnailCacheTestAccess;
    struct Visible {
        uint64_t since = 0;
        uint64_t generation = 0;
        bool seen = false;
        bool force = false;
        bool attempted = false;
        bool admitted = false;
        bool blocked = false;
        size_t estimate = 0;
    };
    static constexpr uint64_t kSettleMs = 80;
    bool visibility_changed_ = false;
    ThumbnailCache cache_;
    std::unordered_map<std::wstring, Visible> visible_;
};
} // namespace pulse::ui

#pragma once
#include "ui_compositor.h"
#include "../ipc/preview_protocol.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace pulse::ui {
enum class PreviewDrawResult { Pending, Bitmap, Text, Hex, Failed };

struct PreviewProperty {
    std::wstring label;
    std::wstring value;
};

class ThumbnailCache {
public:
    ThumbnailCache() = default;
    ~ThumbnailCache();
    void SetDeviceContext(ID2D1DeviceContext2* dc);
    void SetNotifyWindow(HWND hwnd) { hwnd_ = hwnd; }
    void Reset();
    // pan_x/pan_y non-null: cover mode (fill dest, crop overflow) with a
    // draggable pan offset in DIPs; values are clamped and written back.
    // pan_max_x/pan_max_y (optional) receive the current pan limits.
    PreviewDrawResult Draw(ID2D1DeviceContext* dc, const D2D1_RECT_F& dest,
                           const std::wstring& path, DWORD attrs, uint32_t pixel_size,
                           uint64_t generation, uint64_t modified, uint64_t size,
                           float opacity = 1.0f, std::wstring* text = nullptr,
                           bool* truncated = nullptr, uint32_t* bytes_read = nullptr,
                           bool direct_preview = false, std::wstring* error = nullptr,
                           float* pan_x = nullptr, float* pan_y = nullptr,
                           float* pan_max_x = nullptr, float* pan_max_y = nullptr);
    bool Properties(const std::wstring& path, DWORD attrs, uint64_t generation,
                    uint64_t modified, uint64_t size,
                    std::vector<PreviewProperty>& properties);
    bool CachedProperties(const std::wstring& path, uint64_t modified, uint64_t size,
                          std::vector<PreviewProperty>& properties);
private:
    struct Item {
        ComPtr<ID2D1Bitmap> bitmap;
        std::vector<uint8_t> pixels;
        std::wstring text;
        std::wstring error;
        std::vector<PreviewProperty> properties;
        uint32_t w = 0, h = 0, stride = 0;
        uint32_t bytes_read = 0;
        ipc::PreviewContentKind kind = ipc::PreviewContentKind::None;
        bool truncated = false;
        bool failed = false;
        size_t cost = 0;
    };
    struct Request {
        uint32_t id = 0;
        uint64_t generation = 0;
        uint32_t pixels = 0;
        DWORD attrs = 0;
        ipc::PreviewRequestKind kind = ipc::PreviewRequestKind::Content;
        bool details = false;
        std::wstring path, key, identity;
    };
    void Worker();
    bool Connect();
    void StopChild();
    std::wstring Key(const std::wstring& path, uint32_t pixels, uint64_t modified,
                     uint64_t size) const;
    ID2D1DeviceContext2* dc_ = nullptr;
    HWND hwnd_ = nullptr;
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION child_{};
    std::atomic<bool> running_{false};
    std::atomic<uint32_t> next_id_{1};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Request> queue_;
    std::unordered_set<std::wstring> pending_;
    std::unordered_map<std::wstring, Item> items_;
    std::deque<std::wstring> lru_;
    size_t cache_bytes_ = 0;
    std::wstring latest_details_identity_;
    std::thread worker_;
};
} // namespace pulse::ui

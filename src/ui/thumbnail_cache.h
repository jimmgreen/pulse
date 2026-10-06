#pragma once
#include "ui_compositor.h"
#include "preview_viewport.h"
#include "icon_artwork_bounds.h"
#include "cover_palette.h"
#include "../ipc/preview_protocol.h"
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <list>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace pulse::ui {
// Archive: `text` carries the archive tree payload for ArchivePreview.
enum class PreviewDrawResult { Pending, Bitmap, Text, Hex, Failed, Archive, Markdown, Table, Tree };

struct PreviewProperty {
    std::wstring label;
    std::wstring value;
};

// Title, artist and album, in that order.
using AudioMetaValues = std::array<std::wstring, 3>;

class ThumbnailCache {
public:
    // Defaults suit a single large preview (Quick Look); grid thumbnails and
    // the details pane get their own instances and budgets (ui_renderer.h) so
    // a 2048 px selection preview never evicts the visible grid.
    static constexpr size_t kDefaultBudgetBytes = 16ull * 1024ull * 1024ull;
    static constexpr size_t kDefaultMaxItems = 128;
    explicit ThumbnailCache(size_t budget_bytes = kDefaultBudgetBytes,
                            size_t max_items = kDefaultMaxItems);
    ~ThumbnailCache();
    void SetDeviceContext(ID2D1DeviceContext* dc);
    void SetNotifyWindow(HWND hwnd) { hwnd_ = hwnd; }
    // Quick Look's cache: folders come as contents listings and Markdown as
    // rendered documents.
    void SetQuickLookContent(bool on) { quick_look_content_ = on; }
    void SetFolderThumbnail(bool on) { folder_thumbnail_ = on; }
    void Reset();
    void Evict();
    // pan_x/pan_y non-null: cover mode (fill dest, crop overflow) with a
    // draggable pan offset in DIPs; values are clamped and written back.
    // pan_max_x/pan_max_y (optional) receive the current pan limits.
    // align_artwork_bottom applies only to contain mode. artwork_rect receives
    // the drawn alpha bounds in destination coordinates (dest while pending).
    PreviewDrawResult Draw(ID2D1DeviceContext* dc, const D2D1_RECT_F& dest,
                           const std::wstring& path, DWORD attrs, uint32_t pixel_size,
                           uint64_t generation, uint64_t modified, uint64_t size,
                           float opacity = 1.0f, std::wstring* text = nullptr,
                           bool* truncated = nullptr, uint32_t* bytes_read = nullptr,
                           bool direct_preview = false, std::wstring* error = nullptr,
                           float* pan_x = nullptr, float* pan_y = nullptr,
                           float* pan_max_x = nullptr, float* pan_max_y = nullptr,
                           uint32_t frame_index = 0, uint32_t* frame_count = nullptr,
                           uint32_t* frame_delay_ms = nullptr, uint32_t* loop_count = nullptr,
                           uint32_t* decoded_width = nullptr, uint32_t* decoded_height = nullptr,
                           uint32_t* source_width = nullptr, uint32_t* source_height = nullptr,
                           PreviewViewport* viewport = nullptr,
                           uint32_t* text_encoding = nullptr,
                           bool align_artwork_bottom = false,
                           D2D1_RECT_F* artwork_rect = nullptr,
                           uint32_t* media_duration_ms = nullptr,
                           preview::Integrity* integrity = nullptr);
    // media_duration_ms (optional): a video's playing time, 0 when unknown.
    PreviewDrawResult DrawGridThumbnail(ID2D1DeviceContext* dc, const D2D1_RECT_F& dest,
        const std::wstring& path, DWORD attrs, uint32_t pixel_size, uint64_t generation,
        uint64_t modified, uint64_t size, float opacity, bool align_bottom,
        D2D1_RECT_F* artwork, uint32_t* media_duration_ms = nullptr);
    bool Properties(const std::wstring& path, DWORD attrs, uint64_t generation,
                    uint64_t modified, uint64_t size,
                    std::vector<PreviewProperty>& properties);
    bool CachedProperties(const std::wstring& path, uint64_t modified, uint64_t size,
                          std::vector<PreviewProperty>& properties);
    // Title, artist and album of one audio file, for the details columns.
    // Kept apart from Properties because all visible rows want them at once,
    // while the details pane retires its cache every time the selection moves.
    // Three empty strings are a real answer, so an untagged file is not asked
    // for again on every frame.
    bool AudioMeta(const std::wstring& path, DWORD attrs, uint64_t generation,
                   uint64_t modified, uint64_t size, AudioMetaValues& values);
    bool CachedAudioMeta(const std::wstring& path, uint64_t modified, uint64_t size,
                         AudioMetaValues& values);
    // Quick Look only: the cover colours of the still bitmap Draw shows for
    // this file (the exact size, else the stale one drawn meanwhile). False
    // until decoded, and for covers without a usable colour.
    bool Palette(const std::wstring& path, uint32_t pixel_size, uint64_t modified,
                 uint64_t size, CoverPalette& palette);
private:
    friend struct ThumbnailCacheTestAccess;
    friend class FolderThumbnailCache;
    friend struct FolderThumbnailCacheTestAccess;
    struct Item {
        ComPtr<ID2D1Bitmap> bitmap;
        std::vector<uint8_t> pixels;
        IconArtworkBounds artwork_bounds;
        CoverPalette palette;   // Quick Look content only (see Palette)
        std::wstring text;
        std::wstring error;
        std::vector<PreviewProperty> properties;
        uint32_t w = 0, h = 0, stride = 0;
        uint32_t bytes_read = 0;
        uint32_t frame_count = 1;
        uint32_t frame_delay_ms = 0;
        uint32_t loop_count = 0;
        uint32_t text_encoding = 0;  // ipc::PreviewTextEncoding of Text previews
        uint32_t frame_index = 0;
        uint32_t source_width = 0;
        uint32_t source_height = 0;
        uint32_t duration_ms = 0;  // videos: System.Media.Duration
        std::wstring animation_identity;
        ipc::PreviewContentKind kind = ipc::PreviewContentKind::None;
        bool truncated = false;
        preview::Integrity integrity;
        bool failed = false;
        // Transport failure (host timeout / crash): retried with back-off
        // instead of being remembered as "this file has no preview".
        bool transient = false;
        bool folder_refresh_complete = false;
        uint64_t retry_at = 0;
        size_t cost = 0;
        std::list<std::wstring>::iterator lru_position;
    };
    struct Request {
        uint32_t id = 0;
        uint64_t generation = 0;
        uint32_t pixels = 0;
        DWORD attrs = 0;
        ipc::PreviewRequestKind kind = ipc::PreviewRequestKind::Content;
        uint32_t frame_index = 0;
        uint32_t epoch = 0;
        bool details = false;
        uint32_t timeout_ms = 0;
        uint32_t flags = 0;
        std::wstring path, key, identity;
    };
    // Last decoded still bitmap of a file at any pixel size: drawn while a
    // different size is pending or being retried, so a thumbnail never drops
    // back to the file-type icon once it has been shown.
    Item* StaleBitmap(const std::wstring& identity, const std::wstring& except_key);
    static uint32_t ResponseTimeoutMs(const std::wstring& path, ipc::PreviewRequestKind kind);
    void Worker();
    bool FolderRequestCurrent(const Request& request);
    static bool IsTransientResponse(bool transport_ok, const Request& request, uint32_t status);
    void SelectDetailsLocked(const std::wstring& identity);
    bool StoreResult(const Request& request, Item result);
    void Touch(Item& item);
    bool Connect();
    void StopChild();
    std::wstring Key(const std::wstring& path, uint32_t pixels, uint64_t modified,
                     uint64_t size, uint32_t frame_index = 0) const;
    ID2D1DeviceContext* dc_ = nullptr;
    std::atomic<HWND> hwnd_{nullptr};
    std::atomic<bool> quick_look_content_{false};
    std::atomic<bool> folder_thumbnail_{false};
    std::unordered_map<std::wstring, uint32_t> folder_requests_;
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION child_{};
    std::atomic<bool> running_{false};
    std::atomic<uint32_t> next_id_{1};
    uint32_t pipe_token_ = 0;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Request> queue_;
    std::unordered_set<std::wstring> pending_;
    std::unordered_map<std::wstring, Item> items_;
    std::list<std::wstring> lru_;
    size_t cache_bytes_ = 0;
    size_t budget_bytes_ = kDefaultBudgetBytes;
    size_t max_items_ = kDefaultMaxItems;
    std::unordered_map<std::wstring, std::wstring> still_by_identity_;
    std::unordered_map<std::wstring, uint32_t> transient_failures_;
    std::wstring latest_details_identity_;
    std::atomic<uint32_t> epoch_{1};
    std::thread worker_;
};
} // namespace pulse::ui

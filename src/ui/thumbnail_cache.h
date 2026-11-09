#include <future>
#pragma once
#include "ui_compositor.h"
#include <utility>
#include "d2d_device_binding.h"
#include "repaint_targets.h"
#include "preview_viewport.h"
#include "icon_artwork_bounds.h"
#include "cover_palette.h"
#include "../ipc/preview_protocol.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pulse::ui {
// Archive: `text` carries the archive tree payload for ArchivePreview.
enum class PreviewDrawResult { Pending, Bitmap, Text, Hex, Failed, Archive, Markdown, Table, Tree, Custom };

struct PreviewProperty {
    std::wstring label;
    std::wstring value;
};

// A finished grid thumbnail as the persistent store keeps it.
struct StoredThumbnail {
    std::vector<uint8_t> pixels;  // premultiplied BGRA, top-down, width * 4 per row
    uint32_t width = 0, height = 0;
    uint32_t source_width = 0, source_height = 0;
    uint32_t duration_ms = 0;
};
struct ThumbnailStoreKey {
    const std::wstring& path;
    DWORD attrs;
    uint32_t pixels;
    uint64_t modified, size;
    bool keep_always = false;  // folder composites and refined PSDs: kept however quick
};
// Second level under the in-memory LRU for thumbnails that were expensive to
// make (thumbnail_disk_cache.h). Called on the grid cache's worker threads.
class ThumbnailStore {
public:
    virtual ~ThumbnailStore() = default;
    virtual bool Load(const ThumbnailStoreKey& key, StoredThumbnail& out) = 0;
    // decode_ms: how long the host took; quick results are not worth keeping.
    virtual void Save(const ThumbnailStoreKey& key, uint32_t decode_ms, const StoredThumbnail& thumb) = 0;
};

struct DocumentImageSession {
    DocumentImageSession();
    const uint64_t identity;
    std::atomic<bool> active{true};
};

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
    void SetNotifyWindow(HWND hwnd) { repaint_.Set(hwnd); }
    void AddNotifyWindow(HWND hwnd) { repaint_.Add(hwnd); }
    void RemoveNotifyWindow(HWND hwnd);
    // Quick Look's cache: folders come as contents listings and Markdown as
    // rendered documents.
    void SetQuickLookContent(bool on) { quick_look_content_ = on; }
    void SetFolderThumbnail(bool on) { folder_thumbnail_ = on; }
    // Extra ipc::kPreviewRequestFlag* bits sent with every request of this
    // cache (the hover card's kPreviewRequestFlagCard).
    void SetRequestFlags(uint32_t flags) { extra_flags_ = flags; }
    // Non-zero: a request still unanswered after this long is given up as
    // soon as a request for another file is waiting - the host is restarted
    // rather than finishing a decode nobody is looking at any more.
    void SetAbandonStaleMs(uint32_t ms) { abandon_stale_ms_ = ms; }
    // Grid caches: decode on up to `lanes` host processes at once (0 picks
    // from the processor count, at most kMaxLanes) and take the visible
    // requests in the order the last frame drew them. Slow formats (video,
    // shell providers) never hold every lane. Call before the first request.
    void SetParallelism(uint32_t lanes);
    static uint32_t DefaultParallelism();
    // Once per painted frame of a SetParallelism cache: what this frame draws
    // goes ahead of requests only earlier frames asked for.
    void AdvanceFrame(HWND window = nullptr);
    void EndInterestFrame();
    bool TakeUploadFrameRequest() { std::lock_guard lock(mutex_); return std::exchange(upload_frame_pending_, false); }
    static constexpr uint32_t kMaxLanes = 4;
    // The store SetParallelism caches read before asking a host and write
    // after a slow decode; null (tests, tools) keeps memory only.
    static void SetGridStore(ThumbnailStore* store);
    // Grid caches: queue the thumbnail of an item just past the visible ones,
    // behind everything drawn. Quick formats only. False when the queue is
    // too busy to take it now (try again on a later frame).
    bool Prefetch(const std::wstring& path, DWORD attrs, uint32_t pixel_size,
                  uint64_t generation, uint64_t modified, uint64_t size);
    // Grid caches: start the hosts before the first request (a folder shown
    // as thumbnails is still being listed).
    void Warm();
    void Reset();
    void Evict();
    std::shared_future<void> SuspendVolume(const std::wstring& root);
    void ResumeVolume(const std::wstring& root);
    void BeginFrame();
    // Link only caches drawn in the same UI frame; setup precedes drawing.
    void ShareUploadBudget(ThumbnailCache& other) {
        if (!other.shared_upload_budget_) other.shared_upload_budget_ = std::make_shared<UploadBudget>();
        shared_upload_budget_ = other.shared_upload_budget_;
    }
    uint64_t NextRetryDeadline();
    uint64_t WindowRetryDeadline(HWND window);
    // Earliest retry encountered by Draw since the previous call. Zero means idle.
    uint64_t TakeRetryDeadline();
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
                           preview::Integrity* integrity = nullptr,
                           const std::shared_ptr<DocumentImageSession>& document = {},
                           std::shared_ptr<const std::wstring>* shared_text = nullptr);
    // shared_text is reset on every call; empty payloads return a null holder.
    // A nonempty holder owns immutable text independently of cache lifetime.
    // media_duration_ms (optional): a video's playing time, 0 when unknown.
    // Outgoing view animations reuse uploaded artwork without keeping decode
    // requests alive or uploading results that the target view will not use.
    PreviewDrawResult DrawCachedGridThumbnail(ID2D1DeviceContext* dc, const D2D1_RECT_F& dest,
        const std::wstring& path, DWORD attrs, uint32_t pixel_size,
        uint64_t modified, uint64_t size, float opacity, bool align_bottom,
        D2D1_RECT_F* artwork);
    PreviewDrawResult DrawGridThumbnail(ID2D1DeviceContext* dc, const D2D1_RECT_F& dest,
        const std::wstring& path, DWORD attrs, uint32_t pixel_size, uint64_t generation,
        uint64_t modified, uint64_t size, float opacity, bool align_bottom,
        D2D1_RECT_F* artwork, uint32_t* media_duration_ms = nullptr);
    // Photo wall tiles: the picture fills dest, cropping the overflow (a decode
    // of another size meanwhile is stretched). aspect receives the decoded
    // width / height, 0 until there is a bitmap.
    PreviewDrawResult DrawWallThumbnail(ID2D1DeviceContext* dc, const D2D1_RECT_F& dest,
        const std::wstring& path, DWORD attrs, uint32_t pixel_size, uint64_t generation,
        uint64_t modified, uint64_t size, float opacity, float* aspect,
        uint32_t* media_duration_ms = nullptr);
    bool Properties(const std::wstring& path, DWORD attrs, uint64_t generation,
                    uint64_t modified, uint64_t size,
                    std::vector<PreviewProperty>& properties);
    bool CachedProperties(const std::wstring& path, uint64_t modified, uint64_t size,
                          std::vector<PreviewProperty>& properties);
    // Quick Look only: the cover colours of the still bitmap Draw shows for
    // this file (the exact size, else the stale one drawn meanwhile). False
    // until decoded, and for covers without a usable colour.
    bool Palette(const std::wstring& path, uint32_t pixel_size, uint64_t modified,
                 uint64_t size, CoverPalette& palette);
    // Quick Look only: histogram and transparency of the same bitmap.
    bool Stats(const std::wstring& path, uint32_t pixel_size, uint64_t modified,
               uint64_t size, ImageStats& stats);
private:
    friend struct QuickPreviewAuditTest;
    friend struct PreviewDeadlineProbe;
    friend struct ThumbnailCacheTestAccess;
    friend class FolderThumbnailCache;
    friend struct FolderThumbnailCacheTestAccess;
    struct Item {
        bool needs_psd_refine = false;
        uint64_t upload_token = 0;
        bool upload_failed = false;
        ComPtr<ID2D1Bitmap> bitmap;
        std::vector<uint8_t> pixels;
        IconArtworkBounds artwork_bounds;
        CoverPalette palette;   // Quick Look content only (see Palette)
        ImageStats stats;       // Quick Look content only (see Stats)
        std::wstring text;
        std::shared_ptr<const std::wstring> shared_text;
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
        uint64_t queued_at = GetTickCount64();
        uint32_t id = 0;
        uint64_t generation = 0;
        uint32_t pixels = 0;
        DWORD attrs = 0;
        ipc::PreviewRequestKind kind = ipc::PreviewRequestKind::Content;
        uint32_t frame_index = 0;
        uint32_t epoch = 0;
        bool details = false;
        bool interest_tracked = false;
        // Set when taken: on screen and quick, so its host runs a step higher.
        bool urgent = false;
        uint32_t timeout_ms = 0;
        uint32_t budget_deferrals = 0;
        uint32_t flags = 0;
        uint64_t modified = 0, size = 0;
        std::wstring path, key, identity;
        std::shared_ptr<DocumentImageSession> document;
    };
    // Last decoded still bitmap of a file at any pixel size: drawn while a
    // different size is pending or being retried, so a thumbnail never drops
    // back to the file-type icon once it has been shown.
    void PrepareBitmap(const std::wstring& key, bool budgeted);
    uint64_t upload_sequence_ = 0, device_generation_ = 0, upload_elapsed_us_ = 0;
    size_t upload_bytes_ = 0;
    struct UploadBudget { std::atomic<uint64_t> elapsed_us{0}; std::atomic<size_t> bytes{0}; };
    std::shared_ptr<UploadBudget> shared_upload_budget_;
    bool upload_frame_pending_ = false;
    void (*upload_probe_)(ThumbnailCache&) = nullptr;
    Item* StaleBitmap(const std::wstring& identity, const std::wstring& except_key);
    static uint32_t ResponseTimeoutMs(const std::wstring& path, ipc::PreviewRequestKind kind, uint64_t source_size = 0);
    void Worker() { Worker(0); }
    void Worker(uint32_t lane);
    // Lane 0 is pipe_ / child_ / pipe_token_ / worker_; the rest live in extra_lanes_.
    struct ExtraLane {
        HANDLE pipe = INVALID_HANDLE_VALUE;
        PROCESS_INFORMATION child{};
        uint32_t token = 0;
        std::thread worker;
    };
    struct LaneRef {
        HANDLE& pipe;
        PROCESS_INFORMATION& child;
        uint32_t token;
    };
    LaneRef LaneAt(uint32_t lane);
    void StartWorkersLocked();
    bool TakeRequestLocked(Request& out, bool& slow);
    void DropOverflowLocked();
    static bool IsSlowRequest(const Request& request) noexcept;
    bool FolderRequestCurrent(const Request& request);
    static bool IsTransientResponse(bool transport_ok, const Request& request, uint32_t status);
    void SelectDetailsLocked(const std::wstring& identity);
    bool StoreResult(const Request& request, Item result);
    void QueuePsdRefinement(const Request& request);
    void Touch(Item& item);
    bool Connect(uint32_t lane, const Request& request, uint64_t started);
    void LogFailure(uint32_t lane, const Request& request, uint64_t started, uint32_t stage,
                    DWORD error, bool cancelled = false);
    void StopChild(uint32_t lane);
    void StopChildren();
    std::wstring Key(const std::wstring& path, uint32_t pixels, uint64_t modified,
                     uint64_t size, uint32_t frame_index = 0) const;
    D2DDeviceBinding device_binding_;
    ID2D1DeviceContext* dc_ = nullptr;  // device_binding_'s private context
    struct WindowInterest {
        std::unordered_set<std::wstring> committed, drawing, visible, drawing_visible;
        bool active = false;
    };
    std::unordered_map<HWND, WindowInterest> interests_;
    HWND interest_window_ = nullptr;
    bool WantedByWindowLocked(const std::wstring& key) const;
    void NotifyResult(const Request& req);
    RepaintTargets repaint_;
    std::atomic<bool> quick_look_content_{false};
    std::atomic<bool> folder_thumbnail_{false};
    std::atomic<uint32_t> extra_flags_{0};
    std::atomic<uint32_t> abandon_stale_ms_{0};
    std::unordered_map<std::wstring, uint32_t> folder_requests_;
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION child_{};
    std::atomic<bool> running_{false};
    std::atomic<uint32_t> next_id_{1};
    uint32_t pipe_token_ = 0;
    std::mutex mutex_;
    uint64_t draw_retry_deadline_ = 0;
    std::condition_variable cv_;
    std::deque<Request> queue_;
    std::unordered_set<std::wstring> pending_;
    std::unordered_set<std::wstring> retry_interest_;
    bool track_retry_interest_ = false;
    std::unordered_map<std::wstring, Item> items_;
    std::list<std::wstring> lru_;
    size_t cache_bytes_ = 0;
    size_t budget_bytes_ = kDefaultBudgetBytes;
    size_t max_items_ = kDefaultMaxItems;
    std::unordered_map<std::wstring, std::wstring> still_by_identity_;
    std::unordered_map<std::wstring, uint32_t> transient_failures_;
    std::wstring latest_details_identity_;
    std::atomic<uint32_t> epoch_{1};
    struct VolumeRelease {
        std::promise<void> promise;
        std::shared_future<void> future = promise.get_future().share();
        size_t pending = 0;
        bool failed = false;
    };
    bool VolumeSuspendedLocked(const std::wstring& path) const;
    std::unordered_map<std::wstring, std::shared_ptr<VolumeRelease>> suspended_volumes_;
    std::wstring lane_paths_[kMaxLanes];
    std::shared_ptr<VolumeRelease> release_lanes_[kMaxLanes];
    std::thread worker_;
    ExtraLane extra_lanes_[kMaxLanes - 1];
    uint32_t lane_count_ = 1;
    bool visible_order_ = false;
    bool boost_hosts_ = false;
    uint32_t slow_in_flight_ = 0;
    uint32_t psd_refinements_in_flight_ = 0;
    bool lane_busy_[kMaxLanes]{};
    std::atomic<bool> warm_{false};
    uint64_t frame_ = 1;
    uint64_t draw_sequence_ = 0;
    // Visible order: queued key -> (frame, paint sequence) of its latest draw.
    std::unordered_map<std::wstring, std::pair<uint64_t, uint64_t>> wanted_;
};
} // namespace pulse::ui

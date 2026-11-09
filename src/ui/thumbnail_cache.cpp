#include <stdexcept>
#include <chrono>
#include "thumbnail_cache.h"
#include "../common/decode_memory_budget.h"
#include "../fs/cloud_state.h"
#include "../common/utf8_file.h"
#include "../common/runtime_log.h"
#include "../common/preview_diagnostics.h"
#include "../common/preview_extensions.h"
#include "../common/module_location.h"
#include "thumbnail_artwork_layout.h"
#include <algorithm>
#include <cwctype>
#include <iterator>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <psapi.h>

namespace pulse::ui {
uint64_t ThumbnailCache::TakeRetryDeadline() {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto deadline = draw_retry_deadline_;
    draw_retry_deadline_ = 0;
    return deadline;
}
namespace {
std::atomic<uint32_t> g_preview_cache_sequence{1};
std::atomic<ThumbnailStore*> g_grid_store{nullptr};

// Transport failures retry after 1 s, 4 s, then 15 s; the fourth in a row is
// kept as a real failure so a file that always hangs its provider settles.
constexpr uint32_t kTransientRetryMs[] = {1000, 4000, 15000};

bool UploadBitmap(ID2D1DeviceContext* dc, ComPtr<ID2D1Bitmap>& bitmap,
                  std::vector<uint8_t>& pixels, uint32_t w, uint32_t h, uint32_t stride) {
    if (bitmap.get()) return true;
    if (!dc || pixels.empty() || !w || !h) return false;
    auto props = D2D1::BitmapProperties(D2D1::PixelFormat(
        DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(dc->CreateBitmap(D2D1::SizeU(w, h), pixels.data(), stride, &props, &bitmap)))
        return false;
    return true;
}

// Set by DrawWallThumbnail around its Draw: DrawContained fills instead.
thread_local bool t_cover_fill = false;

// Aspect-fit (contain), optionally placing the visible artwork at dest.bottom.
void DrawContained(ID2D1DeviceContext* dc, ID2D1Bitmap* bitmap, uint32_t w, uint32_t h,
                   const D2D1_RECT_F& dest, float opacity, const IconArtworkBounds& bounds,
                   bool align_artwork_bottom, D2D1_RECT_F* artwork_rect) {
    if (t_cover_fill && w > 0 && h > 0) {
        // Photo wall: fill the tile, centred, cropping what overflows.
        const float dw = dest.right - dest.left, dh = dest.bottom - dest.top;
        const float k = std::max(dw / static_cast<float>(w), dh / static_cast<float>(h));
        const float cw = static_cast<float>(w) * k, ch = static_cast<float>(h) * k;
        const D2D1_RECT_F fitted = D2D1::RectF(dest.left + (dw - cw) * 0.5f, dest.top + (dh - ch) * 0.5f,
                                               dest.left + (dw + cw) * 0.5f, dest.top + (dh + ch) * 0.5f);
        if (artwork_rect) *artwork_rect = dest;
        dc->PushAxisAlignedClip(dest, D2D1_ANTIALIAS_MODE_ALIASED);
        dc->DrawBitmap(bitmap, &fitted, std::clamp(opacity, 0.0f, 1.0f),
            D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        dc->PopAxisAlignedClip();
        return;
    }
    const auto fitted = ContainedThumbnailRect(dest, w, h, bounds, align_artwork_bottom);
    if (artwork_rect) *artwork_rect = ThumbnailArtworkRect(fitted, bounds);
    dc->DrawBitmap(bitmap, &fitted, std::clamp(opacity, 0.0f, 1.0f),
        D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
}
} // namespace

// Shell thumbnail providers (Office, CAD, video) routinely need a few seconds
// on a cold start; direct decoders answer well inside the short budget. A
// timeout restarts the host, so the heavy formats get room to finish.
namespace {
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// One poll step while the host works on a request. Sleep(10) really sleeps
// a whole 15.6 ms scheduler tick, which put a floor under every preview (a
// text file took ~22 ms, most of it this wait). The first 250 ms - when
// nearly every answer arrives - poll each millisecond through a
// high-resolution timer; slow decodes then back off to the old step.
struct HostWaitTimer {
    HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                           TIMER_ALL_ACCESS);
    ~HostWaitTimer() { if (handle) CloseHandle(handle); }
};

void WaitForHost(ULONGLONG waited_ms) {
    if (waited_ms >= 250) { Sleep(10); return; }
    thread_local HostWaitTimer timer;
    LARGE_INTEGER due{};
    due.QuadPart = -10000;  // 1 ms
    if (timer.handle && SetWaitableTimer(timer.handle, &due, 0, nullptr, nullptr, FALSE))
        WaitForSingleObject(timer.handle, 20);
    else
        Sleep(1);
}
}  // namespace

uint32_t ThumbnailCache::ResponseTimeoutMs(const std::wstring& path,
                                           ipc::PreviewRequestKind kind, uint64_t source_size) {
    if (kind == ipc::PreviewRequestKind::Properties) return 4000;
    const size_t slash = path.find_last_of(L"\\/");
    const size_t dot = path.find_last_of(L'.');
    std::wstring ext = dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)
        ? std::wstring() : path.substr(dot);
    for (auto& c : ext) c = static_cast<wchar_t>(std::towlower(c));
    // Large lossless PNGs can spend seconds streaming compressed scanlines.
    // Snapshot size is already available; never stat the file on the UI thread.
    if (ext == L".png" && source_size >= (32ull << 20)) {
        const auto chunks = (std::min)(uint64_t(12), source_size / (32ull << 20) + 1);
        return static_cast<uint32_t>((std::max)(uint64_t(10000), chunks * 2500));
    }
    static const wchar_t* const kLight[] = {
        L".jpg", L".jpeg", L".png", L".bmp", L".gif", L".ico", L".webp", L".tif", L".tiff",
        L".txt", L".md", L".json", L".xml", L".csv", L".log", L".ini", L".svg"};
    for (const wchar_t* light : kLight)
        if (ext == light) return 2500;
    return 8000;
}

DocumentImageSession::DocumentImageSession() : identity([] {
    static std::atomic<uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}()) {}

ThumbnailCache::ThumbnailCache(size_t budget_bytes, size_t max_items)
    : budget_bytes_(std::max<size_t>(budget_bytes, 1)),
      max_items_(std::max<size_t>(max_items, 1)) {
    const uint32_t sequence = g_preview_cache_sequence.fetch_add(1);
    pipe_token_ = GetCurrentProcessId() ^ (sequence * 0x9E3779B9u);
    if (!pipe_token_) pipe_token_ = sequence ? sequence : 1;
    for (uint32_t lane = 1; lane < kMaxLanes; ++lane) {
        uint32_t token = pipe_token_ ^ (lane * 0x85EBCA6Bu);
        if (!token || token == pipe_token_) token = pipe_token_ + lane;
        extra_lanes_[lane - 1].token = token;
    }
}

void ThumbnailCache::SetGridStore(ThumbnailStore* store) {
    g_grid_store.store(store, std::memory_order_release);
}

namespace {
bool ThumbnailPathOnVolume(const std::wstring& path, const std::wstring& root) {
    return !root.empty() && path.size() >= root.size() &&
        _wcsnicmp(path.c_str(), root.c_str(), root.size()) == 0 &&
        (root.back() == L'\\' || path.size() == root.size() || path[root.size()] == L'\\');
}
}
bool ThumbnailCache::Prefetch(const std::wstring& path, DWORD attrs, uint32_t pixel_size,
                              uint64_t generation, uint64_t modified, uint64_t size) {
    if (path.empty() || pixel_size < 24) return true;
    // Room left for whatever the next frames draw.
    constexpr size_t kPrefetchQueueLimit = 32;
    const std::wstring key = Key(path, pixel_size, modified, size);
    std::lock_guard lock(mutex_);
    if (VolumeSuspendedLocked(path)) return false;
    if (interest_window_) interests_[interest_window_].drawing.insert(key);
    if (!visible_order_ || items_.contains(key)) return true;
    if (pending_.contains(key)) {
        // An item that scrolled into the prefetch range is still wanted.
        // Do not demote a request drawn by another pane in this frame.
        if (auto it = wanted_.find(key); it != wanted_.end() && it->second.first < frame_)
            it->second = {0, ++draw_sequence_};
        return true;
    }
    if (queue_.size() >= kPrefetchQueueLimit) return false;
    Request request;
    request.interest_tracked = interest_window_ != nullptr;
    request.kind = ipc::PreviewRequestKind::Content;
    request.timeout_ms = ResponseTimeoutMs(path, request.kind, size);
    // A video poster ahead of time would hold a lane for seconds.
    if (IsSlowRequest(request)) return true;
    request.id = next_id_++; request.generation = generation;
    request.pixels = pixel_size; request.attrs = attrs;
    request.epoch = epoch_.load(std::memory_order_relaxed);
    request.path = path; request.key = key; request.identity = Key(path, 0, modified, size);
    request.modified = modified; request.size = size;
    // Frame 0 ranks behind every drawn request and is dropped first.
    wanted_[key] = {0, ++draw_sequence_};
    pending_.insert(key);
    queue_.push_back(std::move(request));
    DropOverflowLocked();
    StartWorkersLocked();
    cv_.notify_one();
    return true;
}

void ThumbnailCache::Warm() {
    std::lock_guard lock(mutex_);
    if (!visible_order_ || running_) return;
    warm_ = true;
    StartWorkersLocked();
}

uint32_t ThumbnailCache::DefaultParallelism() {
    // PULSE_THUMBNAIL_LANES=1..4 pins the count for measurements.
    wchar_t forced[8]{};
    if (GetEnvironmentVariableW(L"PULSE_THUMBNAIL_LANES", forced, 8) > 0 && _wtoi(forced) > 0)
        return (std::min)(static_cast<uint32_t>(_wtoi(forced)), kMaxLanes);
    const DWORD processors = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    // Leave room for the UI thread and the compositor.
    return std::clamp<uint32_t>(processors > 2 ? processors - 2 : 1, 2, kMaxLanes);
}

void ThumbnailCache::SetParallelism(uint32_t lanes) {
    std::lock_guard lock(mutex_);
    if (running_) return;
    lane_count_ = std::clamp<uint32_t>(lanes ? lanes : DefaultParallelism(), 1, kMaxLanes);
    visible_order_ = true;
    // Other programs busy on every core halve the grid's speed: hosts at
    // normal priority get a fair share at best. On screen thumbnails raise
    // their host a step, but only while two cores stay free for the UI.
    // PULSE_THUMBNAIL_BOOST=0 turns it off for measurements.
    wchar_t boost[4]{};
    boost_hosts_ = !folder_thumbnail_ &&
        GetActiveProcessorCount(ALL_PROCESSOR_GROUPS) >= lane_count_ + 2 &&
        !(GetEnvironmentVariableW(L"PULSE_THUMBNAIL_BOOST", boost, 4) > 0 && boost[0] == L'0');
}

bool ThumbnailCache::WantedByWindowLocked(const std::wstring& key) const {
    for (const auto& [window, interest] : interests_) {
        (void)window;
        if (interest.committed.contains(key) || (interest.active && interest.drawing.contains(key))) return true;
    }
    return false;
}

void ThumbnailCache::RemoveNotifyWindow(HWND window) {
    std::lock_guard lock(mutex_);
    interests_.erase(window);
    if (interest_window_ == window) interest_window_ = nullptr;
    repaint_.Remove(window);
    std::erase_if(queue_, [&](const Request& req) {
        if (!req.interest_tracked || WantedByWindowLocked(req.key)) return false;
        pending_.erase(req.key); wanted_.erase(req.key); return true;
    });
}

void ThumbnailCache::EndInterestFrame() {
    std::lock_guard lock(mutex_);
    if (!interest_window_) return;
    auto& interest = interests_.at(interest_window_);
    interest.committed.swap(interest.drawing);
    interest.drawing.clear();
    interest.visible.swap(interest.drawing_visible);
    interest.drawing_visible.clear();
    interest.active = false;
    interest_window_ = nullptr;
    std::erase_if(queue_, [&](const Request& req) {
        if (!req.interest_tracked || WantedByWindowLocked(req.key)) return false;
        pending_.erase(req.key); wanted_.erase(req.key);
        return true;
    });
}

void ThumbnailCache::NotifyResult(const Request& req) {
    std::lock_guard lock(mutex_);
    if (!req.interest_tracked) { repaint_.InvalidateAll(); return; }
    for (const auto& [window, interest] : interests_)
        if (interest.committed.contains(req.key) || (interest.active && interest.drawing.contains(req.key)))
            InvalidateRect(window, nullptr, FALSE);
}

void ThumbnailCache::AdvanceFrame(HWND window) {
    std::lock_guard lock(mutex_);
    interest_window_ = window;
    if (window) { auto& interest = interests_[window]; interest.active = true; interest.drawing.clear(); interest.drawing_visible.clear(); }
    upload_elapsed_us_ = 0; upload_bytes_ = 0;
    if (shared_upload_budget_) { shared_upload_budget_->elapsed_us = 0; shared_upload_budget_->bytes = 0; }
    ++frame_;
    draw_sequence_ = 0;
    // Keep two frames of grace for reversals. Only retire queued grid work:
    // prefetch (rank 0), explicit previews and running decodes stay intact.
    if (visible_order_) {
        std::erase_if(queue_, [&](const Request& request) {
            if (request.interest_tracked) return false;
            const auto wanted = wanted_.find(request.key);
            if (request.details || wanted == wanted_.end() || wanted->second.first == 0 ||
                frame_ <= wanted->second.first || frame_ - wanted->second.first <= 2) return false;
            pending_.erase(request.key);
            wanted_.erase(wanted);
            return true;
        });
    }
    // Keys drawn while their decode was already running stay behind; keep
    // only what is still queued.
    if (wanted_.size() > 256) {
        std::unordered_map<std::wstring, std::pair<uint64_t, uint64_t>> queued;
        for (const auto& request : queue_)
            if (const auto it = wanted_.find(request.key); it != wanted_.end()) queued.insert(*it);
        wanted_.swap(queued);
    }
}

ThumbnailCache::LaneRef ThumbnailCache::LaneAt(uint32_t lane) {
    if (lane == 0 || lane >= kMaxLanes) return {pipe_, child_, pipe_token_};
    ExtraLane& extra = extra_lanes_[lane - 1];
    return {extra.pipe, extra.child, extra.token};
}

void ThumbnailCache::StartWorkersLocked() {
    if (running_.exchange(true)) return;
    worker_ = std::thread([this] { Worker(0); });
    for (uint32_t lane = 1; lane < lane_count_; ++lane)
        extra_lanes_[lane - 1].worker = std::thread([this, lane] { Worker(lane); });
}

// Formats outside the light list (video posters, shell providers, Office,
// CAD) can take seconds; they may hold every lane but one.
bool ThumbnailCache::IsSlowRequest(const Request& request) noexcept {
    return (request.flags & ipc::kPreviewRequestFlagPsdRefine) ||
        (request.kind == ipc::PreviewRequestKind::Content && request.timeout_ms > 2500);
}

bool ThumbnailCache::TakeRequestLocked(Request& out, bool& slow) {
    if (queue_.empty()) return false;
    const uint32_t slow_cap = lane_count_ > 1 ? lane_count_ - 1 : 1;
    auto pick = queue_.end();
    std::pair<uint64_t, uint64_t> best{0, 0};
    for (auto it = queue_.begin(); it != queue_.end(); ++it) {
        if (slow_in_flight_ >= slow_cap && IsSlowRequest(*it)) continue;
        const bool refine = (it->flags & ipc::kPreviewRequestFlagPsdRefine) != 0;
        // One composite upgrade at a time leaves slow slots for new first-pass
        // requests on wider pools and avoids concurrent large-document I/O.
        if (refine && psd_refinements_in_flight_) continue;
        const bool picked_refine = pick != queue_.end() && (pick->flags & ipc::kPreviewRequestFlagPsdRefine);
        if (pick != queue_.end() && !picked_refine && refine) continue;
        if (picked_refine && !refine) pick = queue_.end();
        if (!visible_order_) { if (pick == queue_.end()) pick = it; if (!refine) break; continue; }
        // Latest frame first, then paint order (top-left first).
        std::pair<uint64_t, uint64_t> rank{0, UINT64_MAX};
        if (it->details) rank = {UINT64_MAX, 0};
        else if (const auto wanted = wanted_.find(it->key); wanted != wanted_.end()) rank = wanted->second;
        if (pick == queue_.end() || rank.first > best.first ||
            (rank.first == best.first && rank.second < best.second)) {
            pick = it;
            best = rank;
        }
    }
    if (pick == queue_.end()) return false;
    slow = IsSlowRequest(*pick);
    // Drawn by a frame (prefetch ranks 0) and quick: worth a priority step.
    pick->urgent = boost_hosts_ && !slow && (pick->details || best.first > 0);
    if (slow) ++slow_in_flight_;
    if (pick->flags & ipc::kPreviewRequestFlagPsdRefine) ++psd_refinements_in_flight_;
    wanted_.erase(pick->key);
    out = std::move(*pick);
    queue_.erase(pick);
    return true;
}

// Over the queue limit: drop what the oldest frame asked for (grid order) or
// the back of the queue (newest-first order).
void ThumbnailCache::DropOverflowLocked() {
    while (queue_.size() > 128) {
        auto victim = std::prev(queue_.end());
        if (visible_order_) {
            std::pair<uint64_t, uint64_t> worst{UINT64_MAX, 0};
            for (auto it = queue_.begin(); it != queue_.end(); ++it) {
                if (it->details) continue;
                const auto wanted = wanted_.find(it->key);
                const std::pair<uint64_t, uint64_t> rank = wanted == wanted_.end()
                    ? std::pair<uint64_t, uint64_t>{0, UINT64_MAX} : wanted->second;
                if (rank.first < worst.first || (rank.first == worst.first && rank.second > worst.second)) {
                    worst = rank;
                    victim = it;
                }
            }
        }
        pending_.erase(victim->key);
        wanted_.erase(victim->key);
        queue_.erase(victim);
    }
}

ThumbnailCache::~ThumbnailCache() { Reset(); }
void ThumbnailCache::SetDeviceContext(ID2D1DeviceContext* dc) {
    // Another window's context on the same device keeps every bitmap.
    std::lock_guard lock(mutex_);
    if (!device_binding_.Bind(dc)) return;
    dc_ = device_binding_.Dc();
    ++device_generation_;
    for (auto& [_, item] : items_) item.bitmap.reset();
}
std::wstring ThumbnailCache::Key(const std::wstring& path, uint32_t pixels,
                                 uint64_t modified, uint64_t size,
                                 uint32_t frame_index) const {
    return path + L"\n" + std::to_wstring(pixels) + L":" + std::to_wstring(modified) +
           L":" + std::to_wstring(size) + L":" + std::to_wstring(frame_index);
}
void ThumbnailCache::StopChildren() {
    for (uint32_t lane = 0; lane < kMaxLanes; ++lane) StopChild(lane);
}
void ThumbnailCache::StopChild(uint32_t lane) {
    LaneRef io = LaneAt(lane);
    HANDLE& lane_pipe = io.pipe;
    PROCESS_INFORMATION& lane_child = io.child;
    if (lane_pipe != INVALID_HANDLE_VALUE) { CancelIoEx(lane_pipe, nullptr); CloseHandle(lane_pipe); lane_pipe = INVALID_HANDLE_VALUE; }
    if (lane_child.hProcess) {
        TerminateProcess(lane_child.hProcess, 0);
        // Give asynchronous termination a bounded chance to release resources.
        // SMB I/O may outlive this wait; replacements use their own pipe name.
        WaitForSingleObject(lane_child.hProcess, 2000);
        CloseHandle(lane_child.hProcess); CloseHandle(lane_child.hThread); lane_child = {};
    }
}
void ThumbnailCache::Reset() {
    // Pair the predicate change with Worker wait registration under the same lock.
    { std::lock_guard lock(mutex_); running_ = false; }
    cv_.notify_all();
    if (worker_.joinable()) CancelSynchronousIo(worker_.native_handle());
    for (auto& lane : extra_lanes_)
        if (lane.worker.joinable()) CancelSynchronousIo(lane.worker.native_handle());
    if (worker_.joinable()) worker_.join();
    for (auto& lane : extra_lanes_)
        if (lane.worker.joinable()) lane.worker.join();
    StopChildren();
    std::lock_guard lock(mutex_); queue_.clear(); pending_.clear(); items_.clear(); lru_.clear(); folder_requests_.clear();
    wanted_.clear(); interests_.clear(); interest_window_ = nullptr; slow_in_flight_ = 0; psd_refinements_in_flight_ = 0;
    still_by_identity_.clear(); transient_failures_.clear(); retry_interest_.clear();
    cache_bytes_ = 0; device_binding_.Bind(nullptr); dc_ = nullptr; latest_details_identity_.clear();
    draw_retry_deadline_ = 0;
    epoch_.fetch_add(1, std::memory_order_relaxed);
}
void ThumbnailCache::BeginFrame() {
    std::lock_guard lock(mutex_);
    upload_elapsed_us_ = 0; upload_bytes_ = 0;
    if (shared_upload_budget_) { shared_upload_budget_->elapsed_us = 0; shared_upload_budget_->bytes = 0; }
    track_retry_interest_ = true;
    retry_interest_.clear();
}

uint64_t ThumbnailCache::WindowRetryDeadline(HWND window) {
    std::lock_guard lock(mutex_);
    const auto view = interests_.find(window);
    if (view == interests_.end()) return 0;
    uint64_t next = 0;
    for (const auto& key : view->second.visible) {
        const auto it = items_.find(key);
        if (it == items_.end() || pending_.contains(key)) continue;
        const auto& item = it->second;
        if (item.failed && item.transient && item.retry_at)
            next = next ? (std::min)(next, item.retry_at) : item.retry_at;
    }
    return next;
}

uint64_t ThumbnailCache::NextRetryDeadline() {
    std::lock_guard lock(mutex_);
    uint64_t next = 0;
    for (const auto& key : retry_interest_) {
        const auto it = items_.find(key);
        if (it == items_.end()) continue;
        const auto& item = it->second;
        if (item.failed && item.transient && item.retry_at && !pending_.contains(key))
            next = next ? (std::min)(next, item.retry_at) : item.retry_at;
    }
    return next;
}

bool ThumbnailCache::VolumeSuspendedLocked(const std::wstring& path) const {
    for (const auto& entry : suspended_volumes_)
        if (ThumbnailPathOnVolume(path, entry.first)) return true;
    return false;
}
std::shared_future<void> ThumbnailCache::SuspendVolume(const std::wstring& root) {
    std::lock_guard lock(mutex_);
    std::wstring normalized = root;
    for (auto& ch : normalized) ch = ch == L'/' ? L'\\' : static_cast<wchar_t>(towlower(ch));
    while (!normalized.empty() && normalized.back() == L'\\') normalized.pop_back();
    if (const auto it = suspended_volumes_.find(normalized); it != suspended_volumes_.end()) return it->second->future;
    auto release = std::make_shared<VolumeRelease>();
    suspended_volumes_[normalized] = release;
    for (uint32_t lane = 0; lane < kMaxLanes; ++lane) {
        if (!ThumbnailPathOnVolume(lane_paths_[lane], normalized)) continue;
        if (release_lanes_[lane]) {
            // A timed-out earlier attempt still owns this lane's release.
            release = release_lanes_[lane];
            suspended_volumes_[normalized] = release;
            break;
        }
    }
    for (uint32_t lane = 0; lane < kMaxLanes; ++lane) {
        if (ThumbnailPathOnVolume(lane_paths_[lane], normalized) && !release_lanes_[lane]) {
            release_lanes_[lane] = release;
            ++release->pending;
        }
    }
    for (auto it = queue_.begin(); it != queue_.end();) {
        if (ThumbnailPathOnVolume(it->path, normalized)) {
            pending_.erase(it->key);
            it = queue_.erase(it);
        } else ++it;
    }
    if (!release->pending) release->promise.set_value();
    cv_.notify_all();
    return release->future;
}
void ThumbnailCache::ResumeVolume(const std::wstring& root) {
    std::lock_guard lock(mutex_);
    std::wstring normalized = root;
    for (auto& ch : normalized) ch = ch == L'/' ? L'\\' : static_cast<wchar_t>(towlower(ch));
    while (!normalized.empty() && normalized.back() == L'\\') normalized.pop_back();
    suspended_volumes_.erase(normalized);
}

void ThumbnailCache::Evict() {
    epoch_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock(mutex_);
    queue_.clear(); pending_.clear(); items_.clear(); lru_.clear(); folder_requests_.clear();
    still_by_identity_.clear(); transient_failures_.clear(); retry_interest_.clear();
    wanted_.clear(); interests_.clear(); interest_window_ = nullptr;
    cache_bytes_ = 0;
    latest_details_identity_.clear();
    draw_retry_deadline_ = 0;
}

bool ThumbnailCache::Palette(const std::wstring& path, uint32_t pixel_size, uint64_t modified,
                             uint64_t size, CoverPalette& palette) {
    std::lock_guard lock(mutex_);
    auto it = items_.find(Key(path, pixel_size, modified, size, 0));
    if (it == items_.end() || it->second.failed) {
        const auto link = still_by_identity_.find(Key(path, 0, modified, size));
        it = link == still_by_identity_.end() ? items_.end() : items_.find(link->second);
    }
    if (it == items_.end() || it->second.failed || !it->second.palette.valid) return false;
    palette = it->second.palette;
    return true;
}

bool ThumbnailCache::Stats(const std::wstring& path, uint32_t pixel_size, uint64_t modified,
                           uint64_t size, ImageStats& stats) {
    std::lock_guard lock(mutex_);
    auto it = items_.find(Key(path, pixel_size, modified, size, 0));
    if (it == items_.end() || it->second.failed) {
        const auto link = still_by_identity_.find(Key(path, 0, modified, size));
        it = link == still_by_identity_.end() ? items_.end() : items_.find(link->second);
    }
    if (it == items_.end() || it->second.failed || !it->second.stats.valid) return false;
    stats = it->second.stats;
    return true;
}

void ThumbnailCache::PrepareBitmap(const std::wstring& key, bool budgeted) {
    std::vector<uint8_t> pixels;
    ID2D1DeviceContext* context = nullptr;
    uint32_t w = 0, h = 0, stride = 0;
    uint64_t token = 0, device = 0, epoch = 0;
    {
        std::lock_guard lock(mutex_);
        const auto it = items_.find(key);
        if (it == items_.end()) return;
        auto& item = it->second;
        if (item.bitmap.get() || item.failed || item.pixels.empty() || item.upload_token || !dc_) return;
        const auto elapsed = shared_upload_budget_ ? shared_upload_budget_->elapsed_us.load() : upload_elapsed_us_;
        const auto bytes = shared_upload_budget_ ? shared_upload_budget_->bytes.load() : upload_bytes_;
        if (budgeted && (elapsed >= 3000 || (bytes &&
            item.pixels.size() > 4194304 - (std::min)(bytes, size_t(4194304))))) {
            upload_frame_pending_ = true;
            const auto deadline = GetTickCount64() + 16;
            if (!draw_retry_deadline_ || deadline < draw_retry_deadline_) draw_retry_deadline_ = deadline;
            return;
        }
        token = item.upload_token = ++upload_sequence_;
        device = device_generation_;
        epoch = epoch_.load(std::memory_order_relaxed);
        w = item.w; h = item.h; stride = item.stride;
        pixels = std::move(item.pixels);
        context = dc_; context->AddRef();
        if (budgeted) {
            upload_bytes_ += pixels.size();
            if (shared_upload_budget_) shared_upload_budget_->bytes += pixels.size();
        }
    }
    ComPtr<ID2D1Bitmap> bitmap;
    const auto started = std::chrono::steady_clock::now();
    if (upload_probe_) upload_probe_(*this);
    const bool uploaded = UploadBitmap(context, bitmap, pixels, w, h, stride);
    context->Release();
    {
        std::lock_guard lock(mutex_);
        if (budgeted) {
            const auto elapsed = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started).count());
            upload_elapsed_us_ += elapsed;
            if (shared_upload_budget_) shared_upload_budget_->elapsed_us += elapsed;
        }
        const auto it = items_.find(key);
        if (it == items_.end() || it->second.upload_token != token) return;
        auto& item = it->second;
        item.upload_token = 0;
        item.upload_failed = !uploaded;
        if (uploaded && device_generation_ == device && epoch_.load(std::memory_order_relaxed) == epoch) {
            item.bitmap = std::move(bitmap);
            if (folder_thumbnail_) item.pixels = std::move(pixels);
        }
        else {
            item.pixels = std::move(pixels);
            const auto deadline = GetTickCount64() + 16;
            if (!draw_retry_deadline_ || deadline < draw_retry_deadline_) draw_retry_deadline_ = deadline;
        }
    }
}

ThumbnailCache::Item* ThumbnailCache::StaleBitmap(const std::wstring& identity,
                                                  const std::wstring& except_key) {
    const auto link = still_by_identity_.find(identity);
    if (link == still_by_identity_.end()) return nullptr;
    if (link->second == except_key) return nullptr;
    const auto it = items_.find(link->second);
    if (it == items_.end() || it->second.failed) {
        still_by_identity_.erase(link);
        return nullptr;
    }
    Item& item = it->second;
    if (!item.bitmap.get()) return nullptr;
    Touch(item);
    return &item;
}
// Stage values are stable diagnostics identifiers: launch=1, connect=2, send=3,
// wait=4, timeout=5, exited=6, response=7, mapping-open=8, mapping-view=9,
// ack=10, host-status=11, store=12, cancelled=13.
void ThumbnailCache::LogFailure(uint32_t lane, const Request& req, uint64_t started, uint32_t stage,
                                DWORD error, bool cancelled) {
    LaneRef io = LaneAt(lane);
    const PROCESS_INFORMATION& lane_child = io.child;
    const uint32_t lane_token = io.token;
    {
        std::lock_guard lock(mutex_);
        cancelled = cancelled || req.epoch != epoch_.load(std::memory_order_relaxed) ||
            (req.details && req.identity != latest_details_identity_) ||
            (req.document && !req.document->active.load());
    }
    DWORD exit_code = 0;
    const bool exit_known = lane_child.hProcess && GetExitCodeProcess(lane_child.hProcess, &exit_code);
    diagnostics::runtime::TextEvent(cancelled ? "preview_client_cancelled" : "preview_client_failure",
        {{"extension", preview::DiagnosticExtension(req.path)}}, {
        {"stage", stage}, {"error", error}, {"elapsed_ms", GetTickCount64() - started},
        {"request", req.id}, {"generation", req.generation}, {"host_pid", lane_child.dwProcessId},
        {"exit_known", exit_known}, {"exit_code", exit_code}, {"cache", lane_token},
        {"kind", static_cast<uint32_t>(req.kind)}, {"flags", req.flags},
        {"path_id", preview::DiagnosticPathId(req.path)}},
        cancelled ? diagnostics::runtime::Level::Info : diagnostics::runtime::Level::Error);
}
bool ThumbnailCache::Connect(uint32_t lane, const Request& req, uint64_t started) {
    LaneRef io = LaneAt(lane);
    HANDLE& lane_pipe = io.pipe;
    PROCESS_INFORMATION& lane_child = io.child;
    const uint32_t lane_token = io.token;
    if (lane_pipe != INVALID_HANDLE_VALUE) return true;
    wchar_t exe[MAX_PATH]{};
    const auto module_address = +[] {};
    const DWORD location_error = module_location::DirectoryFromAddress(reinterpret_cast<const void*>(module_address),
        exe, ARRAYSIZE(exe));
    if (location_error != ERROR_SUCCESS) { LogFailure(lane, req, started, 1, location_error); return false; }
    std::wstring cmd = L"\"" + std::wstring(exe) + L"Pulse.Preview.exe\" " +
        std::to_wstring(lane_token) + L" --process-pipe";
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_FORCEOFFFEEDBACK; // background helper: no AppStarting cursor
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | (folder_thumbnail_ ? BELOW_NORMAL_PRIORITY_CLASS : 0),
                        nullptr, nullptr, &si, &lane_child)) {
        const DWORD error = GetLastError();
        LogFailure(lane, req, started, 1, error, !running_);
        return false;
    }
    // TerminateProcess can outlive StopChild's bounded wait while SMB I/O
    // drains. A replacement must never reuse that host's pipe instance.
    const std::wstring pipeName = ipc::PreviewPipeName(lane_token, lane_child.dwProcessId);
    const ULONGLONG deadline = GetTickCount64() + 3000;
    DWORD connect_error = ERROR_SUCCESS;
    do {
        lane_pipe = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr);
        if (lane_pipe != INVALID_HANDLE_VALUE) {
            ULONG server_pid = 0;
            const bool got_pid = GetNamedPipeServerProcessId(lane_pipe, &server_pid) != FALSE;
            const DWORD error = got_pid ? ERROR_ACCESS_DENIED : GetLastError();
            if (got_pid && server_pid == lane_child.dwProcessId) return true;
            LogFailure(lane, req, started, 2, error, !running_);
            CloseHandle(lane_pipe);
            lane_pipe = INVALID_HANDLE_VALUE;
            StopChild(lane);
            return false;
        }
        connect_error = GetLastError();
        if (WaitForSingleObject(lane_child.hProcess, 0) == WAIT_OBJECT_0) break;
        Sleep(25);
    } while (running_ && GetTickCount64() < deadline);
    LogFailure(lane, req, started, 2, connect_error, !running_);
    StopChild(lane); return false;
}
PreviewDrawResult ThumbnailCache::Draw(ID2D1DeviceContext* dc, const D2D1_RECT_F& dest,
                                       const std::wstring& path, DWORD attrs, uint32_t pixels,
                                       uint64_t generation, uint64_t modified, uint64_t size,
                                       float opacity, std::wstring* text,
                                       bool* truncated, uint32_t* bytes_read,
                                       bool direct_preview, std::wstring* error,
                                       float* pan_x, float* pan_y,
                                       float* pan_max_x, float* pan_max_y,
                                       uint32_t frame_index, uint32_t* frame_count,
                                       uint32_t* frame_delay_ms, uint32_t* loop_count,
                                       uint32_t* decoded_width, uint32_t* decoded_height,
                                       uint32_t* source_width, uint32_t* source_height,
                                       PreviewViewport* viewport,
                                       uint32_t* text_encoding,
                                       bool align_artwork_bottom, D2D1_RECT_F* artwork_rect,
                                       uint32_t* media_duration_ms, preview::Integrity* integrity,
                                       const std::shared_ptr<DocumentImageSession>& document,
                                       std::shared_ptr<const std::wstring>* shared_text) {
    if (shared_text) shared_text->reset();
    if (integrity) { *integrity = {}; integrity->state = preview::IntegrityState::Loading; }
    if (artwork_rect) *artwork_rect = dest;
    if (media_duration_ms) *media_duration_ms = 0;
    if (!dc || path.empty() || pixels < 24) return PreviewDrawResult::Failed;
    if (document && !document->active.load()) return PreviewDrawResult::Failed;
    std::wstring document_key = document ? L":document:" + std::to_wstring(document->identity) : L"";
    // A placeholder's answer (Explorer's cached thumbnail, or nothing) is not
    // the file's: once it is downloaded the attributes change and so does the
    // key, so the real preview is fetched instead of the cached refusal.
    if (pulse::fs::IsCloudPlaceholder(attrs)) document_key += L":cloud";
    const std::wstring key = Key(path, pixels, modified, size, frame_index) + document_key;
    PrepareBitmap(key, !direct_preview);
    if (!viewport && !(pan_x && pan_y) && frame_index == 0) {
        std::wstring stale_key;
        {
            std::lock_guard lock(mutex_);
            const auto link = still_by_identity_.find(Key(path, 0, modified, size) + document_key);
            if (link != still_by_identity_.end() && link->second != key) stale_key = link->second;
        }
        if (!stale_key.empty()) PrepareBitmap(stale_key, !direct_preview);
    }
    {
        std::lock_guard lock(mutex_);
        if (VolumeSuspendedLocked(path)) return PreviewDrawResult::Failed;
        if (interest_window_ && !direct_preview) {
            auto& interest = interests_[interest_window_];
            interest.drawing.insert(key); interest.drawing_visible.insert(key);
        }
        if (track_retry_interest_) retry_interest_.insert(key);
        const std::wstring identity = Key(path, 0, modified, size) + document_key;
        if (direct_preview) SelectDetailsLocked(identity);
        const bool visible_order = visible_order_ && !direct_preview;
        auto queue_request = [&](bool psd_refine = false) {
            if (visible_order) wanted_[key] = {frame_, ++draw_sequence_};
            if (pending_.contains(key)) return;
            for (auto old = queue_.begin(); old != queue_.end();) {
                if (old->document && !old->document->active.load()) {
                    pending_.erase(old->key);
                    old = queue_.erase(old);
                } else ++old;
            }
            const auto dot = path.find_last_of(L'.');
            const bool spreadsheet = direct_preview && dot != std::wstring::npos &&
                (_wcsicmp(path.c_str() + dot, L".xlsx") == 0 || _wcsicmp(path.c_str() + dot, L".xlsm") == 0);
            if (spreadsheet) {
                // A rapid sheet switch replaces queued work for this workbook.
                // The active worker can finish, but its old result is only cached.
                for (auto it = queue_.begin(); it != queue_.end();) {
                    if (it->details && it->identity == identity && it->frame_index != frame_index) {
                        pending_.erase(it->key);
                        it = queue_.erase(it);
                    } else ++it;
                }
            }
            pending_.insert(key);
            Request request;
            request.id = next_id_++; request.generation = generation;
            request.pixels = pixels; request.attrs = attrs;
            request.kind = ipc::PreviewRequestKind::Content;
            if (psd_refine) request.flags |= ipc::kPreviewRequestFlagPsdRefine;
            request.document = document;
            if (document) request.flags |= ipc::kPreviewRequestFlagDocumentImage;
            request.interest_tracked = interest_window_ && !direct_preview;
            request.details = direct_preview; request.frame_index = frame_index;
            request.epoch = epoch_.load(std::memory_order_relaxed);
            request.timeout_ms = ResponseTimeoutMs(path, request.kind, direct_preview ? 0 : size);
            request.path = path; request.key = key; request.identity = identity;
            request.modified = modified; request.size = size;
            if (visible_order) queue_.push_back(std::move(request));
            else queue_.push_front(std::move(request));
            DropOverflowLocked();
            StartWorkersLocked();
            cv_.notify_one();
        };
        auto it = items_.find(key);
        if (it != items_.end()) {
            Item& item = it->second;
            if (!direct_preview && item.needs_psd_refine) queue_request(true);
            const auto output_text = [&] {
                // Promotion transfers the buffer once; holders survive cache eviction.
                if (shared_text) {
                    if (!item.shared_text && !item.text.empty())
                        item.shared_text = std::make_shared<const std::wstring>(std::move(item.text));
                    *shared_text = item.shared_text;
                }
                if (text) *text = item.shared_text ? *item.shared_text : item.text;
            };
            Touch(item);
            if (truncated) *truncated = item.truncated;
            if (bytes_read) *bytes_read = item.bytes_read;
            if (error) *error = item.error;
            if (integrity) *integrity = item.integrity;
            if (frame_count) *frame_count = item.frame_count;
            if (frame_delay_ms) *frame_delay_ms = item.frame_delay_ms;
            if (loop_count) *loop_count = item.loop_count;
            if (text_encoding) *text_encoding = item.text_encoding;
            if (decoded_width) *decoded_width = item.w;
            if (decoded_height) *decoded_height = item.h;
            if (source_width) *source_width = item.source_width;
            if (source_height) *source_height = item.source_height;
            if (media_duration_ms) *media_duration_ms = item.duration_ms;
            if (item.kind == ipc::PreviewContentKind::Text ||
                item.kind == ipc::PreviewContentKind::Hex ||
                item.kind == ipc::PreviewContentKind::Archive ||
                item.kind == ipc::PreviewContentKind::Markdown ||
                item.kind == ipc::PreviewContentKind::Table ||
                item.kind == ipc::PreviewContentKind::Tree) {
                output_text();
                if (item.kind == ipc::PreviewContentKind::Tree) return PreviewDrawResult::Tree;
                if (item.kind == ipc::PreviewContentKind::Table) return PreviewDrawResult::Table;
                if (item.kind == ipc::PreviewContentKind::Archive) return PreviewDrawResult::Archive;
                if (item.kind == ipc::PreviewContentKind::Markdown) return PreviewDrawResult::Markdown;
                return item.kind == ipc::PreviewContentKind::Hex
                    ? PreviewDrawResult::Hex : PreviewDrawResult::Text;
            }

            if (item.kind == ipc::PreviewContentKind::Bitmap) output_text();  // e.g. icon sizes
            if (item.bitmap.get()) {
                if (viewport) {
                    viewport->SetContent(dest, static_cast<float>(item.source_width ? item.source_width : item.w),
                        static_cast<float>(item.source_height ? item.source_height : item.h), true);
                    const auto target = viewport->ContentRect();
                    if (artwork_rect) *artwork_rect = ThumbnailArtworkRect(target, item.artwork_bounds);
                    dc->PushAxisAlignedClip(dest, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                    dc->DrawBitmap(item.bitmap.get(), &target, std::clamp(opacity, 0.0f, 1.0f),
                        D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
                    dc->PopAxisAlignedClip();
                    return PreviewDrawResult::Bitmap;
                }
                const float destW = std::max(1.0f, dest.right - dest.left);
                const float destH = std::max(1.0f, dest.bottom - dest.top);
                if (pan_x && pan_y) {
                    // Cover: fill the dest rect and crop the overflow; pan is a
                    // DIP offset into the cropped region. Bitmaps smaller than
                    // the dest stay at scale<=1 (centered, nothing to pan).
                    float cover = std::max(destW / static_cast<float>(item.w),
                                           destH / static_cast<float>(item.h));
                    cover = std::min(cover, 1.0f);
                    const float drawW = item.w * cover;
                    const float drawH = item.h * cover;
                    const float maxPanX = std::max(0.0f, drawW - destW);
                    const float maxPanY = std::max(0.0f, drawH - destH);
                    if (pan_max_x) *pan_max_x = maxPanX;
                    if (pan_max_y) *pan_max_y = maxPanY;
                    *pan_x = std::clamp(*pan_x, 0.0f, maxPanX);
                    *pan_y = std::clamp(*pan_y, 0.0f, maxPanY);
                    const D2D1_RECT_F fitted = D2D1::RectF(
                        dest.left + std::max(0.0f, destW - drawW) * 0.5f - *pan_x,
                        dest.top + std::max(0.0f, destH - drawH) * 0.5f - *pan_y,
                        dest.left + std::max(0.0f, destW - drawW) * 0.5f - *pan_x + drawW,
                        dest.top + std::max(0.0f, destH - drawH) * 0.5f - *pan_y + drawH);
                    dc->PushAxisAlignedClip(dest, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                    dc->DrawBitmap(item.bitmap.get(), &fitted,
                        std::clamp(opacity, 0.0f, 1.0f),
                        D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
                    dc->PopAxisAlignedClip();
                    if (artwork_rect) *artwork_rect = ThumbnailArtworkRect(fitted, item.artwork_bounds);
                    return PreviewDrawResult::Bitmap;
                }
                DrawContained(dc, item.bitmap.get(), item.w, item.h, dest, opacity,
                    item.artwork_bounds, align_artwork_bottom, artwork_rect);
                return PreviewDrawResult::Bitmap;
            }
            if (item.failed) {
                if (item.transient && GetTickCount64() >= item.retry_at) queue_request();
                if (item.transient && !pending_.contains(key) &&
                    (!draw_retry_deadline_ || item.retry_at < draw_retry_deadline_))
                    draw_retry_deadline_ = item.retry_at;
                if (!viewport && !(pan_x && pan_y) && frame_index == 0) {
                    if (Item* stale = StaleBitmap(identity, key)) {
                        if (media_duration_ms) *media_duration_ms = stale->duration_ms;
                        if (decoded_width) *decoded_width = stale->w;
                        if (decoded_height) *decoded_height = stale->h;
                        if (source_width) *source_width = stale->source_width;
                        if (source_height) *source_height = stale->source_height;
                        DrawContained(dc, stale->bitmap.get(), stale->w, stale->h, dest, opacity,
                            stale->artwork_bounds, align_artwork_bottom, artwork_rect);
                        return PreviewDrawResult::Bitmap;
                    }
                }
                return item.transient ? PreviewDrawResult::Pending : PreviewDrawResult::Failed;
            }
        }
        const auto ready = items_.find(key);
        if (ready == items_.end() || (ready->second.pixels.empty() && !ready->second.upload_token)) queue_request();
        else {
            if (!ready->second.upload_failed) upload_frame_pending_ = true;
            const auto deadline = GetTickCount64() + 16;
            if (!draw_retry_deadline_ || deadline < draw_retry_deadline_) draw_retry_deadline_ = deadline;
        }
        // Another size of the same file (view switch, DPI change) or the
        // previous decode stays on screen until this one arrives.
        if (!viewport && !(pan_x && pan_y) && frame_index == 0) {
            if (Item* stale = StaleBitmap(identity, key)) {
                if (media_duration_ms) *media_duration_ms = stale->duration_ms;
                if (decoded_width) *decoded_width = stale->w;
                if (decoded_height) *decoded_height = stale->h;
                if (source_width) *source_width = stale->source_width;
                if (source_height) *source_height = stale->source_height;
                DrawContained(dc, stale->bitmap.get(), stale->w, stale->h, dest, opacity,
                    stale->artwork_bounds, align_artwork_bottom, artwork_rect);
                return PreviewDrawResult::Bitmap;
            }
        }
    }
    return PreviewDrawResult::Pending;
}

PreviewDrawResult ThumbnailCache::DrawCachedGridThumbnail(ID2D1DeviceContext* dc,
    const D2D1_RECT_F& dest, const std::wstring& path, DWORD attrs, uint32_t pixel_size,
    uint64_t modified, uint64_t size, float opacity, bool align_bottom, D2D1_RECT_F* artwork) {
    if (artwork) *artwork = dest;
    if (!dc || path.empty()) return PreviewDrawResult::Failed;
    const std::wstring suffix = pulse::fs::IsCloudPlaceholder(attrs) ? L":cloud" : L"";
    const auto key = Key(path, pixel_size, modified, size) + suffix;
    std::lock_guard lock(mutex_);
    const auto found = items_.find(key);
    Item* item = found != items_.end() && found->second.bitmap.get() && !found->second.failed
        ? &found->second : StaleBitmap(Key(path, 0, modified, size) + suffix, key);
    if (!item) return PreviewDrawResult::Pending;
    Touch(*item);
    DrawContained(dc, item->bitmap.get(), item->w, item->h, dest, opacity,
        item->artwork_bounds, align_bottom, artwork);
    return PreviewDrawResult::Bitmap;
}

PreviewDrawResult ThumbnailCache::DrawGridThumbnail(ID2D1DeviceContext* dc,
    const D2D1_RECT_F& dest, const std::wstring& path, DWORD attrs, uint32_t pixel_size,
    uint64_t generation, uint64_t modified, uint64_t size, float opacity,
    bool align_bottom, D2D1_RECT_F* artwork, uint32_t* media_duration_ms) {
    return Draw(dc, dest, path, attrs, pixel_size, generation, modified, size,
        opacity, nullptr, nullptr, nullptr, false, nullptr,
        nullptr, nullptr, nullptr, nullptr, 0, nullptr, nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, align_bottom, artwork,
        media_duration_ms);
}

PreviewDrawResult ThumbnailCache::DrawWallThumbnail(ID2D1DeviceContext* dc,
    const D2D1_RECT_F& dest, const std::wstring& path, DWORD attrs, uint32_t pixel_size,
    uint64_t generation, uint64_t modified, uint64_t size, float opacity, float* aspect,
    uint32_t* media_duration_ms) {
    uint32_t w = 0, h = 0;
    t_cover_fill = true;
    const PreviewDrawResult result = Draw(dc, dest, path, attrs, pixel_size, generation, modified, size,
        opacity, nullptr, nullptr, nullptr, false, nullptr,
        nullptr, nullptr, nullptr, nullptr, 0, nullptr, nullptr, nullptr,
        &w, &h, nullptr, nullptr, nullptr, nullptr, false, nullptr, media_duration_ms);
    t_cover_fill = false;
    if (aspect) *aspect = result == PreviewDrawResult::Bitmap && w > 0 && h > 0
        ? static_cast<float>(w) / static_cast<float>(h) : 0.0f;
    return result;
}

void ThumbnailCache::SelectDetailsLocked(const std::wstring& identity) {
    if (identity == latest_details_identity_) return;
    latest_details_identity_ = identity;
    for (auto queued = queue_.begin(); queued != queue_.end();) {
        if (queued->details) {
            pending_.erase(queued->key);
            queued = queue_.erase(queued);
        } else {
            ++queued;
        }
    }
}

bool ThumbnailCache::Properties(const std::wstring& path, DWORD attrs, uint64_t generation,
                                uint64_t modified, uint64_t size,
                                std::vector<PreviewProperty>& properties) {
    if (path.empty()) return false;
    const std::wstring identity = Key(path, 0, modified, size) +
        (pulse::fs::IsCloudPlaceholder(attrs) ? L":cloud" : L"");
    const std::wstring key = identity + L":properties";
    std::lock_guard lock(mutex_);
    if (VolumeSuspendedLocked(path)) return false;
    SelectDetailsLocked(identity);
    if (auto it = items_.find(key); it != items_.end()) {
        Touch(it->second);
        properties = it->second.properties;
        if (!it->second.failed) return true;
        if (!it->second.transient || GetTickCount64() < it->second.retry_at) return false;
    }
    if (!pending_.contains(key) && queue_.size() < 128) {
        pending_.insert(key);
        Request request;
        request.id = next_id_++; request.generation = generation;
        request.attrs = attrs; request.kind = ipc::PreviewRequestKind::Properties;
        request.details = true; request.path = path; request.key = key;
        request.identity = identity;
        request.timeout_ms = ResponseTimeoutMs(path, request.kind);
        request.epoch = epoch_.load(std::memory_order_relaxed);
        queue_.push_back(std::move(request));
        StartWorkersLocked();
        cv_.notify_one();
    }
    return false;
}
bool ThumbnailCache::CachedProperties(const std::wstring& path, uint64_t modified,
                                      uint64_t size,
                                      std::vector<PreviewProperty>& properties) {
    if (path.empty()) return false;
    const std::wstring key = Key(path, 0, modified, size) + L":properties";
    std::lock_guard lock(mutex_);
    const auto it = items_.find(key);
    if (it == items_.end() || it->second.failed) return false;
    Touch(it->second);
    properties = it->second.properties;
    return true;
}
void ThumbnailCache::Touch(Item& item) {
    lru_.splice(lru_.end(), lru_, item.lru_position);
}

void ThumbnailCache::QueuePsdRefinement(const Request& request) {
    std::lock_guard lock(mutex_);
    if (!running_ || request.epoch != epoch_.load(std::memory_order_relaxed) ||
        VolumeSuspendedLocked(request.path) || (request.interest_tracked && !WantedByWindowLocked(request.key)) ||
        !items_.contains(request.key) || pending_.contains(request.key)) return;
    Request next = request;
    next.flags |= ipc::kPreviewRequestFlagPsdRefine;
    next.urgent = false;
    next.budget_deferrals = 0;
    next.queued_at = GetTickCount64();
    pending_.insert(next.key);
    queue_.push_back(std::move(next));
    cv_.notify_one();
}

bool ThumbnailCache::StoreResult(const Request& req, Item result) {
    std::lock_guard lock(mutex_);
    // An old decode must not remove a replacement request queued after Evict().
    if (req.epoch != epoch_.load(std::memory_order_relaxed)) return false;
    if (req.interest_tracked && !WantedByWindowLocked(req.key)) { pending_.erase(req.key); return false; }
    if (req.document && !req.document->active.load()) {
        pending_.erase(req.key);
        return false;
    }
    if (req.flags & ipc::kPreviewRequestFlagFolderThumbnail) {
        const auto current = folder_requests_.find(req.key);
        if (current == folder_requests_.end() || current->second != req.id) return false;
        folder_requests_.erase(current);
        result.folder_refresh_complete = (req.flags & ipc::kPreviewRequestFlagFolderRefresh) &&
            (!result.failed || !result.transient);
        if (result.failed && result.error.empty())
            result.error = result.transient ? L"folder-transport-failure" : L"folder-host-rejected";
        // Only transport failures retain old artwork. An authoritative empty or
        // unsupported result must restore the folder icon, including stale sizes.
        if (result.failed && !result.transient) {
            still_by_identity_.erase(req.identity);
            const auto prefix = req.identity + L"\n";
            for (auto old = items_.begin(); old != items_.end();) {
                if (old->first != req.key && old->first.starts_with(prefix)) {
                    cache_bytes_ -= old->second.cost;
                    lru_.erase(old->second.lru_position);
                    transient_failures_.erase(old->first);
                    old = items_.erase(old);
                } else ++old;
            }
        }
        if (result.failed && result.transient) {
            const auto old = items_.find(req.key);
            if (old != items_.end() && !old->second.failed) {
                old->second.retry_at = GetTickCount64() + 30000;
                old->second.error = result.error;
                old->second.folder_refresh_complete = false;
                pending_.erase(req.key);
                return true;
            }
        }
        if (result.failed && result.transient) {
            auto& attempts = transient_failures_[req.key];
            attempts = std::min(attempts + 1, 4u);
            constexpr uint32_t retry_ms[] = {500, 2000, 5000, 30000};
            result.retry_at = GetTickCount64() + retry_ms[attempts - 1];
        } else {
            transient_failures_.erase(req.key);
            result.retry_at = GetTickCount64() + 30000;
        }
        result.transient = false;
        result.cost *= 2; // CPU pixels retained for device recovery plus GPU bitmap.
    }
    pending_.erase(req.key);
    if (req.flags & ipc::kPreviewRequestFlagPsdRefine) {
        if (const auto old = items_.find(req.key); old != items_.end()) old->second.needs_psd_refine = false;
        if (result.failed || result.transient || result.kind != ipc::PreviewContentKind::Bitmap)
            return false;
    }
    if (req.details && req.identity != latest_details_identity_) return false;

    const bool resource_limited = result.error == L"gif-thumbnail-budget" || result.error == L"gif-memory-budget" || result.error == L"decode-memory-budget";
    if (result.failed && resource_limited) {
        result.transient = true;
        result.retry_at = GetTickCount64() + 15000;
        transient_failures_.erase(req.key);
    } else if (result.failed && !result.transient &&
        req.kind == ipc::PreviewRequestKind::Content && req.frame_index == 0 &&
        !(req.attrs & FILE_ATTRIBUTE_DIRECTORY) &&
        !(req.flags & ipc::kPreviewRequestFlagFolderThumbnail) &&
        [&] {
            std::wstring extension(preview::DiagnosticExtension(req.path));
            std::transform(extension.begin(), extension.end(), extension.begin(),
                [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            return preview::IsImageExtension(extension);
        }()) {
        // A decoder can reject an image while its writer is still downloading.
        // Metadata may not change until close, so do not permanently cache this
        // refusal. Visible-interest timers retry without probing files on the UI.
        auto& attempts = transient_failures_[req.key];
        attempts = (std::min)(attempts + 1, 4u);
        constexpr uint32_t retry_ms[] = {1000, 4000, 15000, 30000};
        result.transient = true;
        result.retry_at = GetTickCount64() + retry_ms[attempts - 1];
    } else if (result.failed && result.transient) {
        const uint32_t attempts = ++transient_failures_[req.key];
        if (attempts > std::size(kTransientRetryMs)) {
            result.transient = false;
            transient_failures_.erase(req.key);
        } else {
            result.retry_at = GetTickCount64() + kTransientRetryMs[attempts - 1];
        }
    } else {
        if (!(req.flags & ipc::kPreviewRequestFlagFolderThumbnail)) transient_failures_.erase(req.key);
        // Paged documents (frame_count > 1, zero delay) keep page 1 as the
        // still, like a single-frame image.
        if (!result.failed && result.kind == ipc::PreviewContentKind::Bitmap &&
            ((!req.details && req.frame_index == 0) || result.frame_count <= 1 ||
             (result.frame_delay_ms == 0 && req.frame_index == 0)) &&
            !req.identity.empty())
            still_by_identity_[req.identity] = req.key;
    }

    if (auto old = items_.find(req.key); old != items_.end()) {
        cache_bytes_ -= old->second.cost;
        lru_.erase(old->second.lru_position);
        items_.erase(old);
    }
    // Animation frames of one file are capped at four; pages of a paged
    // document (zero delay) are ordinary LRU entries so every visible page and
    // strip thumbnail can stay resident together.
    if (req.details && result.frame_count > 1 && result.frame_delay_ms > 0) {
        size_t frames = 0;
        for (const auto& [key, item] : items_) {
            if (item.frame_count > 1 && item.animation_identity == req.identity) ++frames;
        }
        if (frames >= 4) {
            for (auto lru = lru_.begin(); lru != lru_.end(); ++lru) {
                auto cached = items_.find(*lru);
                if (cached->second.frame_count > 1 &&
                    cached->second.animation_identity == req.identity) {
                    cache_bytes_ -= cached->second.cost;
                    transient_failures_.erase(cached->first);
                    items_.erase(cached);
                    lru_.erase(lru);
                    break;
                }
            }
        }
    }
    cache_bytes_ += result.cost;
    lru_.push_back(req.key);
    result.lru_position = std::prev(lru_.end());
    items_.emplace(req.key, std::move(result));
    while (!lru_.empty() && (lru_.size() > max_items_ || cache_bytes_ > budget_bytes_)) {
        auto victim = lru_.begin();
        if (req.flags & ipc::kPreviewRequestFlagFolderThumbnail) {
            // Negative entries must not evict useful folder artwork. This also
            // discards a new negative when all existing slots hold bitmaps.
            const auto negative = std::find_if(lru_.begin(), lru_.end(), [&](const auto& key) {
                return items_.at(key).failed;
            });
            if (negative != lru_.end()) victim = negative;
        }
        auto oldest = items_.find(*victim);
        cache_bytes_ -= oldest->second.cost;
        // Retry history belongs to the cached failure, not every path ever visited.
        transient_failures_.erase(oldest->first);
        items_.erase(oldest);
        lru_.erase(victim);
    }
    if (still_by_identity_.size() > max_items_ * 2) {
        for (auto link = still_by_identity_.begin(); link != still_by_identity_.end();) {
            if (items_.contains(link->second)) ++link;
            else link = still_by_identity_.erase(link);
        }
    }
    return true;
}

bool ThumbnailCache::IsTransientResponse(bool transport_ok, const Request& request, uint32_t status) {
    return !transport_ok || ((request.flags & ipc::kPreviewRequestFlagFolderThumbnail) && status == ERROR_TIMEOUT);
}

bool ThumbnailCache::FolderRequestCurrent(const Request& request) {
    { std::lock_guard lock(mutex_);
      if (request.epoch != epoch_.load(std::memory_order_relaxed)) return false;
      if (VolumeSuspendedLocked(request.path) ||
          (request.interest_tracked && !WantedByWindowLocked(request.key))) {
          pending_.erase(request.key); return false;
      } }
    if (request.document && !request.document->active.load()) {
        std::lock_guard lock(mutex_);
        pending_.erase(request.key);
        return false;
    }
    if (!(request.flags & ipc::kPreviewRequestFlagFolderThumbnail)) return true;
    std::lock_guard lock(mutex_);
    const auto it = folder_requests_.find(request.key);
    return it != folder_requests_.end() && it->second == request.id &&
        request.epoch == epoch_.load(std::memory_order_relaxed);
}

void ThumbnailCache::Worker(uint32_t lane) {
    LaneRef io = LaneAt(lane);
    HANDLE& lane_pipe = io.pipe;
    PROCESS_INFORMATION& lane_child = io.child;
    const uint32_t lane_token = io.token;
    // The host's priority class as last set (a new host starts at normal).
    DWORD boosted_pid = 0;
    DWORD boosted_class = NORMAL_PRIORITY_CLASS;
    // Explicit isolated-probe tracing; no file is opened in ordinary sessions.
    FILE* trace_file = nullptr;
    wchar_t trace_path[1024]{};
    const bool general_trace = GetEnvironmentVariableW(L"PULSE_TEST_PREVIEW_TRACE", trace_path,
        ARRAYSIZE(trace_path)) > 0;
    if (general_trace) {
        const auto file = std::wstring(trace_path) + (folder_thumbnail_ ? L".folder-" : L".ordinary-") +
            std::to_wstring(lane_token) + L".log";
        _wfopen_s(&trace_file, file.c_str(), L"a");
    } else if (folder_thumbnail_ && GetEnvironmentVariableW(L"PULSE_TEST_FOLDER_THUMB_TRACE", trace_path,
        ARRAYSIZE(trace_path)) > 0) _wfopen_s(&trace_file, trace_path, L"a");
    std::unique_ptr<FILE, decltype(&std::fclose)> trace(trace_file, &std::fclose);
    if (warm_) {
        // Warm(): every lane starts its host now rather than on its first request.
        Request probe;
        Connect(lane, probe, GetTickCount64());
    }
    while (running_) {
        Request req;
        bool slow = false;
        {
            std::unique_lock lock(mutex_);
            const auto ready = [&] {
                if (!running_ || release_lanes_[lane]) return true;
                // After idle, give the first request to the warm primary host.
                // Spare hosts return only when there is concurrent work.
                if (lane > 0 && !lane_child.hProcess && !lane_busy_[0]) return false;
                return TakeRequestLocked(req, slow);
            };
            if (lane > 0 && lane_child.hProcess) {
                if (!cv_.wait_for(lock, std::chrono::seconds(30), ready)) {
                    lock.unlock();
                    StopChild(lane);
                    lock.lock();
                    lane_paths_[lane].clear();
                    continue;
                }
            } else cv_.wait(lock, ready);
            if (!running_) break;
            if (release_lanes_[lane]) {
                auto release = release_lanes_[lane];
                lock.unlock();
                // Only this lane owns its private host handles. Keep an independent
                // process handle to prove termination after StopChild closes them.
                HANDLE process = nullptr;
                const bool had_process = lane_child.hProcess != nullptr;
                const bool duplicated = !had_process || DuplicateHandle(GetCurrentProcess(), lane_child.hProcess,
                    GetCurrentProcess(), &process, SYNCHRONIZE, FALSE, 0);
                StopChild(lane);
                const bool released = duplicated && (!process || WaitForSingleObject(process, INFINITE) == WAIT_OBJECT_0);
                if (process) CloseHandle(process);
                lock.lock();
                lane_paths_[lane].clear();
                release_lanes_[lane].reset();
                if (!released && !release->failed) {
                    release->promise.set_exception(std::make_exception_ptr(std::runtime_error("Thumbnail host release unconfirmed")));
                    release->failed = true;
                }
                if (--release->pending == 0 && !release->failed) release->promise.set_value();
                continue;
            }
            lane_paths_[lane] = req.path;
            lane_busy_[lane] = true;
            cv_.notify_all();
        }
        // Return both the busy marker and any slow slot on every exit path.
        struct SlowSlot {
            ThumbnailCache* cache;
            bool held;
            bool refinement;
            uint32_t lane;
            ~SlowSlot() {
                {
                    std::lock_guard lock(cache->mutex_);
                    cache->lane_busy_[lane] = false;
                    if (held && cache->slow_in_flight_) --cache->slow_in_flight_;
                    if (refinement && cache->psd_refinements_in_flight_) --cache->psd_refinements_in_flight_;
                }
                cache->cv_.notify_all();
            }
        } slow_slot{this, slow, (req.flags & ipc::kPreviewRequestFlagPsdRefine) != 0, lane};
        if (!FolderRequestCurrent(req)) continue;
        const auto request_start = GetTickCount64();
        if (!(req.flags & ipc::kPreviewRequestFlagGrid)) diagnostics::runtime::TextEvent("preview_client_request",
            {{"extension", preview::DiagnosticExtension(req.path)}}, {
            {"request", req.id}, {"generation", req.generation}, {"cache", LaneAt(lane).token},
            {"path_id", preview::DiagnosticPathId(req.path)}, {"pixels", req.pixels},
            {"source_bytes", req.size}, {"age_ms", request_start - req.queued_at},
            {"deferrals", req.budget_deferrals}, {"kind", static_cast<uint32_t>(req.kind)}, {"flags", req.flags}});
        std::vector<uint8_t> trace_path_bytes;
        if (trace) pulse::EncodeUtf8Bytes(req.path, trace_path_bytes);
        const std::string trace_request_path(trace_path_bytes.begin(), trace_path_bytes.end());
        const auto mark = [&](const char* stage, bool success, uint32_t status = 0) {
            if (!trace) return;
            PROCESS_MEMORY_COUNTERS_EX memory{};
            memory.cb = sizeof(memory);
            const bool memory_ok = lane_child.hProcess && GetProcessMemoryInfo(lane_child.hProcess,
                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
            std::fprintf(trace.get(), "pid=%lu id=%u stage=%s elapsed=%llu tick=%llu ok=%d status=%u host=%lu kind=%u flags=%u pixels=%u timeout=%u urgent=%d class=%lu memory_ok=%d private=%zu working=%zu path=%s\n",
                GetCurrentProcessId(), req.id, stage,
                static_cast<unsigned long long>(GetTickCount64() - request_start),
                static_cast<unsigned long long>(GetTickCount64()), success, status, lane_child.dwProcessId,
                static_cast<unsigned>(req.kind), req.flags, req.pixels, req.timeout_ms, req.urgent ? 1 : 0,
                lane_child.hProcess ? GetPriorityClass(lane_child.hProcess) : 0ul, memory_ok,
                memory.PrivateUsage, memory.WorkingSetSize, trace_request_path.c_str());
            std::fflush(trace.get());
        };
        mark("begin", true);
        // Grid thumbnails of single files may come from the disk store,
        // which needs no host at all.
        ThumbnailStore* const store = visible_order_ && !req.details && !req.document &&
            req.kind == ipc::PreviewRequestKind::Content && req.frame_index == 0 &&
            !folder_thumbnail_ && !(req.flags & ipc::kPreviewRequestFlagFolderThumbnail) &&
            !(req.attrs & FILE_ATTRIBUTE_DIRECTORY) ? g_grid_store.load(std::memory_order_acquire) : nullptr;
        const ThumbnailStoreKey store_key{req.path, req.attrs, req.pixels, req.modified, req.size,
            (req.flags & ipc::kPreviewRequestFlagPsdRefine) != 0};
        // Folder composites: the stored one shows at once and the host is
        // still asked, since a child can change without touching the
        // folder's own time.
        const bool folder_request = folder_thumbnail_ && (req.flags & ipc::kPreviewRequestFlagFolderThumbnail);
        ThumbnailStore* const folder_store = folder_request ? g_grid_store.load(std::memory_order_acquire) : nullptr;
        const ThumbnailStoreKey folder_key{req.identity, req.attrs, req.pixels, req.modified, req.size, true};
        std::vector<uint8_t> folder_shown;
        if (folder_store && !(req.flags & ipc::kPreviewRequestFlagFolderRefresh)) {
            bool fresh = false;
            { std::lock_guard lock(mutex_); fresh = !items_.contains(req.key); }
            StoredThumbnail kept;
            if (fresh && folder_store->Load(folder_key, kept)) {
                folder_shown = kept.pixels;
                Item hit;
                hit.kind = ipc::PreviewContentKind::Bitmap;
                hit.w = kept.width; hit.h = kept.height; hit.stride = kept.width * 4;
                hit.pixels = std::move(kept.pixels);
                hit.artwork_bounds = ThumbnailArtworkBounds(req.path, hit.pixels, hit.w, hit.h, hit.stride);
                hit.frame_count = 1;
                hit.integrity.state = preview::IntegrityState::Complete;
                hit.cost = hit.pixels.size();
                const bool shown = StoreResult(req, std::move(hit));
                mark("disk-hit", shown);
                if (shown) {
                    NotifyResult(req);
                    std::lock_guard lock(mutex_);
                    folder_requests_[req.key] = req.id;
                    pending_.insert(req.key);
                }
            }
        }
        const bool psd_refine = (req.flags & ipc::kPreviewRequestFlagPsdRefine) != 0;
        const auto extension = preview::DiagnosticExtension(req.path);
        const bool psd_grid = visible_order_ && !req.details && !req.document &&
            !((req.flags | extra_flags_.load(std::memory_order_relaxed)) & ipc::kPreviewRequestFlagCard) &&
            (_wcsicmp(std::wstring(extension).c_str(), L".psd") == 0 ||
            _wcsicmp(std::wstring(extension).c_str(), L".psb") == 0) &&
            req.kind == ipc::PreviewRequestKind::Content;
        if (store && !psd_refine) {
            StoredThumbnail kept;
            if (store->Load(store_key, kept)) {
                Item hit;
                hit.kind = ipc::PreviewContentKind::Bitmap;
                hit.w = kept.width; hit.h = kept.height; hit.stride = kept.width * 4;
                hit.pixels = std::move(kept.pixels);
                hit.artwork_bounds = ThumbnailArtworkBounds(req.path, hit.pixels, hit.w, hit.h, hit.stride);
                hit.source_width = kept.source_width;
                hit.source_height = kept.source_height;
                hit.duration_ms = kept.duration_ms;
                hit.frame_count = 1;
                hit.integrity.state = preview::IntegrityState::Complete;
                hit.animation_identity = req.identity;
                hit.cost = hit.pixels.size();
                const bool refine = psd_grid && (std::max)(hit.w, hit.h) <
                    (std::min)(req.pixels, (std::max)(hit.source_width, hit.source_height));
                hit.needs_psd_refine = refine;
                const bool kept_stored = StoreResult(req, std::move(hit));
                mark("disk-hit", kept_stored);
                if (kept_stored) { NotifyResult(req); if (refine) QueuePsdRefinement(req); }
                continue;
            }
        }
        uint32_t failure_stage = 0;
        DWORD failure_error = ERROR_SUCCESS;
        const auto fail = [&](uint32_t stage, DWORD error) {
            if (!failure_stage) { failure_stage = stage; failure_error = error; }
        };
        Item result; bool ok = Connect(lane, req, request_start);
        const bool connected = ok;
        if (ok && boost_hosts_) {
            const DWORD wanted_class = req.urgent ? ABOVE_NORMAL_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS;
            if (lane_child.dwProcessId != boosted_pid || wanted_class != boosted_class) {
                SetPriorityClass(lane_child.hProcess, wanted_class);
                boosted_pid = lane_child.dwProcessId;
                boosted_class = wanted_class;
            }
        }
        mark("connected", ok);
        ipc::PreviewRequest wire; wire.request_id=req.id; wire.generation=req.generation;
        wire.kind=req.kind; wire.pixel_size=req.pixels; wire.attrs=req.attrs;
        wire.frame_index = req.frame_index;
        wire.flags |= req.flags | extra_flags_.load(std::memory_order_relaxed);
        if (!req.details) wire.flags |= ipc::kPreviewRequestFlagGrid;
        else if (quick_look_content_)
            wire.flags |= (req.attrs & FILE_ATTRIBUTE_DIRECTORY) ? ipc::kPreviewRequestFlagFolderListing
                                                                 : ipc::kPreviewRequestFlagRichText;
        if ((wire.flags & ipc::kPreviewRequestFlagGrid) && !(wire.attrs & FILE_ATTRIBUTE_DIRECTORY) &&
            wire.kind == ipc::PreviewRequestKind::Content) wire.magic = ipc::kPreviewBudgetRequestMagic;
        wire.path_chars=(uint32_t)req.path.size();
        if (ok) ok = ipc::WriteAll(lane_pipe, &wire, sizeof(wire)) &&
                     ipc::WriteAll(lane_pipe, req.path.data(), wire.path_chars * sizeof(wchar_t));
        if (connected && !ok) fail(3, GetLastError());
        mark("sent", ok);
        const auto sent_at = GetTickCount64();
        ipc::PreviewResponse response{};
        // A slow decode (48 MP HEIC, a video poster through the shell) is not
        // finished once the pointer has moved on to another file: restarting
        // the host costs far less than the rest of the decode would.
        const uint32_t abandon_ms = abandon_stale_ms_.load(std::memory_order_relaxed);
        // Facts (Properties) are left to finish: they rarely take long, and a
        // restart would also drop the codecs the host has warmed up.
        const auto superseded = [&] {
            if (!abandon_ms || req.kind != ipc::PreviewRequestKind::Content ||
                GetTickCount64() - request_start < abandon_ms) return false;
            std::lock_guard lock(mutex_);
            return !queue_.empty() && queue_.front().path != req.path;
        };
        bool abandoned = false;
        bool demand_cancelled = false;
        preview::DecodeMemoryBudget::Lease decode_lease;
        bool budget_over_limit = false;
        unsigned budget_proposals = 0;
        if (ok) {
            ULONGLONG deadline = GetTickCount64() + (req.timeout_ms ? req.timeout_ms : 1000);
            ULONGLONG last_trace = GetTickCount64();
            DWORD available = 0;
            while (running_ && GetTickCount64() < deadline) {
                if (!FolderRequestCurrent(req)) { fail(13, ERROR_CANCELLED); demand_cancelled = abandoned = true; ok = false; break; }
                if (!PeekNamedPipe(lane_pipe, nullptr, 0, nullptr, &available, nullptr)) { fail(4, GetLastError()); ok=false; break; }
                if (available >= sizeof(response)) {
                    ipc::PreviewResponse proposal{};
                    DWORD peeked = 0;
                    if (!PeekNamedPipe(lane_pipe, &proposal, sizeof(proposal), &peeked, nullptr, nullptr)) { ok = false; break; }
                    if (proposal.magic != ipc::kPreviewBudgetProposalMagic) break;
                    if (wire.magic != ipc::kPreviewBudgetRequestMagic || ++budget_proposals > 16 ||
                        proposal.request_id != req.id || proposal.generation != req.generation ||
                        proposal.mapping_chars || proposal.text_chars || proposal.error_chars || proposal.property_count ||
                        (proposal.flags & ~1u) || !ipc::ReadAll(lane_pipe, &proposal, sizeof(proposal))) {
                        fail(7, ERROR_INVALID_DATA); ok = false; break;
                    }
                    const uint64_t bytes = uint64_t(proposal.width) | (uint64_t(proposal.height) << 32);
                    auto& budget = preview::SharedDecodeMemoryBudget();
                    budget_over_limit = bytes > budget.Limit();
                    const auto waiting_since = GetTickCount64();
                    const bool upgrade = decode_lease.Bytes() != 0;
                    bool granted = false;
                    do {
                        granted = budget.Reserve(decode_lease, bytes, (proposal.flags & 1) != 0);
                        if (granted || upgrade || budget_over_limit) break;
                        if (!running_ || !FolderRequestCurrent(req) || superseded()) {
                            demand_cancelled = abandoned = true; ok = false; break;
                        }
                        if (lane_child.hProcess && WaitForSingleObject(lane_child.hProcess, 0) == WAIT_OBJECT_0) { ok = false; break; }
                        Sleep(10);
                    } while (GetTickCount64() - waiting_since < 2000);
                    // Admission wait is not codec execution and must not spend its watchdog.
                    deadline += GetTickCount64() - waiting_since;
                    if (!ok) break;
                    const uint32_t grant = granted ? ipc::kPreviewBudgetGrant : 0;
                    if (!ipc::WriteAll(lane_pipe, &grant, sizeof(grant))) { ok = false; break; }
                    mark(granted ? "budget-granted" : "budget-deferred", granted, static_cast<uint32_t>(bytes >> 20));
                    available = 0;
                    continue;
                }
                if (superseded()) { abandoned = true; fail(13, ERROR_CANCELLED); ok = false; break; }
                if (lane_child.hProcess && WaitForSingleObject(lane_child.hProcess, 0) == WAIT_OBJECT_0) { fail(6, ERROR_PROCESS_ABORTED); ok=false; break; }
                if (trace && GetTickCount64() - last_trace >= 250) {
                    mark("waiting", true);
                    last_trace = GetTickCount64();
                }
                WaitForHost(GetTickCount64() - request_start);
            }
            if (available < sizeof(response)) {
                fail(running_ ? 5 : 13, running_ ? ERROR_TIMEOUT : ERROR_CANCELLED);
                ok=false;
            }
        }
        mark("header-ready", ok);
        const auto decode_ms = static_cast<uint32_t>(GetTickCount64() - sent_at);
        if (abandoned) {
            // Nothing is remembered about the file: coming back to it asks
            // again at once, without the transient-failure back-off.
            if (!demand_cancelled) { std::lock_guard lock(mutex_); pending_.erase(req.key); }
            LogFailure(lane, req, request_start, failure_stage, failure_error, true);
            StopChild(lane);
            continue;
        }
        const auto read = [&](void* data, DWORD bytes) {
            if (ipc::ReadAll(lane_pipe, data, bytes)) return true;
            fail(7, GetLastError());
            return false;
        };
        if (ok) {
            ok = read(&response, sizeof(response));
            if (ok && (response.magic != ipc::kPreviewMagic || response.request_id != req.id ||
                       response.generation != req.generation || response.mapping_chars > 512 ||
                       response.property_count > 16)) {
                fail(7, ERROR_INVALID_DATA); ok = false;
            }
        }
        std::wstring mapping;
        if (ok && response.mapping_chars) { mapping.resize(response.mapping_chars);
            ok = read(mapping.data(), response.mapping_chars * sizeof(wchar_t)); }
        std::wstring previewText;
        if (ok && response.text_chars) {
            const uint32_t limit = response.kind == ipc::PreviewContentKind::Table ||
                                   response.kind == ipc::PreviewContentKind::Tree
                ? ipc::kPreviewMaxTableChars
                : response.kind == ipc::PreviewContentKind::Markdown
                ? ipc::kPreviewMaxTableChars  // DOCX / EPUB payloads run up to 2M
                : response.kind == ipc::PreviewContentKind::Archive
                ? ipc::kPreviewMaxArchiveChars : ipc::kPreviewMaxTextChars;
            if (response.text_chars > limit) { fail(7, ERROR_INVALID_DATA); ok = false; }
            else {
                previewText.resize(response.text_chars);
                ok = read(previewText.data(),
                                  response.text_chars * sizeof(wchar_t));
            }
        }
        std::wstring errorText;
        if (ok && response.error_chars) {
            if (response.error_chars > 512) { fail(7, ERROR_INVALID_DATA); ok = false; }
            else {
                errorText.resize(response.error_chars);
                ok = read(errorText.data(),
                                  response.error_chars * sizeof(wchar_t));
            }
        }
        std::vector<PreviewProperty> properties;
        for (uint32_t i = 0; ok && i < response.property_count && i < 16; ++i) {
            uint32_t labelChars = 0, valueChars = 0;
            ok = read(&labelChars, sizeof(labelChars)) && labelChars <= 128;
            PreviewProperty property;
            if (ok && labelChars) {
                property.label.resize(labelChars);
                ok = read(property.label.data(), labelChars * sizeof(wchar_t));
            }
            if (ok) ok = read(&valueChars, sizeof(valueChars)) && valueChars <= 1024;
            if (ok && valueChars) {
                property.value.resize(valueChars);
                ok = read(property.value.data(), valueChars * sizeof(wchar_t));
            }
            if (ok) properties.push_back(std::move(property));
        }
        if (connected && !ok && !failure_stage) fail(7, ERROR_INVALID_DATA);
        mark("payload-read", ok, response.status);
        if (ok && response.status == 0 && !mapping.empty()) {
            HANDLE map = OpenFileMappingW(FILE_MAP_READ, FALSE, mapping.c_str());
            if (map) { const size_t bytes=(size_t)response.stride*response.height;
                if (void* p=MapViewOfFile(map, FILE_MAP_READ,0,0,bytes)) {
                    result.pixels.assign((uint8_t*)p,(uint8_t*)p+bytes); UnmapViewOfFile(p);
                    result.w=response.width; result.h=response.height; result.stride=response.stride;
                    result.artwork_bounds = ThumbnailArtworkBounds(req.path, result.pixels,
                        result.w, result.h, result.stride);
                    // Audio covers tint Quick Look's waveform. The pixels
                    // are freed on upload, so measure them here, off the UI
                    // thread; at most 48 x 48 samples.
                    if (quick_look_content_ && response.kind == ipc::PreviewContentKind::Bitmap) {
                        result.palette = ComputeCoverPalette(result.pixels.data(),
                            result.w, result.h, result.stride);
                        result.stats = ComputeImageStats(result.pixels.data(),
                            result.w, result.h, result.stride);
                    }
                } else fail(9, GetLastError());
                CloseHandle(map);
            } else fail(8, GetLastError());
        }
        if (ok && response.mapping_chars) {
            const unsigned char ack=1; ok=ipc::WriteAll(lane_pipe,&ack,1);
            if (!ok) fail(10, GetLastError());
        }
        if (ok) {
            result.kind = response.kind;
            result.text = std::move(previewText);
            result.error = std::move(errorText);
            result.properties = std::move(properties);
            result.truncated = (response.flags & ipc::kPreviewFlagTruncated) != 0;
            result.integrity = response.integrity;
            result.bytes_read = response.bytes_read;
            result.frame_count = (std::max)(1u, response.frame_count);
            result.frame_delay_ms = response.frame_delay_ms;
            result.loop_count = response.loop_count;
            result.text_encoding = (response.flags & ipc::kPreviewFlagEncodingMask) >>
                                   ipc::kPreviewFlagEncodingShift;
            result.frame_index = req.frame_index;
            result.animation_identity = req.identity;
            result.source_width = response.source_width;
            result.source_height = response.source_height;
            result.duration_ms = response.duration_ms;
        }
        result.cost = result.text.size() * sizeof(wchar_t)
            + result.error.size() * sizeof(wchar_t)
            + static_cast<size_t>(response.stride) * response.height;
        for (const auto& property : result.properties)
            result.cost += (property.label.size() + property.value.size()) * sizeof(wchar_t);
        // !ok: the host timed out, crashed or broke the protocol - nothing is
        // known about the file itself, so the result is retried later.
        result.transient = IsTransientResponse(ok, req, response.status);
        if (ok && (req.flags & ipc::kPreviewRequestFlagFolderThumbnail) && response.status == ERROR_TIMEOUT)
            result.error = L"folder-host-timeout";
        result.failed = !ok || response.status != 0 ||
            (req.kind == ipc::PreviewRequestKind::Content &&
             result.kind == ipc::PreviewContentKind::Bitmap && result.pixels.empty());
        if (result.failed && (!ok || result.integrity.state == preview::IntegrityState::Complete)) {
            result.integrity.state = preview::IntegrityState::Failed;
            result.integrity.reason = preview::IntegrityReason::Unavailable;
        }
        if (!psd_refine && ok && result.error == L"decode-memory-budget" && !budget_over_limit && req.budget_deferrals < 32 &&
            running_ && FolderRequestCurrent(req)) {
            decode_lease.Reset();
            ++req.budget_deferrals;
            Sleep((std::min)(250u, 20u * req.budget_deferrals) + lane * 7);
            std::lock_guard lock(mutex_);
            if (running_ && (req.epoch == epoch_.load(std::memory_order_relaxed) && !VolumeSuspendedLocked(req.path) &&
                (!req.interest_tracked || WantedByWindowLocked(req.key)) && (!req.document || req.document->active.load()))) { queue_.push_back(std::move(req)); cv_.notify_one(); }
            else if (req.epoch == epoch_.load(std::memory_order_relaxed)) pending_.erase(req.key);
            continue;
        }
        // A grid entry without a provider is expected; transport/mapping faults
        // and explicit preview failures still carry a terminal diagnostic.
        const bool ordinary_unsupported = !req.details && ok && response.status == 1 &&
            response.kind == ipc::PreviewContentKind::Unsupported;
        if (ok && response.status != 0 && !ordinary_unsupported)
            fail(11, static_cast<DWORD>(response.status));
        if (result.failed && !failure_stage && connected && !ordinary_unsupported)
            fail(12, ERROR_INVALID_DATA);
        if (failure_stage) LogFailure(lane, req, request_start, failure_stage, failure_error,
            failure_stage == 13 || !running_ || !FolderRequestCurrent(req));
        mark("store-begin", ok, response.status);
        // Copied before the item is handed over; written once it is on screen.
        StoredThumbnail keep;
        ThumbnailStore* const saver = store ? store : folder_store;
        const bool embedded_psd = (response.flags & ipc::kPreviewFlagEmbeddedPsd) != 0;
        const bool refine = psd_grid && !psd_refine && !result.failed && embedded_psd;
        const bool keepable = saver && !embedded_psd && !result.failed && !result.transient &&
            result.kind == ipc::PreviewContentKind::Bitmap && result.frame_count == 1 &&
            result.stride == result.w * 4 && result.integrity.state != preview::IntegrityState::Partial;
        if (keepable) {
            keep.pixels = result.pixels;
            keep.width = result.w; keep.height = result.h;
            keep.source_width = result.source_width;
            keep.source_height = result.source_height;
            keep.duration_ms = result.duration_ms;
        }
        result.needs_psd_refine = refine;
        const bool stored = !(psd_refine && (embedded_psd || abandoned)) && StoreResult(req, std::move(result));
        if (psd_refine && (embedded_psd || abandoned)) {
            std::lock_guard lock(mutex_);
            if (req.epoch == epoch_.load(std::memory_order_relaxed)) {
                pending_.erase(req.key);
                if (!abandoned) {
                    if (const auto old = items_.find(req.key); old != items_.end()) old->second.needs_psd_refine = false;
                }
            }
        }
        if (psd_refine) diagnostics::runtime::TextEvent("psd_refinement",
            {{"outcome", abandoned ? L"cancelled" : embedded_psd ? L"embedded-fallback" :
                stored ? L"success" : failure_stage ? L"decode-error" : L"stale"}}, {
            {"path_id", preview::DiagnosticPathId(req.path)}, {"target_edge", req.pixels},
            {"result_edge", (std::max)(response.width, response.height)},
            {"source_edge", (std::max)(response.source_width, response.source_height)},
            {"completed", stored}, {"embedded", embedded_psd}, {"cancelled", abandoned},
            {"elapsed_ms", GetTickCount64() - request_start}});
        mark("stored", stored, response.status);
        if (!stored && !psd_refine) LogFailure(lane, req, request_start, 12, ERROR_CANCELLED, true);
        if (stored) { NotifyResult(req); if (refine) QueuePsdRefinement(req); }
        if (!ok) StopChild(lane);
        if (keepable && stored && !(folder_store && keep.pixels == folder_shown))
            saver->Save(folder_store ? folder_key : store_key, decode_ms, keep);
    }
}
} // namespace pulse::ui

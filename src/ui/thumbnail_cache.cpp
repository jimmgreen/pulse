#include "thumbnail_cache.h"
#include "../common/utf8_file.h"
#include "../common/runtime_log.h"
#include "thumbnail_artwork_layout.h"
#include <algorithm>
#include <cwctype>
#include <iterator>
#include <cstdio>
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
    pixels.clear();
    pixels.shrink_to_fit();
    return true;
}

// Aspect-fit (contain), optionally placing the visible artwork at dest.bottom.
void DrawContained(ID2D1DeviceContext* dc, ID2D1Bitmap* bitmap, uint32_t w, uint32_t h,
                   const D2D1_RECT_F& dest, float opacity, const IconArtworkBounds& bounds,
                   bool align_artwork_bottom, D2D1_RECT_F* artwork_rect) {
    const auto fitted = ContainedThumbnailRect(dest, w, h, bounds, align_artwork_bottom);
    if (artwork_rect) *artwork_rect = ThumbnailArtworkRect(fitted, bounds);
    dc->DrawBitmap(bitmap, &fitted, std::clamp(opacity, 0.0f, 1.0f),
        D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
}
} // namespace

// Shell thumbnail providers (Office, CAD, video) routinely need a few seconds
// on a cold start; direct decoders answer well inside the short budget. A
// timeout restarts the host, so the heavy formats get room to finish.
uint32_t ThumbnailCache::ResponseTimeoutMs(const std::wstring& path,
                                           ipc::PreviewRequestKind kind) {
    if (kind == ipc::PreviewRequestKind::Properties) return 4000;
    const size_t slash = path.find_last_of(L"\\/");
    const size_t dot = path.find_last_of(L'.');
    std::wstring ext = dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)
        ? std::wstring() : path.substr(dot);
    for (auto& c : ext) c = static_cast<wchar_t>(std::towlower(c));
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
}

ThumbnailCache::~ThumbnailCache() { Reset(); }
void ThumbnailCache::SetDeviceContext(ID2D1DeviceContext* dc) {
    if (dc_ == dc) return;
    std::lock_guard lock(mutex_); dc_ = dc;
    for (auto& [_, item] : items_) item.bitmap.reset();
}
std::wstring ThumbnailCache::Key(const std::wstring& path, uint32_t pixels,
                                 uint64_t modified, uint64_t size,
                                 uint32_t frame_index) const {
    return path + L"\n" + std::to_wstring(pixels) + L":" + std::to_wstring(modified) +
           L":" + std::to_wstring(size) + L":" + std::to_wstring(frame_index);
}
void ThumbnailCache::StopChild() {
    if (pipe_ != INVALID_HANDLE_VALUE) { CancelIoEx(pipe_, nullptr); CloseHandle(pipe_); pipe_ = INVALID_HANDLE_VALUE; }
    if (child_.hProcess) { TerminateProcess(child_.hProcess, 0); CloseHandle(child_.hProcess); CloseHandle(child_.hThread); child_ = {}; }
}
void ThumbnailCache::Reset() {
    running_ = false; cv_.notify_all();
    if (worker_.joinable()) CancelSynchronousIo(worker_.native_handle());
    if (worker_.joinable()) worker_.join();
    StopChild();
    std::lock_guard lock(mutex_); queue_.clear(); pending_.clear(); items_.clear(); lru_.clear(); folder_requests_.clear();
    still_by_identity_.clear(); transient_failures_.clear(); retry_interest_.clear();
    cache_bytes_ = 0; dc_ = nullptr; latest_details_identity_.clear();
    draw_retry_deadline_ = 0;
    epoch_.fetch_add(1, std::memory_order_relaxed);
}
void ThumbnailCache::BeginFrame() {
    std::lock_guard lock(mutex_);
    track_retry_interest_ = true;
    retry_interest_.clear();
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

void ThumbnailCache::Evict() {
    epoch_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock(mutex_);
    queue_.clear(); pending_.clear(); items_.clear(); lru_.clear(); folder_requests_.clear();
    still_by_identity_.clear(); transient_failures_.clear(); retry_interest_.clear();
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
    if (!UploadBitmap(dc_, item.bitmap, item.pixels, item.w, item.h, item.stride)) return nullptr;
    Touch(item);
    return &item;
}
// Stage values are stable diagnostics identifiers: launch=1, connect=2, send=3,
// wait=4, timeout=5, exited=6, response=7, mapping-open=8, mapping-view=9,
// ack=10, host-status=11, store=12, cancelled=13.
void ThumbnailCache::LogFailure(const Request& req, uint64_t started, uint32_t stage,
                                DWORD error, bool cancelled) {
    {
        std::lock_guard lock(mutex_);
        cancelled = cancelled || req.epoch != epoch_.load(std::memory_order_relaxed) ||
            (req.details && req.identity != latest_details_identity_) ||
            (req.document && !req.document->active.load());
    }
    DWORD exit_code = 0;
    const bool exit_known = child_.hProcess && GetExitCodeProcess(child_.hProcess, &exit_code);
    diagnostics::runtime::Event(cancelled ? "preview_client_cancelled" : "preview_client_failure", {
        {"stage", stage}, {"error", error}, {"elapsed_ms", GetTickCount64() - started},
        {"request", req.id}, {"generation", req.generation}, {"host_pid", child_.dwProcessId},
        {"exit_known", exit_known}, {"exit_code", exit_code}, {"cache", pipe_token_},
        {"kind", static_cast<uint32_t>(req.kind)}, {"flags", req.flags}},
        cancelled ? diagnostics::runtime::Level::Info : diagnostics::runtime::Level::Error);
}
bool ThumbnailCache::Connect(const Request& req, uint64_t started) {
    if (pipe_ != INVALID_HANDLE_VALUE) return true;
    const std::wstring pipeName = ipc::PreviewPipeName(pipe_token_);
    wchar_t exe[MAX_PATH]{}; GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (!slash) { LogFailure(req, started, 1, ERROR_BAD_PATHNAME); return false; }
    *(slash + 1) = 0;
    std::wstring cmd = L"\"" + std::wstring(exe) + L"Pulse.Preview.exe\" " +
        std::to_wstring(pipe_token_);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_FORCEOFFFEEDBACK; // background helper: no AppStarting cursor
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | (folder_thumbnail_ ? BELOW_NORMAL_PRIORITY_CLASS : 0),
                        nullptr, nullptr, &si, &child_)) {
        const DWORD error = GetLastError();
        LogFailure(req, started, 1, error, !running_);
        return false;
    }
    const ULONGLONG deadline = GetTickCount64() + 3000;
    DWORD connect_error = ERROR_SUCCESS;
    do {
        pipe_ = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr);
        if (pipe_ != INVALID_HANDLE_VALUE) {
            ULONG server_pid = 0;
            const bool got_pid = GetNamedPipeServerProcessId(pipe_, &server_pid) != FALSE;
            const DWORD error = got_pid ? ERROR_ACCESS_DENIED : GetLastError();
            if (got_pid && server_pid == child_.dwProcessId) return true;
            LogFailure(req, started, 2, error, !running_);
            CloseHandle(pipe_);
            pipe_ = INVALID_HANDLE_VALUE;
            StopChild();
            return false;
        }
        connect_error = GetLastError();
        if (WaitForSingleObject(child_.hProcess, 0) == WAIT_OBJECT_0) break;
        Sleep(25);
    } while (running_ && GetTickCount64() < deadline);
    LogFailure(req, started, 2, connect_error, !running_);
    StopChild(); return false;
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
                                       const std::shared_ptr<DocumentImageSession>& document) {
    if (integrity) { *integrity = {}; integrity->state = preview::IntegrityState::Loading; }
    if (artwork_rect) *artwork_rect = dest;
    if (media_duration_ms) *media_duration_ms = 0;
    if (!dc || path.empty() || pixels < 24) return PreviewDrawResult::Failed;
    if (document && !document->active.load()) return PreviewDrawResult::Failed;
    const std::wstring document_key = document ? L":document:" + std::to_wstring(document->identity) : L"";
    const std::wstring key = Key(path, pixels, modified, size, frame_index) + document_key;
    {
        std::lock_guard lock(mutex_);
        if (track_retry_interest_) retry_interest_.insert(key);
        const std::wstring identity = Key(path, 0, modified, size) + document_key;
        if (direct_preview) SelectDetailsLocked(identity);
        auto queue_request = [&] {
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
            request.document = document;
            if (document) request.flags |= ipc::kPreviewRequestFlagDocumentImage;
            request.details = direct_preview; request.frame_index = frame_index;
            request.epoch = epoch_.load(std::memory_order_relaxed);
            request.timeout_ms = ResponseTimeoutMs(path, request.kind);
            request.path = path; request.key = key; request.identity = identity;
            queue_.push_front(std::move(request));
            if (queue_.size() > 128) { pending_.erase(queue_.back().key); queue_.pop_back(); }
            if (!running_.exchange(true)) worker_ = std::thread([this]{ Worker(); });
            cv_.notify_one();
        };
        auto it = items_.find(key);
        if (it != items_.end()) {
            Item& item = it->second;
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
                if (text) *text = item.text;
                if (item.kind == ipc::PreviewContentKind::Tree) return PreviewDrawResult::Tree;
                if (item.kind == ipc::PreviewContentKind::Table) return PreviewDrawResult::Table;
                if (item.kind == ipc::PreviewContentKind::Archive) return PreviewDrawResult::Archive;
                if (item.kind == ipc::PreviewContentKind::Markdown) return PreviewDrawResult::Markdown;
                return item.kind == ipc::PreviewContentKind::Hex
                    ? PreviewDrawResult::Hex : PreviewDrawResult::Text;
            }
            UploadBitmap(dc_, item.bitmap, item.pixels, item.w, item.h, item.stride);
            if (text && item.kind == ipc::PreviewContentKind::Bitmap) *text = item.text;  // e.g. icon sizes
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
        queue_request();
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
    const std::wstring identity = Key(path, 0, modified, size);
    const std::wstring key = identity + L":properties";
    std::lock_guard lock(mutex_);
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
        if (!running_.exchange(true)) worker_ = std::thread([this]{ Worker(); });
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

bool ThumbnailCache::StoreResult(const Request& req, Item result) {
    std::lock_guard lock(mutex_);
    // An old decode must not remove a replacement request queued after Evict().
    if (req.epoch != epoch_.load(std::memory_order_relaxed)) return false;
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
    if (req.details && req.identity != latest_details_identity_) return false;

    if (result.failed && result.transient) {
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

void ThumbnailCache::Worker() {
    // Explicit isolated-probe tracing; no file is opened in ordinary sessions.
    FILE* trace_file = nullptr;
    wchar_t trace_path[1024]{};
    const bool general_trace = GetEnvironmentVariableW(L"PULSE_TEST_PREVIEW_TRACE", trace_path,
        ARRAYSIZE(trace_path)) > 0;
    if (general_trace) {
        const auto file = std::wstring(trace_path) + (folder_thumbnail_ ? L".folder-" : L".ordinary-") +
            std::to_wstring(pipe_token_) + L".log";
        _wfopen_s(&trace_file, file.c_str(), L"a");
    } else if (folder_thumbnail_ && GetEnvironmentVariableW(L"PULSE_TEST_FOLDER_THUMB_TRACE", trace_path,
        ARRAYSIZE(trace_path)) > 0) _wfopen_s(&trace_file, trace_path, L"a");
    std::unique_ptr<FILE, decltype(&std::fclose)> trace(trace_file, &std::fclose);
    while (running_) {
        Request req;
        { std::unique_lock lock(mutex_); cv_.wait(lock, [&]{return !running_ || !queue_.empty();});
          if (!running_) break; req = std::move(queue_.front()); queue_.pop_front(); }
        if (!FolderRequestCurrent(req)) continue;
        const auto request_start = GetTickCount64();
        std::vector<uint8_t> trace_path_bytes;
        if (trace) pulse::EncodeUtf8Bytes(req.path, trace_path_bytes);
        const std::string trace_request_path(trace_path_bytes.begin(), trace_path_bytes.end());
        const auto mark = [&](const char* stage, bool success, uint32_t status = 0) {
            if (!trace) return;
            PROCESS_MEMORY_COUNTERS_EX memory{};
            memory.cb = sizeof(memory);
            const bool memory_ok = child_.hProcess && GetProcessMemoryInfo(child_.hProcess,
                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
            std::fprintf(trace.get(), "pid=%lu id=%u stage=%s elapsed=%llu ok=%d status=%u host=%lu kind=%u flags=%u pixels=%u timeout=%u memory_ok=%d private=%zu working=%zu path=%s\n",
                GetCurrentProcessId(), req.id, stage,
                static_cast<unsigned long long>(GetTickCount64() - request_start), success, status, child_.dwProcessId,
                static_cast<unsigned>(req.kind), req.flags, req.pixels, req.timeout_ms, memory_ok,
                memory.PrivateUsage, memory.WorkingSetSize, trace_request_path.c_str());
            std::fflush(trace.get());
        };
        mark("begin", true);
        uint32_t failure_stage = 0;
        DWORD failure_error = ERROR_SUCCESS;
        const auto fail = [&](uint32_t stage, DWORD error) {
            if (!failure_stage) { failure_stage = stage; failure_error = error; }
        };
        Item result; bool ok = Connect(req, request_start);
        const bool connected = ok;
        mark("connected", ok);
        ipc::PreviewRequest wire; wire.request_id=req.id; wire.generation=req.generation;
        wire.kind=req.kind; wire.pixel_size=req.pixels; wire.attrs=req.attrs;
        wire.frame_index = req.frame_index;
        wire.flags |= req.flags;
        if (!req.details) wire.flags |= ipc::kPreviewRequestFlagGrid;
        else if (quick_look_content_)
            wire.flags |= (req.attrs & FILE_ATTRIBUTE_DIRECTORY) ? ipc::kPreviewRequestFlagFolderListing
                                                                 : ipc::kPreviewRequestFlagRichText;
        wire.path_chars=(uint32_t)req.path.size();
        if (ok) ok = ipc::WriteAll(pipe_, &wire, sizeof(wire)) &&
                     ipc::WriteAll(pipe_, req.path.data(), wire.path_chars * sizeof(wchar_t));
        if (connected && !ok) fail(3, GetLastError());
        mark("sent", ok);
        ipc::PreviewResponse response{};
        if (ok) {
            const ULONGLONG deadline = GetTickCount64() + (req.timeout_ms ? req.timeout_ms : 1000);
            ULONGLONG last_trace = GetTickCount64();
            DWORD available = 0;
            while (running_ && GetTickCount64() < deadline) {
                if (!FolderRequestCurrent(req)) { fail(13, ERROR_CANCELLED); ok = false; break; }
                if (!PeekNamedPipe(pipe_, nullptr, 0, nullptr, &available, nullptr)) { fail(4, GetLastError()); ok=false; break; }
                if (available >= sizeof(response)) break;
                if (child_.hProcess && WaitForSingleObject(child_.hProcess, 0) == WAIT_OBJECT_0) { fail(6, ERROR_PROCESS_ABORTED); ok=false; break; }
                if (trace && GetTickCount64() - last_trace >= 250) {
                    mark("waiting", true);
                    last_trace = GetTickCount64();
                }
                Sleep(10);
            }
            if (available < sizeof(response)) {
                fail(running_ ? 5 : 13, running_ ? ERROR_TIMEOUT : ERROR_CANCELLED);
                ok=false;
            }
        }
        mark("header-ready", ok);
        const auto read = [&](void* data, DWORD bytes) {
            if (ipc::ReadAll(pipe_, data, bytes)) return true;
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
                    result.artwork_bounds = MeasureIconArtwork(result.pixels,
                        result.w, result.h, result.stride);
                    // Audio covers tint Quick Look's waveform. The pixels
                    // are freed on upload, so measure them here, off the UI
                    // thread; at most 48 x 48 samples.
                    if (quick_look_content_ && response.kind == ipc::PreviewContentKind::Bitmap)
                        result.palette = ComputeCoverPalette(result.pixels.data(),
                            result.w, result.h, result.stride);
                } else fail(9, GetLastError());
                CloseHandle(map);
            } else fail(8, GetLastError());
        }
        if (ok && response.mapping_chars) {
            const unsigned char ack=1; ok=ipc::WriteAll(pipe_,&ack,1);
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
        // A grid entry without a provider is expected; transport/mapping faults
        // and explicit preview failures still carry a terminal diagnostic.
        const bool ordinary_unsupported = !req.details && ok && response.status == 1 &&
            response.kind == ipc::PreviewContentKind::Unsupported;
        if (ok && response.status != 0 && !ordinary_unsupported)
            fail(11, static_cast<DWORD>(response.status));
        if (result.failed && !failure_stage && connected && !ordinary_unsupported)
            fail(12, ERROR_INVALID_DATA);
        if (failure_stage) LogFailure(req, request_start, failure_stage, failure_error,
            failure_stage == 13 || !running_ || !FolderRequestCurrent(req));
        mark("store-begin", ok, response.status);
        const bool stored = StoreResult(req, std::move(result));
        mark("stored", stored, response.status);
        if (!stored) LogFailure(req, request_start, 12, ERROR_CANCELLED, true);
        if (const HWND hwnd = hwnd_.load(); stored && hwnd)
            InvalidateRect(hwnd, nullptr, FALSE);
        if (!ok) StopChild();
    }
}
} // namespace pulse::ui

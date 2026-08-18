#include "thumbnail_cache.h"
#include <algorithm>

namespace pulse::ui {
ThumbnailCache::~ThumbnailCache() { Reset(); }
void ThumbnailCache::SetDeviceContext(ID2D1DeviceContext2* dc) {
    if (dc_ == dc) return;
    std::lock_guard lock(mutex_); dc_ = dc;
    for (auto& [_, item] : items_) item.bitmap.reset();
}
std::wstring ThumbnailCache::Key(const std::wstring& path, uint32_t pixels,
                                 uint64_t modified, uint64_t size) const {
    return path + L"\n" + std::to_wstring(pixels) + L":" + std::to_wstring(modified) +
           L":" + std::to_wstring(size);
}
void ThumbnailCache::StopChild() {
    if (pipe_ != INVALID_HANDLE_VALUE) { CancelIoEx(pipe_, nullptr); CloseHandle(pipe_); pipe_ = INVALID_HANDLE_VALUE; }
    if (child_.hProcess) { TerminateProcess(child_.hProcess, 0); CloseHandle(child_.hProcess); CloseHandle(child_.hThread); child_ = {}; }
}
void ThumbnailCache::Reset() {
    running_ = false; cv_.notify_all();
    if (pipe_ != INVALID_HANDLE_VALUE) CancelIoEx(pipe_, nullptr);
    if (worker_.joinable()) worker_.join();
    StopChild();
    std::lock_guard lock(mutex_); queue_.clear(); pending_.clear(); items_.clear(); lru_.clear();
    cache_bytes_ = 0; dc_ = nullptr;
}
bool ThumbnailCache::Connect() {
    if (pipe_ != INVALID_HANDLE_VALUE) return true;
    const DWORD pid = GetCurrentProcessId();
    const std::wstring pipeName = ipc::PreviewPipeName(pid);
    wchar_t exe[MAX_PATH]{}; GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    wchar_t* slash = wcsrchr(exe, L'\\'); if (!slash) return false; *(slash + 1) = 0;
    std::wstring cmd = L"\"" + std::wstring(exe) + L"Pulse.Preview.exe\" " + std::to_wstring(pid);
    STARTUPINFOW si{sizeof(si)};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &child_)) return false;
    const ULONGLONG deadline = GetTickCount64() + 3000;
    do {
        pipe_ = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr);
        if (pipe_ != INVALID_HANDLE_VALUE) return true;
        Sleep(25);
    } while (GetTickCount64() < deadline);
    StopChild(); return false;
}
PreviewDrawResult ThumbnailCache::Draw(ID2D1DeviceContext* dc, const D2D1_RECT_F& dest,
                                       const std::wstring& path, DWORD attrs, uint32_t pixels,
                                       uint64_t generation, uint64_t modified, uint64_t size,
                                       float opacity, std::wstring* text,
                                       bool* truncated, uint32_t* bytes_read,
                                       bool direct_preview, std::wstring* error,
                                       float* pan_x, float* pan_y,
                                       float* pan_max_x, float* pan_max_y) {
    if (!dc || path.empty() || pixels < 24) return PreviewDrawResult::Failed;
    const std::wstring key = Key(path, pixels, modified, size);
    {
        std::lock_guard lock(mutex_);
        const std::wstring identity = Key(path, 0, modified, size);
        if (direct_preview && identity != latest_details_identity_) {
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
        auto it = items_.find(key);
        if (it != items_.end()) {
            Item& item = it->second;
            if (truncated) *truncated = item.truncated;
            if (bytes_read) *bytes_read = item.bytes_read;
            if (error) *error = item.error;
            if (item.kind == ipc::PreviewContentKind::Text ||
                item.kind == ipc::PreviewContentKind::Hex) {
                if (text) *text = item.text;
                return item.kind == ipc::PreviewContentKind::Hex
                    ? PreviewDrawResult::Hex : PreviewDrawResult::Text;
            }
            if (!item.bitmap.get() && !item.pixels.empty() && dc_) {
                auto props = D2D1::BitmapProperties(D2D1::PixelFormat(
                    DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                dc_->CreateBitmap(D2D1::SizeU(item.w, item.h), item.pixels.data(), item.stride,
                                  &props, &item.bitmap);
                item.pixels.clear(); item.pixels.shrink_to_fit();
            }
            if (item.bitmap.get()) {
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
                        dest.left + (destW - drawW) * 0.5f - *pan_x,
                        dest.top + (destH - drawH) * 0.5f - *pan_y,
                        dest.left + (destW - drawW) * 0.5f - *pan_x + drawW,
                        dest.top + (destH - drawH) * 0.5f - *pan_y + drawH);
                    dc->PushAxisAlignedClip(dest, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                    dc->DrawBitmap(item.bitmap.get(), &fitted,
                        std::clamp(opacity, 0.0f, 1.0f),
                        D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
                    dc->PopAxisAlignedClip();
                    return PreviewDrawResult::Bitmap;
                }
                D2D1_RECT_F fitted = dest;
                const float sourceAspect = static_cast<float>(item.w) /
                                           static_cast<float>(std::max(1u, item.h));
                const float destAspect = destW / destH;
                if (sourceAspect > destAspect) {
                    const float height = destW / sourceAspect;
                    const float center = (dest.top + dest.bottom) * 0.5f;
                    fitted.top = center - height * 0.5f;
                    fitted.bottom = center + height * 0.5f;
                } else if (sourceAspect < destAspect) {
                    const float width = destH * sourceAspect;
                    const float center = (dest.left + dest.right) * 0.5f;
                    fitted.left = center - width * 0.5f;
                    fitted.right = center + width * 0.5f;
                }
                dc->DrawBitmap(item.bitmap.get(), &fitted, std::clamp(opacity, 0.0f, 1.0f),
                    D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
                return PreviewDrawResult::Bitmap;
            }
            if (item.failed) return PreviewDrawResult::Failed;
        }
        if (!pending_.contains(key)) {
            pending_.insert(key);
            queue_.push_front({next_id_++, generation, pixels, attrs,
                               ipc::PreviewRequestKind::Content, direct_preview,
                               path, key, identity});
            if (queue_.size() > 128) { pending_.erase(queue_.back().key); queue_.pop_back(); }
            if (!running_.exchange(true)) worker_ = std::thread([this]{ Worker(); });
            cv_.notify_one();
        }
    }
    return PreviewDrawResult::Pending;
}

bool ThumbnailCache::Properties(const std::wstring& path, DWORD attrs, uint64_t generation,
                                uint64_t modified, uint64_t size,
                                std::vector<PreviewProperty>& properties) {
    if (path.empty()) return false;
    const std::wstring identity = Key(path, 0, modified, size);
    const std::wstring key = identity + L":properties";
    std::lock_guard lock(mutex_);
    if (identity != latest_details_identity_) return false;
    if (auto it = items_.find(key); it != items_.end()) {
        properties = it->second.properties;
        return !it->second.failed;
    }
    if (!pending_.contains(key) && queue_.size() < 128) {
        pending_.insert(key);
        queue_.push_back({next_id_++, generation, 0, attrs,
                          ipc::PreviewRequestKind::Properties, true,
                          path, key, identity});
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
    properties = it->second.properties;
    return true;
}
void ThumbnailCache::Worker() {
    while (running_) {
        Request req;
        { std::unique_lock lock(mutex_); cv_.wait(lock, [&]{return !running_ || !queue_.empty();});
          if (!running_) break; req = std::move(queue_.front()); queue_.pop_front(); }
        Item result; bool ok = Connect();
        ipc::PreviewRequest wire; wire.request_id=req.id; wire.generation=req.generation;
        wire.kind=req.kind; wire.pixel_size=req.pixels; wire.attrs=req.attrs;
        wire.path_chars=(uint32_t)req.path.size();
        if (ok) ok = ipc::WriteAll(pipe_, &wire, sizeof(wire)) &&
                     ipc::WriteAll(pipe_, req.path.data(), wire.path_chars * sizeof(wchar_t));
        ipc::PreviewResponse response{};
        if (ok) {
            const ULONGLONG deadline = GetTickCount64() + 1000;
            DWORD available = 0;
            while (running_ && GetTickCount64() < deadline) {
                if (!PeekNamedPipe(pipe_, nullptr, 0, nullptr, &available, nullptr)) { ok=false; break; }
                if (available >= sizeof(response)) break;
                if (child_.hProcess && WaitForSingleObject(child_.hProcess, 0) == WAIT_OBJECT_0) { ok=false; break; }
                Sleep(10);
            }
            if (available < sizeof(response)) ok=false;
        }
        if (ok) ok = ipc::ReadAll(pipe_, &response, sizeof(response)) &&
                     response.magic == ipc::kPreviewMagic && response.request_id == req.id;
        std::wstring mapping;
        if (ok && response.mapping_chars) { mapping.resize(response.mapping_chars);
            ok = ipc::ReadAll(pipe_, mapping.data(), response.mapping_chars * sizeof(wchar_t)); }
        std::wstring previewText;
        if (ok && response.text_chars) {
            if (response.text_chars > 32768) ok = false;
            else {
                previewText.resize(response.text_chars);
                ok = ipc::ReadAll(pipe_, previewText.data(),
                                  response.text_chars * sizeof(wchar_t));
            }
        }
        std::wstring errorText;
        if (ok && response.error_chars) {
            if (response.error_chars > 512) ok = false;
            else {
                errorText.resize(response.error_chars);
                ok = ipc::ReadAll(pipe_, errorText.data(),
                                  response.error_chars * sizeof(wchar_t));
            }
        }
        std::vector<PreviewProperty> properties;
        for (uint32_t i = 0; ok && i < response.property_count && i < 16; ++i) {
            uint32_t labelChars = 0, valueChars = 0;
            ok = ipc::ReadAll(pipe_, &labelChars, sizeof(labelChars)) && labelChars <= 128;
            PreviewProperty property;
            if (ok && labelChars) {
                property.label.resize(labelChars);
                ok = ipc::ReadAll(pipe_, property.label.data(), labelChars * sizeof(wchar_t));
            }
            if (ok) ok = ipc::ReadAll(pipe_, &valueChars, sizeof(valueChars)) && valueChars <= 1024;
            if (ok && valueChars) {
                property.value.resize(valueChars);
                ok = ipc::ReadAll(pipe_, property.value.data(), valueChars * sizeof(wchar_t));
            }
            if (ok) properties.push_back(std::move(property));
        }
        if (ok && response.status == 0 && !mapping.empty()) {
            HANDLE map = OpenFileMappingW(FILE_MAP_READ, FALSE, mapping.c_str());
            if (map) { const size_t bytes=(size_t)response.stride*response.height;
                if (void* p=MapViewOfFile(map, FILE_MAP_READ,0,0,bytes)) {
                    result.pixels.assign((uint8_t*)p,(uint8_t*)p+bytes); UnmapViewOfFile(p);
                    result.w=response.width; result.h=response.height; result.stride=response.stride;
                } CloseHandle(map); }
        }
        if (ok && response.mapping_chars) { const unsigned char ack=1; ok=ipc::WriteAll(pipe_,&ack,1); }
        if (ok) {
            result.kind = response.kind;
            result.text = std::move(previewText);
            result.error = std::move(errorText);
            result.properties = std::move(properties);
            result.truncated = (response.flags & ipc::kPreviewFlagTruncated) != 0;
            result.bytes_read = response.bytes_read;
        }
        result.cost = result.text.size() * sizeof(wchar_t)
            + result.error.size() * sizeof(wchar_t)
            + static_cast<size_t>(response.stride) * response.height;
        for (const auto& property : result.properties)
            result.cost += (property.label.size() + property.value.size()) * sizeof(wchar_t);
        result.failed = !ok || response.status != 0 ||
            (req.kind == ipc::PreviewRequestKind::Content &&
             result.kind == ipc::PreviewContentKind::Bitmap && result.pixels.empty());
        {
            std::lock_guard lock(mutex_);
            pending_.erase(req.key);
            if (auto old = items_.find(req.key); old != items_.end())
                cache_bytes_ -= (std::min)(cache_bytes_, old->second.cost);
            const bool stale = req.details && req.identity != latest_details_identity_;
            if (!stale) {
                cache_bytes_ += result.cost;
                items_[req.key] = std::move(result);
                lru_.push_back(req.key);
            }
            constexpr size_t kCacheBudget = 96ull * 1024ull * 1024ull;
            while (!lru_.empty() && (lru_.size() > 512 || cache_bytes_ > kCacheBudget)) {
                const std::wstring oldest = std::move(lru_.front());
                lru_.pop_front();
                if (auto item = items_.find(oldest); item != items_.end()) {
                    cache_bytes_ -= (std::min)(cache_bytes_, item->second.cost);
                    items_.erase(item);
                }
            }
        }
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
        if (!ok) StopChild();
    }
}
} // namespace pulse::ui

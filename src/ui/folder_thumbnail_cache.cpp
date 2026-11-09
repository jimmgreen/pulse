#include "folder_thumbnail_cache.h"
#include "thumbnail_artwork_layout.h"
#include <algorithm>

namespace pulse::ui {
FolderThumbnailCache::DebugStats FolderThumbnailCache::ReadDebugStats() {
    DebugStats stats;
    std::lock_guard lock(cache_.mutex_);
    stats.visible = visible_.size(); stats.queued = cache_.queue_.size();
    stats.pending = cache_.pending_.size(); stats.cached = cache_.items_.size();
    stats.running = cache_.running_; stats.has_context = cache_.dc_ != nullptr;
    const auto now = GetTickCount64();
    for (const auto& [key, visible] : visible_) {
        stats.oldest_visible_ms = std::max<uint64_t>(stats.oldest_visible_ms, now - visible.since);
        if (visible.blocked) ++stats.blocked;
    }
    for (const auto& [key, item] : cache_.items_) {
        if (item.failed) ++stats.failed;
        if (item.bitmap.get() || !item.pixels.empty()) ++stats.bitmaps;
        if (stats.error.empty() && !item.error.empty()) stats.error = item.error;
    }
    return stats;
}
FolderThumbnailCache::FolderThumbnailCache() { cache_.SetFolderThumbnail(true); }
void FolderThumbnailCache::Reset() { visible_.clear(); cache_.Reset(); }
void FolderThumbnailCache::Refresh() {
    std::lock_guard lock(cache_.mutex_);
    cache_.epoch_.fetch_add(1, std::memory_order_relaxed);
    cache_.queue_.clear(); cache_.pending_.clear(); cache_.folder_requests_.clear();
    cache_.transient_failures_.clear();
    for (auto& [key, item] : cache_.items_) {
        item.retry_at = 0; item.folder_refresh_complete = false;
    }
    for (auto& [key, visible] : visible_) {
        visible.force = true; visible.attempted = false; visible.blocked = false;
    }
}
void FolderThumbnailCache::BeginFrame() {
    cache_.BeginFrame();
    visibility_changed_ = false;
    for (auto& [key, visible] : visible_) visible.seen = false;
}
void FolderThumbnailCache::EndFrame() {
    std::lock_guard lock(cache_.mutex_);
    for (auto it = visible_.begin(); it != visible_.end();) {
        if (!it->second.seen) {
            visibility_changed_ = true;
            cache_.pending_.erase(it->first);
            cache_.transient_failures_.erase(it->first);
            cache_.folder_requests_.erase(it->first);
            it = visible_.erase(it);
        } else ++it;
    }
    if (visibility_changed_) {
        for (auto& [key, visible] : visible_) {
            visible.blocked = false;
            if (!cache_.items_.contains(key) && !cache_.pending_.contains(key)) {
                visible.attempted = false;
                visible.admitted = false;
            }
        }
    }
    for (auto it = cache_.queue_.begin(); it != cache_.queue_.end();) {
        if (!visible_.contains(it->key)) it = cache_.queue_.erase(it);
        else ++it;
    }
}
bool FolderThumbnailCache::Tick(uint64_t now) {
    std::lock_guard lock(cache_.mutex_);
    for (const auto& [key, visible] : visible_) {
        if (visible.blocked || now < visible.since + kSettleMs || cache_.pending_.contains(key)) continue;
        const auto found = cache_.items_.find(key);
        if (found == cache_.items_.end()) {
            if (!visible.attempted) return true;
        } else if (now >= found->second.retry_at) return true;
    }
    return false;
}
PreviewDrawResult FolderThumbnailCache::Draw(ID2D1DeviceContext* dc,
    const D2D1_RECT_F& dest, const std::wstring& path, DWORD attrs, uint32_t pixel_size,
    uint64_t generation, uint64_t modified, uint64_t size, float opacity,
    bool align_bottom, D2D1_RECT_F* artwork, bool single) {
    if (artwork) *artwork = dest;
    if (!dc || path.empty() || pixel_size < 24) return PreviewDrawResult::Failed;
    const auto now = GetTickCount64();
    const std::wstring identity = path + (single ? L"\nfolder-single" : L"\nfolder-stack");
    const std::wstring key = cache_.Key(identity, pixel_size, modified, size);
    auto [visible, inserted] = visible_.try_emplace(key, Visible{now, generation, true});
    visible->second.seen = true;
    if (inserted) visibility_changed_ = true;
    std::unique_lock lock(cache_.mutex_);
    if (!inserted && visible->second.generation != generation) {
        visibility_changed_ = true;
        visible->second = {now, generation, true, visible->second.force};
        cache_.pending_.erase(key); cache_.folder_requests_.erase(key);
        cache_.transient_failures_.erase(key);
        for (auto it = cache_.queue_.begin(); it != cache_.queue_.end();) {
            if (it->key == key) it = cache_.queue_.erase(it); else ++it;
        }
    }
    visible->second.estimate = static_cast<size_t>(pixel_size) * pixel_size * 8;
    auto found = cache_.items_.find(key);
    if (found != cache_.items_.end() && found->second.folder_refresh_complete) visible->second.force = false;
    if (found != cache_.items_.end() && found->second.failed && visible->second.admitted &&
        !cache_.pending_.contains(key)) {
        visible->second.admitted = false;
        // A released reservation can admit blocked work, but must not reset
        // attempted keys whose negative entries were evicted in this viewport.
        for (auto& [other_key, other] : visible_) other.blocked = false;
    }
    const bool due = found == cache_.items_.end() ? !visible->second.attempted : now >= found->second.retry_at;
    bool admit = due && !visible->second.blocked && now >= visible->second.since + kSettleMs &&
        !cache_.pending_.contains(key);
    if (admit) {
        size_t bytes = visible->second.estimate;
        size_t count = 1;
        for (const auto& [other_key, other] : visible_) {
            if (other_key == key) continue;
            const auto resident = cache_.items_.find(other_key);
            if (other.admitted || (resident != cache_.items_.end() && !resident->second.failed)) {
                bytes += other.estimate;
                ++count;
            }
        }
        admit = bytes <= cache_.budget_bytes_ && count <= cache_.max_items_;
        visible->second.blocked = !admit;
        if (admit) {
            visible->second.attempted = true;
            visible->second.admitted = true;
        }
    }
    if (admit) {
        ThumbnailCache::Request request;
        request.id = cache_.next_id_++; request.generation = generation;
        request.pixels = pixel_size; request.attrs = attrs;
        request.path = path; request.key = key; request.identity = identity;
        request.modified = modified; request.size = size;
        request.epoch = cache_.epoch_.load(std::memory_order_relaxed);
        request.timeout_ms = 1500;
        request.flags = ipc::kPreviewRequestFlagFolderThumbnail;
        if (single) request.flags |= ipc::kPreviewRequestFlagFolderSingle;
        if (visible->second.force) request.flags |= ipc::kPreviewRequestFlagFolderRefresh;
        cache_.folder_requests_[key] = request.id;
        cache_.pending_.insert(key);
        cache_.queue_.push_back(std::move(request));
        if (cache_.queue_.size() > ThumbnailCache::kDefaultMaxItems) {
            const auto dropped = cache_.queue_.front().key;
            cache_.pending_.erase(dropped); cache_.folder_requests_.erase(dropped);
            cache_.queue_.pop_front();
        }
        if (!cache_.running_.exchange(true)) cache_.worker_ = std::thread([this] { cache_.Worker(); });
        cache_.cv_.notify_one();
    }
    std::wstring draw_key = key;
    auto* item = found == cache_.items_.end() ? nullptr : &found->second;
    if (!item || item->failed) {
        const auto previous = cache_.still_by_identity_.find(identity);
        if (previous != cache_.still_by_identity_.end()) {
            const auto old = cache_.items_.find(previous->second);
            if (old != cache_.items_.end() && !old->second.failed) { item = &old->second; draw_key = previous->second; }
        }
    }
    const bool settled_without_artwork = found != cache_.items_.end() ? found->second.failed :
        visible->second.attempted && !cache_.pending_.contains(key);
    lock.unlock();
    cache_.PrepareBitmap(draw_key, true);
    lock.lock();
    const auto ready = cache_.items_.find(draw_key);
    item = ready == cache_.items_.end() ? nullptr : &ready->second;
    if (item && item->bitmap.get()) {
        cache_.Touch(*item);
        const auto fitted = ContainedThumbnailRect(dest, item->w, item->h, item->artwork_bounds, align_bottom);
        if (artwork) *artwork = ThumbnailArtworkRect(fitted, item->artwork_bounds);
        ComPtr<ID2D1Bitmap> bitmap;
        bitmap.p = item->bitmap.get();
        bitmap->AddRef();
        lock.unlock();
        dc->DrawBitmap(bitmap.get(), &fitted, std::clamp(opacity, 0.0f, 1.0f),
            D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        return PreviewDrawResult::Bitmap;
    }
    return settled_without_artwork ? PreviewDrawResult::Failed : PreviewDrawResult::Pending;
}
} // namespace pulse::ui

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
// Keep two eight-item screens even at 512px (CPU recovery + GPU = 2 MiB
// each). RenderCaches offsets this allowance against the ordinary grid budget.
FolderThumbnailCache::FolderThumbnailCache() : cache_(32ull * 1024 * 1024, 256) {
    cache_.SetFolderThumbnail(true);
    cache_.SetFolderParallelism(2);
    cache_.folder_revisit_ = std::make_unique<ThumbnailCache>(size_t{8} * 1024 * 1024, 128);
    cache_.folder_revisit_->SetFolderThumbnail(true);
    cache_.folder_revisit_->preserve_bitmap_on_replace_ = true;
    cache_.folder_revisit_->ShareUploadBudget(cache_);
}
void FolderThumbnailCache::Reset() {
    visible_.clear();
    owner_visible_.clear();
    frame_owner_ = nullptr;
    visibility_changed_ = false;
    cache_.Reset();
}
void FolderThumbnailCache::RemoveNotifyWindow(HWND hwnd) {
    cache_.RemoveNotifyWindow(hwnd);
    std::lock_guard lock(cache_.mutex_);
    owner_visible_.erase(hwnd);
    RetireInvisibleLocked();
}
void FolderThumbnailCache::TrimForBackground(size_t max_bytes) {
    {
        std::lock_guard lock(cache_.mutex_);
        visible_.clear();
        owner_visible_.clear();
        frame_owner_ = nullptr;
        visibility_changed_ = false;
    }
    cache_.TrimToBytes(max_bytes);
}
size_t FolderThumbnailCache::TrimInactiveWorkingSet() {
    size_t freed = 0;
    std::lock_guard lock(cache_.mutex_);
    size_t visible_bytes = 0;
    const bool has_active = !visible_.empty() || !cache_.pending_.empty();
    if (has_active) {
        for (const auto& [key, item] : cache_.items_) {
            if (visible_.contains(key) || cache_.pending_.contains(key) || item.upload_token != 0)
                visible_bytes += item.cost;
        }
    }
    const size_t target_bytes = has_active
        ? (std::min)(cache_.budget_bytes_, visible_bytes + (4u * 1024u * 1024u))
        : 0;
    if (cache_.cache_bytes_ > target_bytes) {
        for (auto it = cache_.lru_.begin(); it != cache_.lru_.end() && cache_.cache_bytes_ > target_bytes;) {
            const auto item_it = cache_.items_.find(*it);
            if (item_it == cache_.items_.end()) {
                it = cache_.lru_.erase(it);
                continue;
            }
            const bool keep_item = visible_.contains(*it) || cache_.pending_.contains(*it) ||
                item_it->second.upload_token != 0;
            if (keep_item) {
                ++it;
                continue;
            }
            const size_t item_cost = (std::min)(cache_.cache_bytes_, item_it->second.cost);
            cache_.cache_bytes_ -= item_cost;
            freed += item_cost;
            cache_.transient_failures_.erase(*it);
            cache_.items_.erase(item_it);
            it = cache_.lru_.erase(it);
        }
        for (auto link = cache_.still_by_identity_.begin(); link != cache_.still_by_identity_.end();) {
            if (cache_.items_.contains(link->second)) ++link;
            else link = cache_.still_by_identity_.erase(link);
        }
    }
    return freed;
}
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
void FolderThumbnailCache::DropFailedEntries() {
    // Folder icon mode changed: every cached folder composite (and the compact
    // revisit copy, which answers Draw before the request path does) belongs to
    // the old mode and must go, or the old picture keeps winning.
    cache_.Evict();
    std::lock_guard lock(cache_.mutex_);
    cache_.epoch_.fetch_add(1, std::memory_order_relaxed);
    for (auto& [key, visible] : visible_) {
        visible.force = true; visible.attempted = false; visible.blocked = false;
        visible.admitted = false; visible.seen = true;
    }
}
void FolderThumbnailCache::BeginFrame(HWND owner, bool full) {
    cache_.BeginFrame();
    visibility_changed_ = false;
    frame_owner_ = owner;
    // Damage-only draws do not enumerate the whole viewport. Other windows
    // also retain their interest until their own full frame or detach.
    if (full) owner_visible_[owner].clear();
}
void FolderThumbnailCache::EndFrame() {
    std::lock_guard lock(cache_.mutex_);
    RetireInvisibleLocked();
}
void FolderThumbnailCache::RetireInvisibleLocked() {
    for (auto& [key, visible] : visible_) visible.seen = false;
    for (const auto& [owner, keys] : owner_visible_) {
        for (const auto& key : keys) {
            if (const auto found = visible_.find(key); found != visible_.end())
                found->second.seen = true;
        }
    }
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
    owner_visible_[frame_owner_].insert(key);
    auto [visible, inserted] = visible_.try_emplace(key, Visible{now, generation, true});
    visible->second.seen = true;
    if (inserted) visibility_changed_ = true;
    std::unique_lock lock(cache_.mutex_);
    const bool shared = std::any_of(owner_visible_.begin(), owner_visible_.end(),
        [&](const auto& owner) { return owner.first != frame_owner_ && owner.second.contains(key); });
    // Window-local generation numbers must not cancel another window's
    // compatible request for the same content-addressed thumbnail key.
    if (!inserted && visible->second.generation != generation &&
        visible->second.generation_owner == frame_owner_ && !shared) {
        visibility_changed_ = true;
        visible->second = {now, generation, true, visible->second.force};
        cache_.pending_.erase(key); cache_.folder_requests_.erase(key);
        cache_.transient_failures_.erase(key);
        for (auto it = cache_.queue_.begin(); it != cache_.queue_.end();) {
            if (it->key == key) it = cache_.queue_.erase(it); else ++it;
        }
    }
    visible->second.generation = generation;
    visible->second.generation_owner = frame_owner_;
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
        cache_.StartWorkersLocked();
        cache_.WakeAllLanesLocked();
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
    auto& revisit = *cache_.folder_revisit_;
    // Keep a GPU-ready compact copy before the full-size entry can be evicted.
    revisit.PrepareBitmap(identity, true);
    {
        std::lock_guard revisit_lock(revisit.mutex_);
        if (auto kept = revisit.items_.find(identity); kept != revisit.items_.end()) revisit.Touch(kept->second);
    }
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
    lock.unlock();
    std::unique_lock revisit_lock(revisit.mutex_);
    if (auto kept = revisit.items_.find(identity); kept != revisit.items_.end() && kept->second.bitmap.get()) {
        auto& compact = kept->second;
        revisit.Touch(compact);
        const auto fitted = ContainedThumbnailRect(dest, compact.w, compact.h, compact.artwork_bounds, align_bottom);
        if (artwork) *artwork = ThumbnailArtworkRect(fitted, compact.artwork_bounds);
        ComPtr<ID2D1Bitmap> bitmap;
        bitmap.p = compact.bitmap.get(); bitmap->AddRef();
        revisit_lock.unlock();
        dc->DrawBitmap(bitmap.get(), &fitted, std::clamp(opacity, 0.0f, 1.0f),
            D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        return PreviewDrawResult::Bitmap;
    }
    return settled_without_artwork ? PreviewDrawResult::Failed : PreviewDrawResult::Pending;
}
} // namespace pulse::ui

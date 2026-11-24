// Included once by index_engine.cpp: resource mutations stay on its worker.
#pragma once
#include "../common/volume_path.h"
namespace pulse::index {
std::shared_ptr<VolumePause> Engine::PauseVolumes(std::vector<std::wstring> ids, std::vector<std::wstring> roots) {
    auto lease = std::make_shared<VolumePause>();
    lease->ids = std::move(ids); lease->roots = std::move(roots);
    {
        std::lock_guard lock(volume_pause_mutex_);
        volume_pause_requests_.push_back(lease);
        pending_pause_requests_.fetch_add(1);
    }
    if (change_signal_) SetEvent(change_signal_);
    return lease;
}
void Engine::ResumeVolumes(const std::shared_ptr<VolumePause>& lease) {
    if (lease) lease->active = false;
    if (change_signal_) SetEvent(change_signal_);
}
bool Engine::VolumePaused(const std::wstring& id) const {
    for (const auto& lease : volume_pauses_)
        for (const auto& value : lease->ids)
            if (NormalizeVolumeId(value) == NormalizeVolumeId(id)) return true;
    return false;
}
bool Engine::VolumePauseRequestedOrActive(std::wstring_view id, std::wstring_view path) {
    const std::wstring norm_id = NormalizeVolumeId(std::wstring(id));
    for (const auto& lease : volume_pauses_) {
        if (!lease->active.load()) continue;
        for (const auto& value : lease->ids)
            if (!norm_id.empty() && NormalizeVolumeId(value) == norm_id) return true;
        if (!path.empty() && pulse::path::OnAnyVolume(path, lease->roots)) return true;
    }
    if (pending_pause_requests_.load() == 0) return false;
    std::lock_guard lock(volume_pause_mutex_);
    for (const auto& lease : volume_pause_requests_) {
        if (!lease->active.load()) continue;
        for (const auto& value : lease->ids)
            if (!norm_id.empty() && NormalizeVolumeId(value) == norm_id) return true;
        if (!path.empty() && pulse::path::OnAnyVolume(path, lease->roots)) return true;
    }
    return false;
}
bool Engine::PathPaused(const std::wstring& path) const {
    for (const auto& lease : volume_pauses_)
        if (pulse::path::OnAnyVolume(path, lease->roots)) return true;
    return false;
}
void Engine::ApplyVolumePauses() {
    const auto before = volume_pauses_.size();
    std::erase_if(volume_pauses_, [](const auto& lease) { return !lease->active.load(); });
    const bool resumed = before != volume_pauses_.size();
    {
        std::lock_guard lock(volume_pause_mutex_);
        for (auto& lease : volume_pause_requests_) if (lease->active) volume_pauses_.push_back(std::move(lease));
        volume_pause_requests_.clear();
        pending_pause_requests_.store(0);
    }
    for (auto it = journal_streams_.begin(); it != journal_streams_.end();) {
        if (VolumePaused(it->first)) it = journal_streams_.erase(it); // joins after cancelling/draining USN I/O
        else ++it;
    }
    for (auto& watch : watches_) {
        const bool handle_open = watch.dir != INVALID_HANDLE_VALUE ||
            (watch.io && watch.io->dir != INVALID_HANDLE_VALUE);
        if (handle_open && PathPaused(watch.path)) CloseWalkWatchHandle(watch);
    }
    for (const auto& lease : volume_pauses_) if (!lease->acknowledged) {
        lease->acknowledged = true;
        lease->released.set_value();
    }
    if (!resumed) return;
    // Retain each journal cursor so missed records are replayed after release.
    for (const auto& volume : vols_) if (!VolumePaused(volume.volume_id) && volume.journal_id &&
        !journal_streams_.contains(volume.volume_id) && !volume_retry_after_.contains(volume.volume_id))
        journal_streams_[volume.volume_id] = std::make_unique<UsnStream>(volume.letter, volume.journal_id, volume.next_usn, change_signal_);
    for (auto& watch : watches_) if (watch.dir == INVALID_HANDLE_VALUE && !PathPaused(watch.path)) {
        if (OpenWalkWatchHandle(watch)) {
            if (!watch.volume_id.empty()) {
                QueueVolumeReconcile(watch.volume_id);
                GapFeed();
            } else {
                // Directory watches have no journal: reconcile on the existing rebuild path.
                rebuild_requested_ = true;
            }
        }
    }
}
}

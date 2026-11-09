#include "drive_eject_ui.h"
#include "app_internal.h"
#include "../ui/ui_renderer.h"
#include "drive_verbs.h"
#include "app_hover_preview.h"
#include "../common/volume_path.h"
#include "../index/index_volume_release.h"
#include <future>
#include <thread>
#include <chrono>

namespace pulse {
struct DriveEjectTask {
    std::wstring root;
    uint8_t type = 0;
    HWND notify = nullptr;
    app::DriveEjectPlan plan;
    DWORD error = ERROR_SUCCESS;
    bool suspended = false;
    std::wstring index_pipe;
    index::VolumeReleaseLease index_release;
};
namespace {
void PostFailure(const std::shared_ptr<DriveEjectTask>& task, DWORD error, bool own_resources = false) {
    auto result = std::make_unique<app::DriveVerbResult>();
    result->root = task->root;
    result->error = error;
    if (own_resources) result->veto_name = L"Pulse";
    if (PostMessageW(task->notify, WM_DRIVE_VERB, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
}
void Append(std::vector<std::shared_future<void>>& to, std::vector<std::shared_future<void>> from) {
    to.insert(to.end(), from.begin(), from.end());
}
}

bool StartPreparedDriveEject(AppState& state, const std::wstring& root, uint8_t type) {
    if (state.core.drive_eject || root.empty()) return false;
    auto task = std::make_shared<DriveEjectTask>();
    task->root = root;
    task->type = type;
    task->notify = ServiceWindow(state);
    task->index_pipe = state.core.index.ReleasePipeName();
    state.core.drive_eject = task;
    try {
        std::thread([task] {
            try { task->error = app::PrepareDriveEject(task->root, task->plan); }
            catch (...) { task->error = ERROR_NOT_ENOUGH_MEMORY; }
            auto payload = std::make_unique<std::shared_ptr<DriveEjectTask>>(task);
            if (PostMessageW(task->notify, WM_DRIVE_EJECT_READY, 0, reinterpret_cast<LPARAM>(payload.get())))
                payload.release();
        }).detach();
        return true;
    } catch (...) {
        state.core.drive_eject.reset();
        return false;
    }
}

void ApplyDriveEjectReady(AppState* state, LPARAM raw) {
    std::unique_ptr<std::shared_ptr<DriveEjectTask>> payload(reinterpret_cast<std::shared_ptr<DriveEjectTask>*>(raw));
    if (!state || !payload || state->core.drive_eject != *payload) return;
    auto task = *payload;
    if (task->error != ERROR_SUCCESS) { PostFailure(task, task->error); return; }
    std::vector<std::shared_future<void>> releases;
    task->suspended = true;
    try {
        for (const auto& root : task->plan.roots) {
            Append(releases, state->watches.SuspendVolume(root));
            releases.push_back(state->folderSizes.SuspendVolume(root));
            auto& caches = state->core.render_caches;
            releases.push_back(caches.thumbnails.SuspendVolume(root));
            releases.push_back(caches.details.SuspendVolume(root));
            releases.push_back(caches.hover.SuspendVolume(root));
            app::ForEachWindowSharingCore(*state, [&](AppState& window) {
                if (path::OnVolume(window.hoverPreview.path, root)) HideHoverPreview(window);
                Append(releases, window.renderer.SuspendVolume(root));
                Append(releases, window.quickPreview.SuspendVolume(root));
                Append(releases, window.detailsPreview.SuspendVolume(root));
                InvalidateRect(window.hwnd, nullptr, FALSE);
            });
        }
        std::thread([task, releases = std::move(releases)] {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            try {
                for (auto& release : releases) {
                    if (!release.valid()) continue;
                    if (release.wait_until(deadline) != std::future_status::ready) {
                        PostFailure(task, ERROR_DEVICE_IN_USE, true);
                        return;
                    }
                    release.get();
                }
            } catch (...) { PostFailure(task, ERROR_DEVICE_IN_USE, true); return; }
            DWORD index_error = ERROR_NOT_ENOUGH_MEMORY;
            try { index_error = task->index_release.Acquire(task->index_pipe, task->plan.roots); }
            catch (...) {}
            // Older index services cannot release proactively. The actual eject still
            // verifies identity and lets the system negotiate safe device removal.
            if (index_error != ERROR_SUCCESS && index_error != ERROR_NOT_SUPPORTED) {
                PostFailure(task, index_error, true); return;
            }
            if (!app::StartDriveEject(task->notify, WM_DRIVE_VERB, task->root, task->type, task->plan.volume_name))
                PostFailure(task, ERROR_NOT_ENOUGH_MEMORY);
        }).detach();
    } catch (...) { PostFailure(task, ERROR_NOT_ENOUGH_MEMORY); }
}

void InheritDriveEjectSuspension(AppState& state) {
    const auto task = state.core.drive_eject;
    if (!task || !task->suspended) return;
    // Called before the new window's first layout/paint. Its private preview
    // objects have not opened files yet; shared caches are already suspended.
    for (const auto& root : task->plan.roots) {
        state.renderer.SuspendVolume(root);
        state.quickPreview.SuspendVolume(root);
        state.detailsPreview.SuspendVolume(root);
    }
}

void CompletePreparedDriveEject(AppState& state, const std::wstring& root) {
    auto task = state.core.drive_eject;
    if (!task || !path::EqualInsensitive(task->root, root)) return;
    task->index_release.Release();
    if (task->suspended) {
        for (const auto& volume : task->plan.roots) {
            state.watches.ResumeVolume(volume);
            state.folderSizes.ResumeVolume(volume);
            auto& caches = state.core.render_caches;
            caches.thumbnails.ResumeVolume(volume);
            caches.details.ResumeVolume(volume);
            caches.hover.ResumeVolume(volume);
            app::ForEachWindowSharingCore(state, [&](AppState& window) {
                window.renderer.ResumeVolume(volume);
                window.quickPreview.ResumeVolume(volume);
                window.detailsPreview.ResumeVolume(volume);
            });
        }
        SyncVisibleWatches(state);
    }
    state.core.drive_eject.reset();
}
}

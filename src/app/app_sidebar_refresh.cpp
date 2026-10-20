#include "app_sidebar_refresh.h"
#include "app_internal.h"
#include "sidebar_refresh_schedule.h"
#include <atomic>
#include <mutex>

namespace pulse {
struct SidebarRefreshLoad {
    std::atomic<bool> cancelled{false};
    std::mutex mutex;
    app::SidebarRefreshSchedule schedule;
    app::SidebarModel result;
    bool ready = false;
    bool rebuilt = false;
    bool succeeded = false;
    bool retry_rebuild = false; // UI thread only
};

namespace {
struct ThreadErrorModeScope {
    DWORD previous = 0;
    BOOL changed = SetThreadErrorMode(SEM_FAILCRITICALERRORS, &previous);
    ~ThreadErrorModeScope() { if (changed) SetThreadErrorMode(previous, nullptr); }
};

struct SidebarComScope {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ~SidebarComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
};

void StartPending(AppState& state) {
    const auto load = state.sidebarRefresh;
    const auto request = load->schedule.Begin();
    if (!request) return;
    const bool rebuild = *request || load->retry_rebuild;
    const auto recycle = state.recycle_info;
    const auto prev_kf = state.sidebar.known_folders;
    auto drives = rebuild ? std::vector<app::SidebarEntry>{} : state.sidebar.drives;
    state.worker.EnqueueIo([load, rebuild, recycle, prev_kf, drives = std::move(drives)]() mutable {
        if (load->cancelled) return;
        app::SidebarModel result;
        bool succeeded = false;
        try {
            ThreadErrorModeScope error_mode;
            if (rebuild) {
                // Known-folder Shell calls run on a pooled thread without a
                // COM apartment; capacity-only updates do not need one.
                SidebarComScope com;
                if (SUCCEEDED(com.result) || com.result == RPC_E_CHANGED_MODE) {
                    result = app::BuildSidebarModel(&recycle, &prev_kf);
                    succeeded = true;
                }
            } else {
                result.known_folders = prev_kf;
                result.drives = std::move(drives);
                app::RefreshSidebarDriveCapacity(result.drives);
                succeeded = true;
            }
        } catch (...) {
            // Publish completion even on failure: WorkerPool's outer catch
            // otherwise leaves the sidebar scheduler permanently busy.
        }
        if (load->cancelled) return;
        std::lock_guard lock(load->mutex);
        load->result = std::move(result);
        load->rebuilt = rebuild;
        load->succeeded = succeeded;
        load->ready = true;
    });
}
} // namespace

void RequestSidebarRefresh(AppState& state, bool rebuild) {
    if (!state.sidebarRefresh) state.sidebarRefresh = std::make_shared<SidebarRefreshLoad>();
    if (state.sidebarRefresh->cancelled) return;
    state.sidebarRefresh->schedule.Request(rebuild);
    StartPending(state);
}

bool TickSidebarRefresh(AppState& state, ULONGLONG now) {
    const auto load = state.sidebarRefresh;
    if (!load || load->cancelled) return false;
    app::SidebarModel result;
    bool ready = false;
    bool rebuilt = false;
    bool succeeded = false;
    {
        std::lock_guard lock(load->mutex);
        ready = load->ready;
        if (ready) {
            result = std::move(load->result);
            rebuilt = load->rebuilt;
            succeeded = load->succeeded;
            load->ready = false;
        }
    }
    bool applied = false;
    if (ready) {
        load->schedule.Complete(now);
        // Keep the last good model, and retry a failed rebuild on the next
        // event or normal low-frequency tick rather than looping immediately.
        if (rebuilt) load->retry_rebuild = !succeeded;
        // A newer language/volume rebuild supersedes the in-flight snapshot.
        if (succeeded && !load->schedule.RebuildPending()) {
            if (rebuilt) {
                result.system_networks = std::move(state.sidebar.system_networks);
                state.sidebar = std::move(result);
                SyncSavedSearchSidebar(state);
                ApplyRecycleOccupancy(state);
            } else state.sidebar.drives = std::move(result.drives);
            applied = true;
        }
    }
    load->schedule.Tick(now);
    StartPending(state);
    return applied;
}

void CancelSidebarRefresh(AppState& state) {
    if (state.sidebarRefresh) state.sidebarRefresh->cancelled = true;
}
} // namespace pulse

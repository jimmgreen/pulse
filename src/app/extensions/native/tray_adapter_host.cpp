#include "tray_adapter_host.h"
#include "../../app_runtime.h"
#include "../../app_state.h"
#include "../../window_registry.h"
#include "../../../common/json_value.h"
#include "../../../fs/fs_snapshot.h"
#include <map>
#include <mutex>
#include <vector>

namespace pulse::extensions::native {
namespace {

struct SharedTrayTracker {
    uint64_t revision = 1;
    uint64_t total_bytes = 0;
    std::vector<TrayEntryData> entries;
    bool initialized = false;
};

std::mutex g_tray_rev_mutex;
auto& GlobalTrayTrackers() {
    static auto* map = new std::map<const app::StagingTray*, SharedTrayTracker>();
    return *map;
}

bool SameTrackedEntries(const std::vector<TrayEntryData>& a, const std::vector<TrayEntryData>& b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].item_id != b[i].item_id ||
            a[i].batch_id != b[i].batch_id ||
            a[i].size_bytes != b[i].size_bytes ||
            a[i].flags != b[i].flags ||
            a[i].path != b[i].path ||
            a[i].name != b[i].name) {
            return false;
        }
    }
    return true;
}

void InvalidateCoreWindows(AppState& app) {
    app::ForEachWindowSharingCore(app, [](AppState& window) {
        if (window.hwnd && IsWindow(window.hwnd)) {
            InvalidateRect(window.hwnd, nullptr, FALSE);
        }
    });
}

} // namespace

TrayBackend MakeTrayBackend(HWND owner) {
    TrayBackend backend;
    backend.valid_owner = [owner] { return IsWindow(owner) != FALSE; };
    backend.snapshot = [owner](TraySnapshotData& out) -> PulseStatus {
        auto* app = GetAppState(owner);
        if (!app || !IsWindow(owner)) return PULSE_ERROR_STATE;
        const auto& tray = app->core.tray;
        out.entries.clear();
        out.total_bytes = 0;
        for (const auto& batch : tray.batches()) {
            out.total_bytes += batch.total_size;
            for (const auto& item : batch.items) {
                TrayEntryData entry;
                entry.item_id = item.id;
                entry.batch_id = batch.id;
                entry.size_bytes = item.size;
                entry.flags = PULSE_TRAY_ITEM_NONE;
                if (item.is_dir) entry.flags |= PULSE_TRAY_ITEM_FOLDER;
                if (batch.move_intent) entry.flags |= PULSE_TRAY_ITEM_MOVE_INTENT;
                if (!item.exists) entry.flags |= PULSE_TRAY_ITEM_MISSING;
                if (item.inflight_task != 0) entry.flags |= PULSE_TRAY_ITEM_IN_FLIGHT;
                entry.path = json::Utf8(item.path);
                entry.name = json::Utf8(TrayItemName(item.path));
                out.entries.push_back(std::move(entry));
            }
        }
        {
            std::lock_guard lock(g_tray_rev_mutex);
            auto& tracker = GlobalTrayTrackers()[&tray];
            if (!tracker.initialized) {
                tracker.initialized = true;
                tracker.total_bytes = out.total_bytes;
                tracker.entries = out.entries;
            } else if (tracker.total_bytes != out.total_bytes ||
                       !SameTrackedEntries(tracker.entries, out.entries)) {
                ++tracker.revision;
                tracker.total_bytes = out.total_bytes;
                tracker.entries = out.entries;
            }
            out.revision = tracker.revision;
        }
        return PULSE_OK;
    };
    backend.collect = [owner](const std::vector<std::wstring>& raw_paths, bool move_intent) -> PulseStatus {
        auto* app = GetAppState(owner);
        if (!app || !IsWindow(owner)) return PULSE_ERROR_STATE;
        std::vector<std::wstring> normalized;
        normalized.reserve(raw_paths.size());
        for (const auto& p : raw_paths) {
            std::wstring norm = fs::NormalizePath(p);
            if (norm.empty()) return PULSE_ERROR_ARGUMENT;
            normalized.push_back(std::move(norm));
        }
        app->core.tray.Collect(normalized, move_intent);
        InvalidateCoreWindows(*app);
        return PULSE_OK;
    };
    backend.remove_item = [owner](uint64_t item_id) -> PulseStatus {
        auto* app = GetAppState(owner);
        if (!app || !IsWindow(owner)) return PULSE_ERROR_STATE;
        const auto& batches = app->core.tray.batches();
        for (size_t b = 0; b < batches.size(); ++b) {
            const auto& items = batches[b].items;
            for (size_t i = 0; i < items.size(); ++i) {
                if (items[i].id == item_id) {
                    app->core.tray.RemoveItem(static_cast<int>(b), static_cast<int>(i));
                    InvalidateCoreWindows(*app);
                    return PULSE_OK;
                }
            }
        }
        return PULSE_ERROR_STATE;
    };
    backend.clear = [owner]() -> PulseStatus {
        auto* app = GetAppState(owner);
        if (!app || !IsWindow(owner)) return PULSE_ERROR_STATE;
        app->core.tray.Clear();
        InvalidateCoreWindows(*app);
        return PULSE_OK;
    };
    backend.release_to_directory = [owner](const std::wstring& destination_dir, uint32_t release_mode) -> PulseStatus {
        auto* app = GetAppState(owner);
        if (!app || !IsWindow(owner)) return PULSE_ERROR_STATE;
        const std::wstring norm_dest = fs::NormalizePath(destination_dir);
        if (norm_dest.empty()) return PULSE_ERROR_ARGUMENT;
        const DWORD attrs = GetFileAttributesW(norm_dest.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            return PULSE_ERROR_ARGUMENT;
        }

        const auto mode = release_mode == PULSE_TRAY_RELEASE_MOVE ? app::TrayReleaseMode::Move
            : release_mode == PULSE_TRAY_RELEASE_BATCH_DEFAULT ? app::TrayReleaseMode::Staged
            : app::TrayReleaseMode::Copy;
        auto plan = app->core.tray.PlanRelease(norm_dest, mode, {}, {}, false);
        if (plan.copies.sources.empty() && plan.moves.sources.empty()) return PULSE_ERROR_STATE;

        auto submit_group = [&](ops::OpType type, const std::vector<std::wstring>& sources,
                                const std::vector<uint64_t>& ids, bool is_move) -> PulseStatus {
            if (sources.empty()) return PULSE_OK;
            ops::OpRequest req;
            req.type = type;
            req.sources = sources;
            req.dest_dir = norm_dest;
            req.collision_policy = ops::CollisionPolicy::KeepBoth;
            const uint64_t task = app->core.ops.Submit(req);
            if (task == 0) return PULSE_ERROR_UNAVAILABLE;
            app->core.tray.MarkInFlight(task, ids, is_move);
            return PULSE_OK;
        };

        const PulseStatus copy_st = submit_group(ops::OpType::Copy, plan.copies.sources, plan.copies.item_ids, false);
        if (copy_st != PULSE_OK) return copy_st;
        const PulseStatus move_st = submit_group(ops::OpType::Move, plan.moves.sources, plan.moves.item_ids, true);
        if (move_st != PULSE_OK) return move_st;

        InvalidateCoreWindows(*app);
        return PULSE_OK;
    };
    return backend;
}

TrayAdapter::TrayAdapter(HWND owner, std::function<bool()> alive,
                         std::shared_ptr<DiagnosticsSink> diagnostics,
                         std::function<void()> wake)
    : TrayAdapter(MakeTrayBackend(owner), std::move(alive), std::move(diagnostics), std::move(wake)) {}

} // namespace pulse::extensions::native

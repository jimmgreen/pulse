#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <functional>
#include <string>
#include <vector>
#include "../ui/file_operation_dialog.h"
namespace pulse {
struct DeletePreflight {
    std::atomic<bool> cancel{false};
    std::atomic<bool> done{false};
    ULONGLONG started_at = 0;
    HWND notify_window = nullptr;
    // Immutable after launch: paths is rewritten by the background probe.
    size_t input_count = 0;
    std::wstring first_path;
    bool preparation_shown = false; // UI thread only
    bool waiting_confirmation = false; // UI thread only
    bool cancelled = false; // UI thread only
    bool terminal = false; // UI thread only when no task was submitted
    bool dispatch_failed = false; // UI launch failure; separate from worker's failed field.
    uint64_t task_id = 0; // First accepted task; extension recycle requests submit one task.
    HWND confirmation = nullptr; // Live native confirmation, UI thread only
    // Optional read-only probe gate for deterministic cancellation tests.
    std::function<void()> before_probe;
    std::function<void(HWND)> confirmation_observer; // Isolated host probes only.
    bool failed = false;
    bool permanent = false;
    bool recycle_only = false;
    bool force_confirmation = false;
    bool offer_recycle = false;
    std::vector<std::wstring> paths;
    std::vector<std::wstring> direct;
    ui::PermanentDeleteInfo::NoBinReason reason = ui::PermanentDeleteInfo::NoBinReason::TooLarge;
};
inline bool ShouldShowDeletePreparation(const DeletePreflight& request, ULONGLONG now) {
    return !request.done.load() && !request.cancel.load() && !request.preparation_shown &&
        now - request.started_at >= 200;
}
inline bool CanSubmitDeletePreflight(const std::shared_ptr<DeletePreflight>& current,
                                    const std::shared_ptr<DeletePreflight>& request) {
    return request && current == request && request->done.load()
        && !request->cancel.load() && !request->failed;
}
void StartDeletePreflight(const std::shared_ptr<DeletePreflight>& request);
}

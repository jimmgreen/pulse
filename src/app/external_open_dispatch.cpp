#include "../common/runtime_log.h"
#include "external_open_dispatch.h"
#include <algorithm>
#include "external_open_wait.h"
#include "app_internal.h"
#include "open_routing.h"
#include "shell_window_sync.h"
#include "explorer_takeover_probe.h"
#include "window_registry.h"
#include "../common/localization.h"

namespace pulse {
namespace {
void CollectFailures(AppState& state) {
    const auto failures = state.single_instance.TakeOpenFailures();
    auto& notice = state.core.open_failure_notice;
    const auto add = [](uint32_t& value, uint32_t count) {
        value += (std::min)(count, UINT32_MAX - value);
    };
    add(notice.pending, failures.timed_out); add(notice.failed, failures.failed);
}
}
void DispatchQueuedExternalOpen(AppState& state) {
    using Coordinator = app::SingleInstanceCoordinator;
    auto& coordinator = state.single_instance;
    auto& notice = state.core.open_failure_notice;
    if (state.core.exiting) { notice.Clear(); return; }
    auto request = coordinator.TakeQueuedOpenRequest(GetTickCount64(), true);
    CollectFailures(state);
    if (!request) return;
    if (request->explorer) {
        bool defer = false;
        try {
            const auto& context = *request->explorer;
            const auto now = GetTickCount64();
            if (context.handoff && !request->explorer_received) {
                ExplorerTakeoverProbeDelivery(state, context.handoff, "receive-attempt");
                request->explorer_received = context.handoff->Receive(now);
                if (request->explorer_received) ExplorerTakeoverProbeDelivery(state, context.handoff, "received");
            }
            auto* target = state.core.windows.MostRecent();
            if (request->explorer_received && target && context.handoff &&
                context.handoff->state == app::HandoffState::Pending && now < context.handoff->deadline_tick) {
                ExplorerTakeoverProbeDelivery(state, context.handoff, "dispatch-before");
                const auto result = HandleExplorerTakeover(*target, context);
                ExplorerTakeoverProbeDelivery(state, context.handoff, "dispatch-after");
                defer = result.status == ExternalOpenStatus::Deferred;
                if (defer) ExplorerTakeoverProbeDelivery(state, context.handoff, "deferred");
                else if (result.status == ExternalOpenStatus::Dispatched && result.target) {
                    ExplorerTakeoverProbeDelivery(state, context.handoff, "dispatched");
                    Coordinator::CompleteDelivery(*request, true);
                }
            }
        } catch (...) { defer = false; }
        // The fixed Explorer deadline and cancellation belong to the context.
        // Failed/expired Explorer transfers keep their source, without ordinary toasts.
        coordinator.FinishOpenClaim(std::move(*request), defer);
        CollectFailures(state);
        return;
    }
    bool defer = false;
    try {
        const auto now = GetTickCount64();
        auto* target = state.core.windows.MostRecent();
        if (!target) notice.Failed(false);
        else if (app::AdvanceOpenWait(request->ui_wait_deadline, now, false) == app::OpenWaitAction::Cancel)
            notice.TimedOut(false);
        else {
            const auto dispatch_begin = GetTickCount64();
            const auto result = OpenExternalFolder(*target, request->path, request->reveal);
            if (request->path == L"pulse:win-e") diagnostics::runtime::Event("win_e_dispatch_timing", {
                {"dispatch_ms", GetTickCount64() - dispatch_begin},
                {"status", static_cast<uint64_t>(result.status)},
                {"visible", result.target && IsWindowVisible(result.target->hwnd)},
                {"minimized", result.target && IsIconic(result.target->hwnd)}});
            if (result.status == ExternalOpenStatus::Deferred) {
                defer = app::AdvanceOpenWait(request->ui_wait_deadline, now, true) == app::OpenWaitAction::Defer;
                if (!defer) notice.TimedOut(false);
            } else if (result.status == ExternalOpenStatus::Dispatched && result.target)
                Coordinator::CompleteDelivery(*request, true);
            else notice.Failed(false);
        }
    } catch (...) { notice.Failed(state.core.exiting); }
    coordinator.FinishOpenClaim(std::move(*request), defer);
    CollectFailures(state);
}
void TickExternalOpen(AppState& state) {
    auto& core = state.core;
    if (core.exiting) { core.open_failure_notice.Clear(); return; }
    if (core.windows.MostRecent() != &state) return;
    const auto now = GetTickCount64();
    if (now - core.open_wait_tick >= 100) {
        core.open_wait_tick = now;
        DispatchQueuedExternalOpen(state);
    }
    auto& notice = core.open_failure_notice;
    const auto counts = notice.Take(IsWindowVisible(state.hwnd) && !IsIconic(state.hwnd),
        ExternalOpenWindowBusy(state), state.notification_toast.IsVisible(), false);
    if (!counts) return;
    const auto detail = counts.timed_out && counts.failed ? l10n::StringId::ExternalOpenMixedFailureDetail :
        counts.failed ? l10n::StringId::ExternalOpenQueueFailedDetail : l10n::StringId::ExternalOpenWaitExpiredDetail;
    // No request path, identity or payload is retained by the UI summary.
    try {
        state.notification_toast.Show(state.hwnd,
            l10n::Get(counts.failed ? l10n::StringId::ExternalOpenQueueFailedTitle : l10n::StringId::ExternalOpenWaitExpiredTitle),
            app::FormatExternalOpenNotice(l10n::Get(detail), counts), true);
    } catch (...) {
        notice.pending += (std::min)(counts.timed_out, UINT32_MAX - notice.pending);
        notice.failed += (std::min)(counts.failed, UINT32_MAX - notice.failed);
    }
}
}

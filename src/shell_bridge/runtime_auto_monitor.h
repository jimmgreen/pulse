#pragma once
#include "runtime_auto_imports.h"
#include "runtime_auto_module_epoch.h"
namespace pulse::shell_bridge::runtime {
// Runs on the initialization worker, never in the loader callback. Every pass
// retains its scanned modules through patching; committed slots are revalidated
// before restoration. A newly loaded module can run before the next pass.
namespace auto_monitor_detail {
inline bool RetryScan(const AutoImportResult& value) noexcept {
    // Install rolls its own transaction back. Never retry over residual writes
    // or page protections, and never turn a real revocation into a retry.
    return !value.patched_slots && value.protection_restored &&
        (value.status == HRESULT_FROM_WIN32(ERROR_BAD_LENGTH) ||
         value.status == HRESULT_FROM_WIN32(ERROR_PARTIAL_COPY) ||
         value.status == HRESULT_FROM_WIN32(ERROR_READ_FAULT));
}
template<class Patch, class Epoch, class Wait>
inline AutoImportResult Monitor(HMODULE owner, std::span<const AutoImportHook> hooks,
    Patch& initial, Epoch& epoch, AutoImportPatch::Permit permit, void* context,
    bool (*enabled)(void*) noexcept, void (*revoke)(void*) noexcept,
    void (*observe)(const AutoImportResult&) noexcept, Wait wait) noexcept {
    std::vector<std::unique_ptr<Patch>> batches;
    std::unique_ptr<Patch> pending;
    bool retry = false;
    HRESULT stopped = HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED);
    try {
        while (enabled(context)) {
            wait();
            if (!enabled(context)) break;
            if (!epoch.Changed() && !retry) continue;
            retry = false;
            AutoImportResult added;
            if (batches.size() >= 256) {
                // Bound retained bookkeeping, not the lifetime of valid hooks.
                // A full monitor still observes revocation and restores on exit.
                added.status = E_OUTOFMEMORY;
            } else {
                pending = std::make_unique<Patch>();
                added = pending->Install(owner, hooks, permit, context);
                if (FAILED(added.status) && added.status != HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND)) {
                    if (!RetryScan(added)) { stopped = added.status; break; }
                    retry = true;
                }
                if (added.status == S_OK) {
                    if (!enabled(context)) break;
                    batches.push_back(std::move(pending));
                    batches.back()->Commit();
                } else pending.reset();
            }
            auto coverage = initial.CurrentCoverage();
            for (const auto& batch : batches) {
                const auto value = batch->CurrentCoverage();
                coverage.api_mask |= value.api_mask; coverage.patched_slots += value.patched_slots;
                coverage.protection_restored &= value.protection_restored;
            }
            if (FAILED(added.status) && added.status != HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND)) coverage.status = added.status;
            coverage.foreign_slots = added.foreign_slots; coverage.skipped_modules = added.skipped_modules;
            observe(coverage);
        }
    } catch (...) { stopped = E_OUTOFMEMORY; }
    revoke(context);
    epoch.Stop();
    AutoImportResult restored; restored.status = stopped; restored.protection_restored = true;
    const auto collect = [&](const AutoImportResult& value) {
        restored.api_mask |= value.api_mask; restored.patched_slots += value.patched_slots;
        restored.protection_restored &= value.protection_restored;
        if (FAILED(value.status)) restored.status = value.status;
    };
    if (pending) collect(pending->Rollback());
    for (auto it = batches.rbegin(); it != batches.rend(); ++it) collect((*it)->RestoreCommitted());
    collect(initial.RestoreCommitted());
    observe(restored);
    return restored;
}
}
inline AutoImportResult MonitorAutoImports(HMODULE owner, std::span<const AutoImportHook> hooks,
    AutoImportPatch& initial, AutoModuleEpoch& epoch, AutoImportPatch::Permit permit, void* context,
    bool (*enabled)(void*) noexcept, void (*revoke)(void*) noexcept,
    void (*observe)(const AutoImportResult&) noexcept) noexcept {
    return auto_monitor_detail::Monitor(owner, hooks, initial, epoch, permit, context,
        enabled, revoke, observe, [] { Sleep(250); });
}
}

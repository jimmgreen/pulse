#pragma once
#include "runtime_auto_imports.h"
#include "runtime_auto_module_epoch.h"
namespace pulse::shell_bridge::runtime {
// Runs on the initialization worker, never in the loader callback. Every pass
// retains its scanned modules through patching; committed slots are revalidated
// before restoration. A newly loaded module can run before the next pass.
inline AutoImportResult MonitorAutoImports(HMODULE owner, std::span<const AutoImportHook> hooks,
    AutoImportPatch& initial, AutoModuleEpoch& epoch, AutoImportPatch::Permit permit, void* context,
    bool (*enabled)(void*) noexcept, void (*revoke)(void*) noexcept,
    void (*observe)(const AutoImportResult&) noexcept) noexcept {
    std::vector<std::unique_ptr<AutoImportPatch>> batches;
    HRESULT stopped = HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED);
    try {
        while (enabled(context)) {
            Sleep(250);
            if (!enabled(context)) break;
            if (!epoch.Changed()) continue;
            if (batches.size() >= 256) { stopped = E_OUTOFMEMORY; break; }
            auto patch = std::make_unique<AutoImportPatch>();
            const auto added = patch->Install(owner, hooks, permit, context);
            if (FAILED(added.status) && added.status != HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND)) { stopped = added.status; break; }
            if (added.status == S_OK) {
                if (!enabled(context)) break;
                batches.push_back(std::move(patch));
                batches.back()->Commit();
            }
            auto coverage = initial.CurrentCoverage();
            for (const auto& batch : batches) {
                const auto value = batch->CurrentCoverage();
                coverage.api_mask |= value.api_mask; coverage.patched_slots += value.patched_slots;
                coverage.protection_restored &= value.protection_restored;
            }
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
    for (auto it = batches.rbegin(); it != batches.rend(); ++it) collect((*it)->RestoreCommitted());
    collect(initial.RestoreCommitted());
    observe(restored);
    return restored;
}
}

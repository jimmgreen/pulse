#ifdef PULSE_AUTO_MONITOR_TEST_HEADER
#include PULSE_AUTO_MONITOR_TEST_HEADER
#else
#include "../shell_bridge/runtime_auto_monitor.h"
#endif
#include <cstdio>
using namespace pulse::shell_bridge::runtime;
namespace {
struct Scenario {
    HRESULT error = S_OK;
    bool residual = false, dirty = false, active = true, revoked = false;
    bool epoch_once = false, initial_restored = false, saw_error_coverage = false;
    int waits = 0, stop_at = 4, installs = 0, rollback_calls = 0, commits = 0;
    int restores = 0, revoke_at = 0;
} *current;
struct Patch {
    bool initial = false;
    AutoImportResult Install(HMODULE, std::span<const AutoImportHook>, AutoImportPatch::Permit, void*) noexcept {
        ++current->installs;
        if (current->installs == 1 && FAILED(current->error))
            return {current->error, 0, current->residual ? 1u : 0u, 0, 0, !current->dirty};
        return {S_OK, 1, 1};
    }
    AutoImportResult Rollback() noexcept {
        ++current->rollback_calls;
        return {current->residual || current->dirty ? E_ACCESSDENIED : S_OK,
            0, current->residual ? 1u : 0u, 0, 0, !current->dirty};
    }
    void Commit() noexcept { ++current->commits; }
    AutoImportResult CurrentCoverage() noexcept { return {S_OK, 1, 1}; }
    AutoImportResult RestoreCommitted() noexcept {
        ++current->restores;
        if (initial) current->initial_restored = true;
        return {S_OK};
    }
};
struct Epoch {
    bool Changed() noexcept { return !current->epoch_once || current->waits == 1; }
    void Stop() noexcept {}
};
bool Enabled(void*) noexcept { return current->active; }
void Revoke(void*) noexcept { current->revoked = true; current->revoke_at = current->waits; }
void Observe(const AutoImportResult& value) noexcept {
    if (!current->revoked && FAILED(value.status) && value.patched_slots && !current->initial_restored)
        current->saw_error_coverage = true;
}
AutoImportResult Run(Scenario& s) {
    current = &s;
    Patch initial; initial.initial = true; Epoch epoch;
    return auto_monitor_detail::Monitor<Patch>(nullptr, {}, initial, epoch, nullptr, nullptr,
        Enabled, Revoke, Observe, [&] { if (++s.waits >= s.stop_at) s.active = false; });
}
int failures = 0;
void Check(bool value, const char* label) {
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label);
    failures += !value;
}
}
int main() {
    for (DWORD error : {ERROR_BAD_LENGTH, ERROR_PARTIAL_COPY, ERROR_READ_FAULT}) {
        Scenario s; s.error = HRESULT_FROM_WIN32(error); s.epoch_once = true;
        Run(s);
        Check(s.installs == 2 && s.commits == 1 && s.saw_error_coverage && s.revoke_at == s.stop_at,
            "transient scan failure retains existing hooks and retries without another epoch");
        Check(s.restores == 2 && s.initial_restored, "real disable restores initial and retry batches");
    }
    {
        Scenario s; s.error = HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED);
        auto r = Run(s);
        Check(s.installs == 1 && s.revoke_at == 1 && r.status == s.error, "permit revocation is never retried");
    }
    for (bool dirty : {false, true}) {
        Scenario s; s.error = HRESULT_FROM_WIN32(ERROR_READ_FAULT); s.residual = !dirty; s.dirty = dirty;
        auto r = Run(s);
        Check(s.installs == 1 && s.rollback_calls == 1 && s.revoke_at == 1 && r.status == E_ACCESSDENIED &&
            (r.patched_slots || !r.protection_restored), "rollback failure is retried at shutdown and remains visible");
    }
    {
        Scenario s; s.stop_at = 260; Run(s);
        Check(s.installs == 256 && s.commits == 256 && s.revoke_at == 260 && s.saw_error_coverage,
            "256 batch limit preserves committed hooks until real disable");
        Check(s.restores == 257, "batch limit still restores every committed batch on disable");
    }
    {
        Scenario s; s.error = E_ACCESSDENIED; auto r = Run(s);
        Check(s.installs == 1 && s.revoke_at == 1 && r.status == E_ACCESSDENIED, "non-transient failure stops monitor");
    }
    return failures ? 1 : 0;
}

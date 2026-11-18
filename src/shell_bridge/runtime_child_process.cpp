#include "runtime_child_process.h"
#include "runtime_child_session.h"
#include "runtime_child_injection.h"
#include <atomic>
#include <memory>
namespace pulse::shell_bridge::runtime {
CreateProcessWFunction original_create_process = &::CreateProcessW;
#if defined(PULSE_RUNTIME_CHILD_ROUTING_TEST)
decltype(&::ResumeThread) test_resume_child_thread = &::ResumeThread;
#endif
namespace {
struct Routing {
    std::wstring control;
    std::string dll;
    ChildRoutingEnabled enabled = nullptr;
    ChildRoutingLog log = nullptr;
    ChildDiagnosticLog diagnostics = nullptr;
};
std::atomic<Routing*> routing{nullptr};
std::atomic<EarlyChildRoutingResolver> early_resolver{nullptr};
thread_local bool creating = false;
struct Scope { bool previous = creating; Scope() { creating = true; } ~Scope() { creating = previous; } };
struct DiagnosticSlot {
    ChildCallDiagnostic event;
    std::atomic<bool> complete{false}, delivered{false};
};
constinit std::array<DiagnosticSlot, kChildDiagnosticCapacity> diagnostic_slots{};
constinit std::atomic<uint64_t> diagnostic_calls{0};
constinit std::atomic<uint64_t> diagnostic_total{0}, diagnostic_w_dropped{0};
constinit std::array<std::atomic<uint64_t>, static_cast<size_t>(ChildCallEntry::Count)> entry_counts{};
constinit std::array<std::atomic<uint64_t>, static_cast<size_t>(ChildCallBranch::Count)> branch_counts{};
std::atomic<bool> diagnostic_published{false};
thread_local bool flushing = false;
thread_local ChildGateReason gate_reason = ChildGateReason::None;
thread_local DWORD gate_error = 0;
void FlushDiagnostics() noexcept {
    const DWORD saved = GetLastError();
    const auto* state = routing.load(std::memory_order_acquire);
    if (!flushing && diagnostic_published.load(std::memory_order_acquire) && state && state->diagnostics) {
        flushing = true;
        for (auto& slot : diagnostic_slots) {
            if (slot.complete.load(std::memory_order_acquire) && !slot.delivered.exchange(true, std::memory_order_acq_rel))
                state->diagnostics(slot.event);
        }
        flushing = false;
    }
    SetLastError(saved);
}
struct Observation {
    ChildCallDiagnostic event;
    explicit Observation(ChildCallEntry entry) : event(BeginChildCallDiagnostic(entry)) {}
    ~Observation() { CompleteChildCallDiagnostic(event); }
    BOOL Returned(BOOL result, DWORD error, const PROCESS_INFORMATION* info) noexcept {
        event.result = result; event.native_error = error;
        // Failed native calls do not promise initialized output.
        if (result && info) event.child_pid = info->dwProcessId;
        return result;
    }
};
void Report(const Routing& state, ChildCallEntry entry, const char* reason, DWORD error, DWORD pid = 0) noexcept {
    if (state.log) state.log(reason, error, pid, entry);
}
struct ResumeOwnSuspension {
    HANDLE thread = nullptr;
    const Routing* state = nullptr;
    DWORD pid = 0;
    ChildCallEntry entry = ChildCallEntry::CreateProcessW;
    ~ResumeOwnSuspension() {
        #if defined(PULSE_RUNTIME_CHILD_ROUTING_TEST)
        const auto resume = test_resume_child_thread;
#else
        const auto resume = &::ResumeThread;
#endif
        if (thread && resume(thread) == DWORD(-1)) Report(*state, entry, "CHILD-RESUME-FAILED", GetLastError(), pid);
    }
};
}
void SetEarlyChildRoutingResolver(EarlyChildRoutingResolver resolver) noexcept {
    early_resolver.store(resolver, std::memory_order_release);
}
bool ConfigureChildRouting(const std::wstring& control, const std::string& dll,
    ChildRoutingEnabled enabled, ChildRoutingLog log, ChildDiagnosticLog diagnostics) noexcept {
    try {
        auto state = std::make_unique<Routing>();
        state->control = control; state->dll = dll; state->enabled = enabled; state->log = log; state->diagnostics = diagnostics;
        Routing* expected = nullptr;
        if (!routing.compare_exchange_strong(expected, state.get())) return false;
        state.release(); return true;
    } catch (...) { return false; }
}
ChildCallDiagnostic BeginChildCallDiagnostic(ChildCallEntry entry) noexcept {
    ChildCallDiagnostic event;
    event.entry = entry;
    event.sequence = diagnostic_total.fetch_add(1, std::memory_order_relaxed) + 1;
    event.entered_tick = GetTickCount64();
    if (entry == ChildCallEntry::CreateProcessW) diagnostic_calls.fetch_add(1, std::memory_order_relaxed);
    if (static_cast<size_t>(entry) < entry_counts.size()) entry_counts[static_cast<size_t>(entry)].fetch_add(1, std::memory_order_relaxed);
    return event;
}
void CompleteChildCallDiagnostic(ChildCallDiagnostic event) noexcept {
    const DWORD saved = GetLastError();
    event.returned_tick = GetTickCount64();
    if (event.entry == ChildCallEntry::CreateProcessW) {
        if (static_cast<size_t>(event.branch) < branch_counts.size()) branch_counts[static_cast<size_t>(event.branch)].fetch_add(1, std::memory_order_relaxed);
        if (event.sequence > kChildDiagnosticCapacity) diagnostic_w_dropped.fetch_add(1, std::memory_order_relaxed);
    }
    if (event.sequence && event.sequence <= diagnostic_slots.size()) {
        auto& slot = diagnostic_slots[static_cast<size_t>(event.sequence - 1)];
        slot.event = event;
        slot.complete.store(true, std::memory_order_release);
    }
    FlushDiagnostics();
    SetLastError(saved);
}
void SetChildGateDiagnostic(ChildGateReason reason, DWORD error) noexcept { gate_reason = reason; gate_error = error; }
ChildDiagnosticSnapshot ReadChildRoutingDiagnostics() noexcept {
    ChildDiagnosticSnapshot snapshot;
    snapshot.calls = diagnostic_calls.load(std::memory_order_relaxed);
    snapshot.dropped = diagnostic_w_dropped.load(std::memory_order_relaxed);
    snapshot.total_calls = diagnostic_total.load(std::memory_order_relaxed);
    snapshot.total_dropped = snapshot.total_calls > kChildDiagnosticCapacity ? snapshot.total_calls - kChildDiagnosticCapacity : 0;
    for (size_t i = 0; i < snapshot.entries.size(); ++i) snapshot.entries[i] = entry_counts[i].load(std::memory_order_relaxed);
    for (size_t i = 0; i < snapshot.branches.size(); ++i) snapshot.branches[i] = branch_counts[i].load(std::memory_order_relaxed);
    for (const auto& slot : diagnostic_slots) if (slot.complete.load(std::memory_order_acquire)) snapshot.events[snapshot.count++] = slot.event;
    return snapshot;
}
void PublishChildRoutingDiagnostics() noexcept {
    diagnostic_published.store(true, std::memory_order_release);
    FlushDiagnostics();
}
BOOL RouteChildCreation(ChildCallEntry entry, void* context, ChildNativeCall invoke,
    ChildEligibility eligibility, DWORD flags, LPVOID environment,
    LPSTARTUPINFOW startup, LPPROCESS_INFORMATION information) noexcept {
    struct NativeError {
        DWORD value = ERROR_SUCCESS;
        bool captured = false;
        ~NativeError() { if (captured) SetLastError(value); }
    } native_error_restore;
    const DWORD incoming_error = GetLastError();
    const bool reentry = creating;
    Scope recursion;
    Observation observation(entry);
    auto native = [&] {
        SetLastError(incoming_error);
        const BOOL result = invoke(context, flags, environment);
        native_error_restore.value = GetLastError(); native_error_restore.captured = true;
        return observation.Returned(result, native_error_restore.value, information);
    };
    const auto* state = routing.load(std::memory_order_acquire);
    if (reentry) { observation.event.branch = ChildCallBranch::Reentry; return native(); }
    Routing early;
    if (!state) {
        if (const auto resolve = early_resolver.load(std::memory_order_acquire)) {
            EarlyChildRouting value;
            if (resolve(value)) {
                early.control.swap(value.control); early.dll.swap(value.dll);
                early.enabled = value.enabled; early.log = value.log; early.diagnostics = value.diagnostics;
                state = &early;
                observation.event.early_resolved = true;
            }
        }
    }
    if (!state) { observation.event.branch = ChildCallBranch::NotConfigured; return native(); }
    if (!state->enabled) { observation.event.branch = ChildCallBranch::MissingEnabled; return native(); }
    gate_reason = ChildGateReason::Unknown; gate_error = ERROR_SUCCESS;
    if (!state->enabled()) {
        observation.event.branch = ChildCallBranch::Disabled;
        observation.event.gate = gate_reason; observation.event.gate_error = gate_error;
        return native();
    }
    observation.event.branch = ChildCallBranch::Routed;
    // Extended attributes remain opaque and unchanged. Actual child token and
    // mitigation policies are checked before any image mutation.
    const DWORD expected_startup = (flags & EXTENDED_STARTUPINFO_PRESENT) ? sizeof(STARTUPINFOEXW) : sizeof(STARTUPINFOW);
    if (!information || !startup || startup->cb != expected_startup ||
        (flags & (DEBUG_PROCESS | DEBUG_ONLY_THIS_PROCESS | CREATE_PROTECTED_PROCESS | CREATE_SECURE_PROCESS))) {
        observation.event.branch = ChildCallBranch::StartupContract;
        Report(*state, entry, "child-startup-contract-native", ERROR_NOT_SUPPORTED); return native();
    }
    if (eligibility) {
        const auto check = eligibility(context, nullptr);
        if (check.error) {
            observation.event.branch = ChildCallBranch::TokenContract;
            observation.event.gate = ChildGateReason::ReplacementToken;
            observation.event.gate_error = check.error; observation.event.token_reason = check.reason;
            Report(*state, entry, "child-token-contract-native", check.error); return native();
        }
    }
    ChildSessionPreparation preparation;
    ChildSessionDiagnostic diagnostic;
    const DWORD prepared = PrepareChildSession(state->control, environment, flags, preparation, diagnostic);
    if (prepared != ERROR_SUCCESS) {
        observation.event.branch = ChildCallBranch::PrepareFailed;
        observation.event.prepare_stage = static_cast<DWORD>(diagnostic.stage);
        observation.event.gate_error = prepared;
        Report(*state, entry, "child-prepare-native", prepared); return native();
    }
    // No exceptions/retries may cross the successful original creation. Caller
    // security/handles/cwd/command and startup are passed through unchanged.
    SetLastError(incoming_error);
    const BOOL created = invoke(context, preparation.EnvironmentFlags() | CREATE_SUSPENDED, preparation.Environment());
    const DWORD native_error = GetLastError();
    native_error_restore.value = native_error; native_error_restore.captured = true;
    observation.Returned(created, native_error, created ? information : nullptr);
    if (!created) { Report(*state, entry, "child-create-failed", native_error); SetLastError(native_error); return created; }
    {
        ResumeOwnSuspension resume{(flags & CREATE_SUSPENDED) ? nullptr : information->hThread, state, information->dwProcessId, entry};
        const auto child_check = eligibility ? eligibility(context, information->hProcess) : ChildEligibilityDiagnostic{};
        if (child_check.error) {
            observation.event.gate = ChildGateReason::ReplacementToken;
            observation.event.gate_error = child_check.error; observation.event.token_reason = child_check.reason;
            Report(*state, entry, "child-created-token-native", child_check.error, information->dwProcessId);
            return created;
        }
        const DWORD bound = preparation.Bind(information->hProcess, information->dwProcessId, diagnostic);
        if (bound != ERROR_SUCCESS) Report(*state, entry, "child-bind-native", bound, information->dwProcessId);
        else {
            const auto injected = InjectChildRuntime(information->hProcess, state->dll.c_str());
            Report(*state, entry, injected.injected ? "child-injected-awaiting-ready" : injected.rollback_failed ?
                "CHILD-ROLLBACK-FAILED" : injected.restored ? "child-restored-native" : "child-preflight-native",
                injected.error, information->dwProcessId);
        }
    }
    SetLastError(native_error);
    return created;
}
BOOL WINAPI HookCreateProcessW(LPCWSTR application, LPWSTR command, LPSECURITY_ATTRIBUTES process_attributes,
    LPSECURITY_ATTRIBUTES thread_attributes, BOOL inherit_handles, DWORD flags, LPVOID environment,
    LPCWSTR directory, LPSTARTUPINFOW startup, LPPROCESS_INFORMATION information) noexcept {
    struct Arguments {
        LPCWSTR application; LPWSTR command; LPSECURITY_ATTRIBUTES process_attributes, thread_attributes;
        BOOL inherit; LPCWSTR directory; LPSTARTUPINFOW startup; LPPROCESS_INFORMATION information;
    } args{application, command, process_attributes, thread_attributes, inherit_handles, directory, startup, information};
    const auto invoke = [](void* opaque, DWORD call_flags, LPVOID call_environment) noexcept -> BOOL {
        const auto& a = *static_cast<Arguments*>(opaque);
        return original_create_process(a.application, a.command, a.process_attributes, a.thread_attributes,
            a.inherit, call_flags, call_environment, a.directory, a.startup, a.information);
    };
    return RouteChildCreation(ChildCallEntry::CreateProcessW, &args, invoke, nullptr, flags, environment, startup, information);
}

}

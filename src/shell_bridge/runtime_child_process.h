#pragma once
#include <windows.h>
#include <string>
#include <array>
#include <cstddef>
#include <cstdint>
namespace pulse::shell_bridge::runtime {
using CreateProcessWFunction = decltype(&::CreateProcessW);
extern CreateProcessWFunction original_create_process;
using ChildRoutingEnabled = bool (*)() noexcept;

enum class ChildCallBranch : uint32_t { Routed, Reentry, NotConfigured, MissingEnabled, Disabled, StartupContract, PrepareFailed, ObserveOnly, TokenContract, Count };
enum class ChildCallEntry : uint32_t { CreateProcessW, CreateProcessA, CreateProcessAsUserW, CreateProcessWithTokenW, CreateProcessWithLogonW, Count };
enum class ChildGateReason : uint32_t { None, Unknown, NotPublished, Revoked, MarkerPresent, MarkerQuery, ControlMissing, ParentChain, Exception, ReplacementToken };
using ChildRoutingLog = void (*)(const char*, DWORD, DWORD, ChildCallEntry) noexcept;
struct ChildCallDiagnostic {
    uint64_t sequence = 0, entered_tick = 0, returned_tick = 0;
    ChildCallEntry entry = ChildCallEntry::CreateProcessW;
    ChildCallBranch branch = ChildCallBranch::NotConfigured;
    ChildGateReason gate = ChildGateReason::None;
    DWORD gate_error = 0, native_error = 0, child_pid = 0, prepare_stage = 0, token_reason = 0;
    BOOL result = FALSE;
    bool early_resolved = false;
};
inline constexpr size_t kChildDiagnosticCapacity = 64;
struct ChildDiagnosticSnapshot {
    uint64_t calls = 0, dropped = 0; // W only; observers never increase these.
    uint64_t total_calls = 0, total_dropped = 0;
    std::array<uint64_t, static_cast<size_t>(ChildCallEntry::Count)> entries{};
    std::array<uint64_t, static_cast<size_t>(ChildCallBranch::Count)> branches{};
    std::array<ChildCallDiagnostic, kChildDiagnosticCapacity> events{};
    size_t count = 0;
};
// Shared bounded observation queue. Begin/Complete do not authorize routing or
// alter W recursion state. Each Begin must be completed once by its stack owner.
ChildCallDiagnostic BeginChildCallDiagnostic(ChildCallEntry) noexcept;
void CompleteChildCallDiagnostic(ChildCallDiagnostic) noexcept;
using ChildDiagnosticLog = void (*)(const ChildCallDiagnostic&) noexcept;
// Callback-local diagnostic only; it never authorizes or changes routing.
void SetChildGateDiagnostic(ChildGateReason, DWORD) noexcept;
ChildDiagnosticSnapshot ReadChildRoutingDiagnostics() noexcept;
// Only after runtime readiness publication: drains completed, immutable entries.
// Never waits for an in-flight native call. No command/environment/path data.
void PublishChildRoutingDiagnostics() noexcept;
// The resolver is installed as a function pointer only under loader lock.
// It runs on the creation caller, never waits for the initialization worker,
// and must validate a complete bound session before returning configuration.
struct EarlyChildRouting {
    std::wstring control;
    std::string dll;
    ChildRoutingEnabled enabled = nullptr;
    ChildRoutingLog log = nullptr;
    ChildDiagnosticLog diagnostics = nullptr;
};
using EarlyChildRoutingResolver = bool (*)(EarlyChildRouting&) noexcept;
void SetEarlyChildRoutingResolver(EarlyChildRoutingResolver) noexcept;
// Immutable process-lifetime configuration, published before runtime readiness.
bool ConfigureChildRouting(const std::wstring& control, const std::string& dll,
    ChildRoutingEnabled enabled, ChildRoutingLog log, ChildDiagnosticLog diagnostics = nullptr) noexcept;
// Internal shared creation transaction. The adapter owns the argument context;
// only the existing suspension/environment policy is supplied to its one call.
using ChildNativeCall = BOOL (*)(void*, DWORD, LPVOID) noexcept;
struct ChildEligibilityDiagnostic { DWORD error = 0, reason = 0; };
using ChildEligibility = ChildEligibilityDiagnostic (*)(void*, HANDLE) noexcept;
BOOL RouteChildCreation(ChildCallEntry entry, void* context, ChildNativeCall native,
    ChildEligibility eligibility, DWORD flags, LPVOID environment,
    LPSTARTUPINFOW startup, LPPROCESS_INFORMATION information) noexcept;
#if defined(PULSE_RUNTIME_CHILD_ROUTING_TEST)
extern decltype(&::ResumeThread) test_resume_child_thread;
#endif
BOOL WINAPI HookCreateProcessW(LPCWSTR application, LPWSTR command, LPSECURITY_ATTRIBUTES process_attributes,
    LPSECURITY_ATTRIBUTES thread_attributes, BOOL inherit_handles, DWORD flags, LPVOID environment,
    LPCWSTR directory, LPSTARTUPINFOW startup, LPPROCESS_INFORMATION information) noexcept;
}

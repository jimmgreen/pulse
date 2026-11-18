#pragma once
#include <windows.h>
#include "explorer_window_takeover.h"
#include <array>
#include <mutex>
#include <string_view>

namespace pulse::app {
inline constexpr wchar_t kExplorerScopeOwner[] = L"Pulse.ExplorerScope.TestOwner";
inline constexpr uint32_t kExplorerScopeExternalEmptySelection = 1;
struct ExplorerScopeEvidence {
    HANDLE close_authorized_event = nullptr; // Opened only by the fully gated test probe.
    ~ExplorerScopeEvidence() { if (close_authorized_event) CloseHandle(close_authorized_event); }
    std::atomic<bool> quit{false}, stopped{false};
    std::atomic<bool> thread_created{false}, stop_requested{false};
    std::atomic<DWORD> thread_id{0};
    std::atomic<long> shell_count{-1}, shell_index{-1}, result{E_PENDING};
    std::atomic<const char*> stage{"not-created"};
    struct Trace { uint64_t tick; const char* stage; long result; };
    std::array<Trace, 64> trace{};
    size_t trace_count = 0;
    std::array<Trace, 32> handoff_trace{};
    size_t handoff_trace_count = 0;
    std::mutex mutex;
    ExplorerTakeoverRequest submitted;
};
struct ExplorerScopeToken {
    uint32_t magic = 0x50585345, version = 1;
    uint64_t hwnd = 0, created = 0, owner = 0;
    uint32_t pid = 0, tid = 0, busy = 0, reserved = 0;
};
struct ExplorerSourceScope {
    ExplorerScopeToken token;
    std::wstring folder;
    std::shared_ptr<ExplorerScopeEvidence> evidence = std::make_shared<ExplorerScopeEvidence>();
};
inline std::wstring ExplorerScopeCloseEventName(const ExplorerScopeToken& token) {
    return L"Local\\Pulse.ExplorerClose." + std::to_wstring(token.owner) + L"." +
        std::to_wstring(token.hwnd) + L"." + std::to_wstring(token.pid) + L"." + std::to_wstring(token.created);
}
// Stage names are static literals. Bounded diagnostics never retain UI or COM.
inline void TraceExplorerScope(const std::shared_ptr<const ExplorerSourceScope>& scope, const char* stage, HRESULT result = E_PENDING) {
    if (!scope) return;
    auto& evidence = *scope->evidence;
    evidence.stage = stage; evidence.result = result;
    std::lock_guard lock(evidence.mutex);
    evidence.trace[evidence.trace_count++ % evidence.trace.size()] = {GetTickCount64(), stage, result};
    if (std::string_view(stage).starts_with("handoff-"))
        evidence.handoff_trace[evidence.handoff_trace_count++ % evidence.handoff_trace.size()] = {GetTickCount64(), stage, result};
}
inline bool ExplorerScopeIdentity(const ExplorerScopeToken& expected, const ExplorerScopeToken& actual) {
    return expected.hwnd && expected.created && expected.pid && expected.tid && expected.owner &&
        expected.hwnd == actual.hwnd && expected.pid == actual.pid && expected.tid == actual.tid &&
        expected.created == actual.created && expected.owner == actual.owner;
}
std::wstring CanonicalExplorerScopePath(const std::wstring& path);
// Window identity is separate from the stricter folder/tab close check.
bool ExplorerScopeWindow(const ExplorerSourceScope& scope, HWND window);
inline bool ExplorerScopeView(const ExplorerSourceScope& scope, bool identity,
        const std::wstring& canonical_folder, size_t views, int tabs) {
    return identity && !canonical_folder.empty() &&
        _wcsicmp(scope.folder.c_str(), canonical_folder.c_str()) == 0 && views == 1 && tabs == 1;
}
}

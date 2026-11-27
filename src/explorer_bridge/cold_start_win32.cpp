#include "cold_start.h"
#include "../common/current_user_security.h"
#include "../common/dialog_admin_trust.h"
#include "../common/dialog_bridge_paths.h"
#include "../common/dialog_bundle.h"
#include "../common/explorer_launch_request.h"
#include <tlhelp32.h>
#include <array>
#include <filesystem>

namespace pulse::explorer_bridge::cold_start {
namespace {
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
bool SamePath(const std::wstring& left, const std::wstring& right) {
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}
DWORD ExistingHost(const std::wstring& host, bool& found) {
    found = false;
    DWORD session = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) return GetLastError();
    const auto sid = CurrentUserSidString();
    if (sid.empty()) return ERROR_ACCESS_DENIED;
    Handle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
    if (snapshot.value == INVALID_HANDLE_VALUE) return GetLastError();
    PROCESSENTRY32W entry{sizeof(entry)};
    if (!Process32FirstW(snapshot.value, &entry)) return GetLastError();
    do {
        if (_wcsicmp(entry.szExeFile, L"pulse.exe")) continue;
        DWORD target_session = 0;
        if (!ProcessIdToSessionId(entry.th32ProcessID, &target_session)) continue;
        if (target_session != session) continue;
        Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, entry.th32ProcessID)};
        if (!process.value) {
            const DWORD error = GetLastError();
            if (error == ERROR_INVALID_PARAMETER) continue;
            return error;
        }
        if (WaitForSingleObject(process.value, 0) != WAIT_TIMEOUT) continue;
        const auto process_sid = ProcessUserSidString(process.value);
        if (process_sid.empty()) return ERROR_ACCESS_DENIED;
        if (process_sid != sid) continue;
        std::array<wchar_t, 32768> path{};
        DWORD length = static_cast<DWORD>(path.size());
        if (!QueryFullProcessImageNameW(process.value, 0, path.data(), &length)) return GetLastError();
        if (SamePath(host, std::wstring(path.data(), length))) { found = true; return ERROR_SUCCESS; }
    } while (Process32NextW(snapshot.value, &entry));
    const DWORD error = GetLastError();
    return error == ERROR_NO_MORE_FILES ? ERROR_SUCCESS : error;
}
class NativeHooks final : public Hooks {
public:
    uint64_t Now() noexcept override { return GetTickCount64(); }
    void Pause(DWORD milliseconds) noexcept override { Sleep(milliseconds); }
    EndpointState Find() noexcept override {
        const auto endpoint = direct_reveal_detail::FindPrimaryEndpoint({});
        if (endpoint) return endpoint->pipe_name.empty() ? EndpointState::Starting : EndpointState::Ready;
        // The production singleton intentionally keeps its legacy, session-local
        // name. Presence includes an instance that has not published its endpoint.
        Handle singleton{OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\Pulse.Singleton")};
        if (singleton.value || GetLastError() != ERROR_FILE_NOT_FOUND) return EndpointState::Starting;
        return EndpointState::Absent;
    }
    DWORD Acquire(DWORD timeout_ms) noexcept override {
        try {
            const auto base = direct_reveal_detail::EndpointName({});
            if (base.empty()) return ERROR_ACCESS_DENIED;
            CurrentUserSecurityAttributes security;
            if (!security) return ERROR_ACCESS_DENIED;
            mutex_.value = CreateMutexW(security.get(), FALSE, (base + L".BrowseColdStart").c_str());
            if (!mutex_.value) return GetLastError();
            const DWORD result = WaitForSingleObject(mutex_.value, timeout_ms);
            if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) { held_ = true; return ERROR_SUCCESS; }
            return result == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
        } catch (...) { return ERROR_NOT_ENOUGH_MEMORY; }
    }
    void Release() noexcept override {
        if (held_) { ReleaseMutex(mutex_.value); held_ = false; }
    }
    ~NativeHooks() override { Release(); }
    DWORD Start() noexcept override {
        try {
            namespace paths = dialog_bridge_paths;
            namespace trust = dialog_admin;
            const auto common = paths::CommonDialogBridgeRoot();
            if (common.empty()) return ERROR_PATH_NOT_FOUND;
            DWORD error = trust::VerifyProtectedPath(common, true);
            if (!error) error = trust::VerifyProtectedPath(common + L"\\active-bridge.json", false);
            if (error) return error;
            paths::ActiveBridgePointer pointer;
            if ((error = paths::ReadActiveBridgePointer(common, pointer)) != ERROR_SUCCESS) return error;
            if (pointer.disabled) return ERROR_ACCESS_DISABLED_BY_POLICY;
            if (pointer.install_root.empty() || pointer.host_exe.empty()) return ERROR_INVALID_DATA;
            error = trust::VerifyProtectedPath(pointer.install_root, true);
            if (!error) error = trust::VerifyProtectedPath(pointer.install_root + L"\\app-current.json", false);
            if (!error) error = dialog_bundle::ResolveInstalledAppRuntime(pointer.install_root, lease_);
            if (!error) error = trust::VerifyProtectedTree(lease_.Directory());
            if (error) return error;
            if (!SamePath(lease_.Host(), pointer.host_exe)) return ERROR_REVISION_MISMATCH;
            bool existing = false;
            error = ExistingHost(lease_.Host(), existing);
            if (error || existing) return error;
            if (Find() != EndpointState::Absent) return ERROR_SUCCESS;
            // This is the existing sign-in policy, not a new hidden-mode flag:
            // users without start_in_tray may see their startup/session window.
            auto command = explorer_launch::Quote(lease_.Host()) + L" --startup";
            STARTUPINFOW startup{sizeof(startup)};
            startup.dwFlags = STARTF_USESHOWWINDOW;
            startup.wShowWindow = SW_SHOWNORMAL;
            PROCESS_INFORMATION process{};
            if (!CreateProcessW(lease_.Host().c_str(), command.data(), nullptr, nullptr, FALSE,
                    CREATE_UNICODE_ENVIRONMENT, nullptr, lease_.Directory().c_str(), &startup, &process)) return GetLastError();
            CloseHandle(process.hThread);
            process_.value = process.hProcess;
            AllowSetForegroundWindow(process.dwProcessId);
            return ERROR_SUCCESS;
        } catch (...) { return ERROR_NOT_ENOUGH_MEMORY; }
    }
    Outcome Submit(const browse::Request& request, bool wait_for_ready, uint64_t deadline_tick) noexcept override {
        const auto now = Now();
        if (now >= deadline_tick) return {DirectRevealResult::Rejected, ERROR_TIMEOUT};
        const auto remaining = deadline_tick - now;
        const DWORD timeout = static_cast<DWORD>(remaining > 60000 ? 60000 : remaining);
        DirectRevealDiagnostics diagnostics;
        const auto result = direct_reveal_detail::SendDirectRequest(
            request.intent == browse::Intent::Reveal ? 3u : 4u, request.path, timeout, {},
            &diagnostics, wait_for_ready, deadline_tick);
        const DWORD error = result == DirectRevealResult::Accepted ? ERROR_SUCCESS :
            result == DirectRevealResult::Indeterminate || diagnostics.deadline_expired ? ERROR_TIMEOUT : ERROR_NOT_READY;
        return {result, error};
    }
private:
    Handle mutex_, process_;
    bool held_ = false;
    dialog_bundle::BundleLease lease_;
};
}
Outcome Deliver(const browse::Request& request, DWORD ready_timeout_ms) noexcept {
    try {
        if (request.intent != browse::Intent::OpenFolder && request.intent != browse::Intent::Reveal)
            return {DirectRevealResult::Unsupported, ERROR_NOT_SUPPORTED};
        const auto explorer = explorer_launch::SystemExplorer();
        const bool select = request.intent == browse::Intent::Reveal;
        const auto command = explorer_launch::Quote(explorer) + (select ? L" /select, " : L" ") +
            explorer_launch::Quote(request.path);
        explorer_launch::Request parsed;
        if (!explorer_launch::Resolve(explorer.c_str(), command.c_str(), CREATE_UNICODE_ENVIRONMENT, parsed) ||
            parsed.path != request.path || parsed.select != select)
            return {DirectRevealResult::Unsupported, ERROR_NOT_SUPPORTED};
        const DWORD attributes = GetFileAttributesW(request.path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) return {DirectRevealResult::Rejected, GetLastError()};
        if (!select && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return {DirectRevealResult::Unsupported, ERROR_DIRECTORY};
        NativeHooks hooks;
        return Deliver(request, hooks, ready_timeout_ms);
    } catch (...) { return {DirectRevealResult::Rejected, ERROR_NOT_ENOUGH_MEMORY}; }
}
}

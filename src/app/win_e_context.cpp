#include "win_e_context.h"
#include "../common/runtime_log.h"
#include <array>
#include <string>
#include <utility>

namespace pulse::app {
namespace {
namespace log = diagnostics::runtime;
// The logger accepts initializer_list but caps numeric fields at twelve.
// Array deduction plus this expansion keeps the bound a compile-time contract.
template<size_t N, size_t... I>
void Emit(const char* event, const log::Field (&fields)[N], std::index_sequence<I...>) {
    static_assert(N <= 12);
    log::Event(event, {fields[I]...});
}
template<size_t N>
void Emit(const char* event, const log::Field (&fields)[N]) {
    Emit(event, fields, std::make_index_sequence<N>{});
}
template<size_t N, size_t... I>
void EmitProgram(const std::wstring& name, const log::Field (&fields)[N], std::index_sequence<I...>) {
    static_assert(N <= 12);
    log::TextEvent("win_e_context_process", {{"program", name}}, {fields[I]...});
}
template<size_t N>
void EmitProgram(const std::wstring& name, const log::Field (&fields)[N]) {
    EmitProgram(name, fields, std::make_index_sequence<N>{});
}
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value) CloseHandle(value); }
};
struct Security {
    DWORD token_error = 0, elevation_error = 0, integrity_error = 0;
    DWORD elevated = 0, integrity = 0;
};
Security ReadSecurity(HANDLE process) {
    Security result;
    Handle token;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token.value)) {
        result.token_error = GetLastError();
        return result;
    }
    TOKEN_ELEVATION elevation{};
    DWORD bytes = 0;
    if (GetTokenInformation(token.value, TokenElevation, &elevation, sizeof(elevation), &bytes))
        result.elevated = elevation.TokenIsElevated;
    else result.elevation_error = GetLastError();
    // A mandatory label is one SID; bounded stack storage avoids a size-query
    // round trip and unbounded allocation in the observer.
    alignas(TOKEN_MANDATORY_LABEL) std::array<unsigned char, sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE> buffer{};
    if (!GetTokenInformation(token.value, TokenIntegrityLevel, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes)) {
        result.integrity_error = GetLastError();
        return result;
    }
    const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer.data());
    if (!IsValidSid(label->Label.Sid) || !*GetSidSubAuthorityCount(label->Label.Sid)) {
        result.integrity_error = ERROR_INVALID_SID;
        return result;
    }
    result.integrity = *GetSidSubAuthority(label->Label.Sid, *GetSidSubAuthorityCount(label->Label.Sid) - 1);
    return result;
}
uint64_t FileTimeValue(FILETIME value) {
    return (static_cast<uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
}
// *_i32 coordinates retain the exact signed LONG as a 32-bit bit pattern,
// because runtime::Field is unsigned. Consumers reinterpret them as int32.
uint64_t Coordinate(LONG value) { return static_cast<uint32_t>(value); }
}

uint64_t WinEContextObserver::Poll(HWND foreground, uint64_t now, bool force) {
    DWORD pid = 0;
    SetLastError(ERROR_SUCCESS);
    const DWORD thread = foreground ? GetWindowThreadProcessId(foreground, &pid) : 0;
    const DWORD window_error = thread ? ERROR_SUCCESS : foreground ? GetLastError() : ERROR_INVALID_WINDOW_HANDLE;
    if (!force && CurrentContextId() && foreground == foreground_ && pid == pid_) return CurrentContextId();
    foreground_ = foreground;
    pid_ = pid;
    const uint64_t id = log::NextId();
    Handle process;
    DWORD process_error = 0, image_error = 0, creation_error = 0;
    uint64_t creation = 0;
    std::wstring program;
    Security security;
    if (pid) {
        process.value = open_process_(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process.value) process_error = GetLastError();
    } else process_error = ERROR_INVALID_PARAMETER;
    if (process.value) {
        std::array<wchar_t, 32768> path{};
        DWORD length = static_cast<DWORD>(path.size());
        if (QueryFullProcessImageNameW(process.value, 0, path.data(), &length)) {
            const std::wstring_view full(path.data(), length);
            const auto slash = full.find_last_of(L"\\/");
            program.assign(full.substr(slash == std::wstring_view::npos ? 0 : slash + 1));
        } else image_error = GetLastError();
        FILETIME created{}, exited{}, kernel{}, user{};
        if (GetProcessTimes(process.value, &created, &exited, &kernel, &user)) creation = FileTimeValue(created);
        else creation_error = GetLastError();
        security = ReadSecurity(process.value);
    } else security.token_error = process_error;
    const Security self = ReadSecurity(GetCurrentProcess());
    EmitProgram(program, {{"context_id", id}, {"tick", now}, {"pid", pid}, {"thread_id", thread},
        {"creation_time", creation}, {"window_error", window_error}, {"process_error", process_error},
        {"image_error", image_error}, {"creation_error", creation_error}});
    Emit("win_e_context_security", {{"context_id", id}, {"elevated", security.elevated},
        {"integrity", security.integrity}, {"token_error", security.token_error},
        {"elevation_error", security.elevation_error}, {"integrity_error", security.integrity_error},
        {"self_integrity", self.integrity}, {"self_token_error", self.token_error},
        {"self_integrity_error", self.integrity_error}});
    RECT rect{};
    SetLastError(ERROR_SUCCESS);
    const bool rectangle = foreground && GetWindowRect(foreground, &rect);
    const DWORD rectangle_error = rectangle ? ERROR_SUCCESS : foreground ? GetLastError() : ERROR_INVALID_WINDOW_HANDLE;
    Emit("win_e_context_window", {{"context_id", id}, {"hwnd", reinterpret_cast<uintptr_t>(foreground)},
        {"rect_error", rectangle_error}, {"left_i32", Coordinate(rect.left)}, {"top_i32", Coordinate(rect.top)},
        {"right_i32", Coordinate(rect.right)}, {"bottom_i32", Coordinate(rect.bottom)},
        {"visible", foreground && IsWindowVisible(foreground)}, {"minimized", foreground && IsIconic(foreground)}});
    const HMONITOR monitor = rectangle ? MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST) : nullptr;
    MONITORINFO info{sizeof(info)};
    SetLastError(ERROR_SUCCESS);
    const bool monitor_ok = monitor && GetMonitorInfoW(monitor, &info);
    const DWORD monitor_error = monitor_ok ? ERROR_SUCCESS : monitor ? GetLastError() : ERROR_INVALID_MONITOR_HANDLE;
    Emit("win_e_context_monitor", {{"context_id", id}, {"monitor_error", monitor_error},
        {"left_i32", Coordinate(info.rcMonitor.left)}, {"top_i32", Coordinate(info.rcMonitor.top)},
        {"right_i32", Coordinate(info.rcMonitor.right)}, {"bottom_i32", Coordinate(info.rcMonitor.bottom)},
        {"work_left_i32", Coordinate(info.rcWork.left)}, {"work_top_i32", Coordinate(info.rcWork.top)},
        {"work_right_i32", Coordinate(info.rcWork.right)}, {"work_bottom_i32", Coordinate(info.rcWork.bottom)},
        {"primary", (info.dwFlags & MONITORINFOF_PRIMARY) != 0}});
    context_id_.store(id, std::memory_order_release);
    return id;
}
}

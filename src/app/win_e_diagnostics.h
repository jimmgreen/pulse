#pragma once
#include "win_e_foreground.h"
#include "../common/runtime_log.h"

namespace pulse::app {
// Keep this bounded to the logger's 12-field contract. Geometry is a later
// observation, not a reason to surrender an accepted shortcut to Explorer.
inline void LogWinEForeground(bool accepted, DWORD pid, bool known,
        const RECT& bounds, const RECT& monitor) noexcept {
    diagnostics::runtime::Event("win_e_foreground", {{"accepted", accepted},
        {"kind_at_log", static_cast<uint64_t>(ClassifyWinEForeground(known, bounds, monitor))},
        {"foreground_pid", pid}, {"geometry_at_log", 1},
        {"window_left_u32", static_cast<uint32_t>(bounds.left)},
        {"window_top_u32", static_cast<uint32_t>(bounds.top)},
        {"window_right_u32", static_cast<uint32_t>(bounds.right)},
        {"window_bottom_u32", static_cast<uint32_t>(bounds.bottom)},
        {"monitor_left_u32", static_cast<uint32_t>(monitor.left)},
        {"monitor_top_u32", static_cast<uint32_t>(monitor.top)},
        {"monitor_right_u32", static_cast<uint32_t>(monitor.right)},
        {"monitor_bottom_u32", static_cast<uint32_t>(monitor.bottom)}});
}
inline void LogWinEGate(uint32_t flags, DWORD pid) noexcept {
    DWORD elevated = 0, error = ERROR_SUCCESS;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process) {
        HANDLE token = nullptr;
        if (OpenProcessToken(process, TOKEN_QUERY, &token)) {
            TOKEN_ELEVATION value{};
            DWORD size = 0;
            if (GetTokenInformation(token, TokenElevation, &value, sizeof(value), &size))
                elevated = value.TokenIsElevated;
            else error = GetLastError();
            CloseHandle(token);
        } else error = GetLastError();
        CloseHandle(process);
    } else error = GetLastError();
    diagnostics::runtime::Event("win_e_gate", {{"observed_left_win", flags & 1},
        {"observed_right_win", (flags >> 1) & 1}, {"async_win", (flags >> 2) & 1},
        {"control", (flags >> 3) & 1}, {"alt", (flags >> 4) & 1},
        {"shift", (flags >> 5) & 1}, {"injected", (flags >> 6) & 1},
        {"eligible", (flags >> 7) & 1}, {"foreground_pid", pid},
        {"foreground_elevated", elevated}, {"token_error", error}});
}
}

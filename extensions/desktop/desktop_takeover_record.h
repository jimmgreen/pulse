// desktop_takeover_record.h - Crash-safe ownership of Explorer's native icon view.
// The host records the exact SysListView32 before hiding it asynchronously. Recovery restores only
// the owned view; legacy records without a HWND still restore the earlier Show desktop icons switch.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <string>

namespace desktop_takeover {

inline constexpr wchar_t kDefaultRecordKey[] = L"Software\\Pulse\\DesktopTakeover";

inline constexpr UINT kCmdToggleDesktopIcons = 0x7402;

struct TakeoverState {
    bool active = false;
    bool hid_icons = false;  // the host turned "Show desktop icons" off and owes turning it back on
    uint64_t listview_hwnd = 0;
    uint32_t host_pid = 0;
    uint32_t crash_count = 0;
    uint64_t last_crash_tick_ms = 0;
    std::wstring snapshot_path;
};

namespace detail {

inline BOOL CALLBACK FindDefViewEnumProc(HWND top, LPARAM lparam) noexcept {
    auto* out_defview = reinterpret_cast<HWND*>(lparam);
    wchar_t cls[64]{};
    if (GetClassNameW(top, cls, ARRAYSIZE(cls)) <= 0) return TRUE;
    if (wcscmp(cls, L"Progman") != 0 && wcscmp(cls, L"WorkerW") != 0) return TRUE;
    HWND def_view = FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr);
    if (def_view) {
        *out_defview = def_view;
        return FALSE;
    }
    return TRUE;
}

inline bool ProcessAlive(DWORD pid) noexcept {
    if (pid == 0) return false;
    HANDLE proc = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return false;
    DWORD exit_code = 0;
    const bool alive = GetExitCodeProcess(proc, &exit_code) && exit_code == STILL_ACTIVE &&
                       WaitForSingleObject(proc, 0) == WAIT_TIMEOUT;
    CloseHandle(proc);
    return alive;
}

inline bool WriteDword(HKEY key, const wchar_t* name, DWORD value) noexcept {
    return RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value)) == ERROR_SUCCESS;
}

inline bool WriteQword(HKEY key, const wchar_t* name, uint64_t value) noexcept {
    return RegSetValueExW(key, name, 0, REG_QWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value)) == ERROR_SUCCESS;
}

inline DWORD ReadDword(HKEY root, const wchar_t* subkey, const wchar_t* name, DWORD fallback = 0) noexcept {
    DWORD value = fallback;
    DWORD bytes = sizeof(value);
    if (RegGetValueW(root, subkey, name, RRF_RT_REG_DWORD, nullptr, &value, &bytes) != ERROR_SUCCESS) {
        return fallback;
    }
    return value;
}

inline uint64_t ReadQword(HKEY root, const wchar_t* subkey, const wchar_t* name, uint64_t fallback = 0) noexcept {
    uint64_t value = fallback;
    DWORD bytes = sizeof(value);
    if (RegGetValueW(root, subkey, name, RRF_RT_REG_QWORD, nullptr, &value, &bytes) != ERROR_SUCCESS) {
        return fallback;
    }
    return value;
}

inline std::wstring ReadSz(HKEY root, const wchar_t* subkey, const wchar_t* name) {
    wchar_t buf[2048]{};
    DWORD bytes = sizeof(buf) - sizeof(wchar_t);
    if (RegGetValueW(root, subkey, name, RRF_RT_REG_SZ, nullptr, buf, &bytes) != ERROR_SUCCESS) {
        return {};
    }
    return buf;
}

} // namespace detail

// Locates Explorer's native desktop SysListView32 across Win10, Win11 21H2-23H2
// (WorkerW parent) and Win11 24H2+ (Progman parent with child WorkerW sibling).
inline HWND FindNativeDesktopDefView() noexcept {
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (progman) {
        if (HWND dv = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr)) {
            return dv;
        }
    }
    HWND def_view = nullptr;
    EnumWindows(&detail::FindDefViewEnumProc, reinterpret_cast<LPARAM>(&def_view));
    return def_view;
}

inline HWND FindNativeDesktopListView() noexcept {
    HWND def_view = FindNativeDesktopDefView();
    if (!def_view) return nullptr;
    return FindWindowExW(def_view, nullptr, L"SysListView32", nullptr);
}

inline TakeoverState ReadState(const wchar_t* reg_key = kDefaultRecordKey) {
    TakeoverState st;
    st.active = detail::ReadDword(HKEY_CURRENT_USER, reg_key, L"Active", 0) != 0;
    st.hid_icons = detail::ReadDword(HKEY_CURRENT_USER, reg_key, L"HidIcons", 0) != 0;
    st.listview_hwnd = detail::ReadQword(HKEY_CURRENT_USER, reg_key, L"ListViewHwnd", 0);
    st.host_pid = detail::ReadDword(HKEY_CURRENT_USER, reg_key, L"HostPid", 0);
    st.crash_count = detail::ReadDword(HKEY_CURRENT_USER, reg_key, L"CrashCount", 0);
    st.last_crash_tick_ms = detail::ReadQword(HKEY_CURRENT_USER, reg_key, L"LastCrashTickMs", 0);
    st.snapshot_path = detail::ReadSz(HKEY_CURRENT_USER, reg_key, L"SnapshotPath");
    return st;
}

inline bool RecordActive(DWORD host_pid, bool hid_icons, const std::wstring& snapshot_path,
                         const wchar_t* reg_key = kDefaultRecordKey) noexcept {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, reg_key, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const bool ok =
        detail::WriteDword(key, L"Active", 1u) &&
        detail::WriteDword(key, L"HidIcons", hid_icons ? 1u : 0u) &&
        detail::WriteQword(key, L"ListViewHwnd", 0u) &&
        detail::WriteDword(key, L"HostPid", host_pid) &&
        RegSetValueExW(key, L"SnapshotPath", 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(snapshot_path.c_str()),
                       static_cast<DWORD>((snapshot_path.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    RegCloseKey(key);
    return ok;
}

inline void RecordIconsRestored(const wchar_t* reg_key = kDefaultRecordKey) noexcept {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, reg_key, 0, KEY_WRITE, &key) == ERROR_SUCCESS) {
        detail::WriteDword(key, L"HidIcons", 0u);
        RegCloseKey(key);
    }
}

inline bool RecordCrashEvent(uint64_t now_tick_ms, uint64_t window_ms, uint32_t max_crashes,
                             bool& out_tripped_breaker, const wchar_t* reg_key = kDefaultRecordKey) noexcept {
    out_tripped_breaker = false;
    TakeoverState st = ReadState(reg_key);
    uint32_t count = 1u;
    if (st.last_crash_tick_ms != 0 && now_tick_ms >= st.last_crash_tick_ms &&
        (now_tick_ms - st.last_crash_tick_ms) <= window_ms) {
        count = st.crash_count + 1u;
    }
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, reg_key, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    detail::WriteDword(key, L"CrashCount", count);
    detail::WriteQword(key, L"LastCrashTickMs", now_tick_ms);
    if (count >= max_crashes) {
        out_tripped_breaker = true;
        detail::WriteDword(key, L"Active", 0u);
    }
    RegCloseKey(key);
    return true;
}

inline void ClearActive(const wchar_t* reg_key = kDefaultRecordKey, bool remove_key = false) noexcept {
    if (remove_key) {
        RegDeleteTreeW(HKEY_CURRENT_USER, reg_key);
        return;
    }
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, reg_key, 0, KEY_WRITE, &key) == ERROR_SUCCESS) {
        detail::WriteDword(key, L"Active", 0u);
        detail::WriteDword(key, L"HidIcons", 0u);
        detail::WriteDword(key, L"HostPid", 0u);
        detail::WriteDword(key, L"CrashCount", 0u);
        RegCloseKey(key);
    }
}

// Explorer's "Show desktop icons" state, as SHGetSetSettings reports it (HideIcons).
inline bool DesktopIconsHidden() noexcept {
    using GetSetFn = void(WINAPI*)(void*, DWORD, BOOL);
    HMODULE shell = GetModuleHandleW(L"shell32.dll");
    if (!shell) shell = LoadLibraryExW(L"shell32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto get_set = shell ? reinterpret_cast<GetSetFn>(GetProcAddress(shell, "SHGetSetSettings")) : nullptr;
    if (!get_set) return false;
    // SHELLSTATE begins with its flag bit-field; fHideIcons is bit 12. SSF_HIDEICONS = 0x4000.
    DWORD shell_state[16]{};
    get_set(shell_state, 0x00004000u, FALSE);
    return (shell_state[0] & (1u << 12)) != 0;
}

// Flips Explorer's "Show desktop icons" switch through the desktop view, exactly as its menu does.
inline bool ToggleDesktopIcons() noexcept {
    HWND def_view = FindNativeDesktopDefView();
    if (!def_view) return false;
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(def_view, WM_COMMAND, kCmdToggleDesktopIcons, 0, SMTO_ABORTIFHUNG, 3000, &result) != 0;
}

// Hide the actual view without querying Shell's per-process settings cache or changing user preferences.
inline bool HideNativeDesktopListView(HWND list_view, const wchar_t* reg_key = kDefaultRecordKey) noexcept {
    if (!list_view || !IsWindow(list_view)) return false;
    if ((GetWindowLongPtrW(list_view, GWL_STYLE) & WS_VISIBLE) == 0) return false;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, reg_key, 0, KEY_WRITE, &key) != ERROR_SUCCESS) return false;
    const bool recorded = detail::WriteDword(key, L"HidIcons", 1u) &&
        detail::WriteQword(key, L"ListViewHwnd", reinterpret_cast<uintptr_t>(list_view));
    RegCloseKey(key);
    if (!recorded) return false;
    return ShowWindowAsync(list_view, SW_HIDE) != FALSE;
}

inline bool RestoreOwnedListView(HWND list_view, const TakeoverState& state) noexcept {
    if (!state.hid_icons || !list_view || state.listview_hwnd != reinterpret_cast<uintptr_t>(list_view) ||
        !IsWindow(list_view) || (GetWindowLongPtrW(list_view, GWL_STYLE) & WS_VISIBLE) != 0) return false;
    return ShowWindowAsync(list_view, SW_SHOWNOACTIVATE) != FALSE;
}

// Turns the desktop icons back on when this record says the host turned them off.
inline bool RestoreDesktopIcons(const wchar_t* reg_key = kDefaultRecordKey) noexcept {
    const TakeoverState st = ReadState(reg_key);
    bool restored = false;
    if (st.hid_icons && st.listview_hwnd == 0 && DesktopIconsHidden()) restored = ToggleDesktopIcons();
    if (RestoreOwnedListView(FindNativeDesktopListView(), st)) restored = true;
    RecordIconsRestored(reg_key);
    return restored;
}

// Called by Pulse startup / extension disable / crash circuit-breaker:
// If Active == 1 and either extension_enabled is false OR HostPid is dead, turns Explorer's
// desktop icons back on (when the host turned them off) and marks Active = 0.
inline bool RecoverOrphanedTakeover(bool extension_enabled,
                                    const wchar_t* reg_key = kDefaultRecordKey,
                                    bool allow_live_desktop = true) noexcept {
    const TakeoverState st = ReadState(reg_key);
    if (!st.active) return false;
    if (extension_enabled && detail::ProcessAlive(st.host_pid)) return false;
    if (allow_live_desktop) RestoreDesktopIcons(reg_key);
    ClearActive(reg_key, false);
    return true;
}

} // namespace desktop_takeover

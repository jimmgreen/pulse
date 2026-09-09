#pragma once
#include <windows.h>
#include <shellscalingapi.h>

namespace pulse::compat {
// Also exercises the legacy path on development machines without changing the OS.
inline bool LegacyMode() noexcept {
    wchar_t value[4]{};
    return GetEnvironmentVariableW(L"PULSE_COMPAT_81", value, ARRAYSIZE(value)) == 1 && value[0] == L'1';
}

inline FARPROC UserApi(const char* name) noexcept {
    return GetProcAddress(GetModuleHandleW(L"user32.dll"), name);
}

inline FARPROC ScalingApi(const char* name) noexcept {
    static const HMODULE module = LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return module ? GetProcAddress(module, name) : nullptr;
}

inline bool ModernWindows() noexcept {
    return !LegacyMode() && UserApi("GetDpiForWindow") != nullptr;
}

inline UINT WindowDpi(HWND hwnd) noexcept {
    using Fn = UINT(WINAPI*)(HWND);
    static const auto fn = reinterpret_cast<Fn>(UserApi("GetDpiForWindow"));
    if (!LegacyMode() && fn) {
        const UINT dpi = fn(hwnd);
        if (dpi) return dpi;
    }
    UINT x = 96, y = 96;
    using MonitorFn = HRESULT(WINAPI*)(HMONITOR, MONITOR_DPI_TYPE, UINT*, UINT*);
    static const auto monitor_fn = reinterpret_cast<MonitorFn>(ScalingApi("GetDpiForMonitor"));
    if (monitor_fn && SUCCEEDED(monitor_fn(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST),
                                          MDT_EFFECTIVE_DPI, &x, &y)) && x) return x;
    HDC dc = GetDC(nullptr);
    const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return dpi > 0 ? static_cast<UINT>(dpi) : 96;
}

inline int SystemMetricsForDpi(int metric, UINT dpi) noexcept {
    using Fn = int(WINAPI*)(int, UINT);
    static const auto fn = reinterpret_cast<Fn>(UserApi("GetSystemMetricsForDpi"));
    if (!LegacyMode() && fn) return fn(metric, dpi);
    HDC dc = GetDC(nullptr);
    const int system_dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return MulDiv(GetSystemMetrics(metric), static_cast<int>(dpi), system_dpi > 0 ? system_dpi : 96);
}

inline void EnableDpiAwareness() noexcept {
    using Fn = BOOL(WINAPI*)(HANDLE);
    static const auto fn = reinterpret_cast<Fn>(UserApi("SetProcessDpiAwarenessContext"));
    if (!LegacyMode() && fn && fn(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)))) return;
    using AwarenessFn = HRESULT(WINAPI*)(PROCESS_DPI_AWARENESS);
    static const auto awareness = reinterpret_cast<AwarenessFn>(ScalingApi("SetProcessDpiAwareness"));
    const HRESULT result = awareness ? awareness(PROCESS_PER_MONITOR_DPI_AWARE) : E_NOTIMPL;
    if (FAILED(result) && result != E_ACCESSDENIED) SetProcessDPIAware();
}
}

#pragma comment(lib, "shcore.lib")

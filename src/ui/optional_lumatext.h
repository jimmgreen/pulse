#pragma once
#include "../common/windows_compat.h"
#include <delayimp.h>

namespace pulse::ui {
#if defined(PULSE_HAS_LUMATEXT)
inline bool ResolveLumaImports() noexcept {
    __try {
        return SUCCEEDED(__HrLoadAllImportsForDll("lumatext.dll"));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline bool LoadOptionalLumaText() noexcept {
    if (!compat::ModernWindows()) return false;
    static const bool available = [] {
        wchar_t path[32768]{};
        const DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
        if (!length || length >= ARRAYSIZE(path)) return false;
        wchar_t* slash = wcsrchr(path, L'\\');
        if (!slash) return false;
        if (wcscpy_s(slash + 1, ARRAYSIZE(path) - (slash + 1 - path), L"lumatext.dll")) return false;
        // Retain the module for process lifetime: delay-import slots reference it.
        DWORD previous_mode = 0;
        const bool changed_mode = SetThreadErrorMode(GetThreadErrorMode() |
            SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &previous_mode) != FALSE;
        const HMODULE module = LoadLibraryExW(path, nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        const bool resolved = module && ResolveLumaImports();
        if (changed_mode) SetThreadErrorMode(previous_mode, nullptr);
        return resolved;
    }();
    return available;
}
#else
inline bool LoadOptionalLumaText() noexcept { return false; }
#endif
}

#pragma once
#include "../common/windows_compat.h"
#include <delayimp.h>
#include "optional_lumatext_binding.h"

namespace pulse::ui {
#if defined(PULSE_HAS_LUMATEXT)
inline bool ResolveLumaImports(HMODULE importer, HMODULE expected) noexcept {
    __try {
        return detail::VerifyLumaDelayBinding(importer, expected, false) &&
            SUCCEEDED(__HrLoadAllImportsForDll("lumatext.dll")) &&
            detail::VerifyLumaDelayBinding(importer, expected, true);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline bool LoadOptionalLumaText() noexcept {
    if (!compat::ModernWindows()) return false;
    static const bool available = [] {
        wchar_t path[32768]{};
        HMODULE importer = nullptr;
        if (!detail::ModuleSiblingPath(reinterpret_cast<const void*>(&LoadOptionalLumaText), L"lumatext.dll",
            path, ARRAYSIZE(path), importer)) return false;
        // Hold the exact sibling file against replacement during load/binding.
        HANDLE expected_file = CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (expected_file == INVALID_HANDLE_VALUE) return false;
        // Retain the module for process lifetime: delay-import slots reference it.
        DWORD previous_mode = 0;
        const bool changed_mode = SetThreadErrorMode(GetThreadErrorMode() |
            SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &previous_mode) != FALSE;
        const HMODULE module = LoadLibraryExW(path, nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        const bool resolved = module && detail::ModuleMatchesFile(module, expected_file) && ResolveLumaImports(importer, module);
        CloseHandle(expected_file);
        if (changed_mode) SetThreadErrorMode(previous_mode, nullptr);
        return resolved;
    }();
    return available;
}
#else
inline bool LoadOptionalLumaText() noexcept { return false; }
#endif
}

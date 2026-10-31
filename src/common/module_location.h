#pragma once
#include <windows.h>
#include <cwchar>
namespace pulse::module_location {
// Resolve only the code's containing module. Never consult CWD/PATH or the
// calling process EXE when the code lives in a separately loaded UI DLL.
inline DWORD DirectoryFromAddress(const void* address, wchar_t* directory, DWORD capacity) noexcept {
    if (!directory || !capacity) return ERROR_INVALID_PARAMETER;
    directory[0] = 0;
    if (!address) return ERROR_INVALID_PARAMETER;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(address), &module)) return GetLastError();
    const DWORD length = GetModuleFileNameW(module, directory, capacity);
    if (!length) return GetLastError();
    if (length >= capacity) { directory[0] = 0; return ERROR_INSUFFICIENT_BUFFER; }
    wchar_t* slash = wcsrchr(directory, L'\\');
    if (!slash) { directory[0] = 0; return ERROR_BAD_PATHNAME; }
    slash[1] = 0;
    return ERROR_SUCCESS;
}
}

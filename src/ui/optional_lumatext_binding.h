#pragma once
#include <windows.h>
#include <delayimp.h>
#include <cstring>
#include <cstddef>
#include <cwchar>
namespace pulse::ui::detail {
inline bool ModuleSiblingPath(const void* address, const wchar_t* leaf, wchar_t* path, DWORD capacity, HMODULE& owner) noexcept {
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(address), &owner)) return false;
    const DWORD length = GetModuleFileNameW(owner, path, capacity);
    if (!length || length >= capacity) return false;
    wchar_t* slash = wcsrchr(path, L'\\');
    return slash && wcscpy_s(slash + 1, capacity - static_cast<size_t>(slash + 1 - path), leaf) == 0;
}
inline bool ModuleMatchesFile(HMODULE module, HANDLE expected_file) noexcept {
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(module, path, ARRAYSIZE(path));
    if (!module || !length || length >= ARRAYSIZE(path)) return false;
    HANDLE actual_file = CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (actual_file == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION actual{}, expected{};
    const bool same = GetFileInformationByHandle(actual_file, &actual) && GetFileInformationByHandle(expected_file, &expected) &&
        actual.dwVolumeSerialNumber == expected.dwVolumeSerialNumber && actual.nFileIndexHigh == expected.nFileIndexHigh &&
        actual.nFileIndexLow == expected.nFileIndexLow;
    CloseHandle(actual_file); return same;
}
template<class T> inline T* ImagePointer(BYTE* base, DWORD image_size, DWORD rva, size_t count = 1) noexcept {
    if (!rva || rva >= image_size || count > (image_size - rva) / sizeof(T)) return nullptr;
    return reinterpret_cast<T*>(base + rva);
}
// The delay helper resolves by basename. Check its actual cached module and,
// after resolution, every imported address against the exact absolute module.
// PE delay import layout: https://learn.microsoft.com/windows/win32/debug/pe-format
inline bool VerifyLumaDelayBinding(HMODULE importer, HMODULE expected, bool resolved) noexcept {
    __try {
        if (!importer || !expected) return false;
        auto* base = reinterpret_cast<BYTE*>(importer);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 1024 * 1024) return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC ||
            nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT) return false;
        const DWORD size = nt->OptionalHeader.SizeOfImage;
        const auto directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
        const auto* descriptors = ImagePointer<ImgDelayDescr>(base, size, directory.VirtualAddress,
            directory.Size / sizeof(ImgDelayDescr));
        if (!descriptors) return false;
        for (size_t i = 0; i < directory.Size / sizeof(ImgDelayDescr); ++i) {
            const auto& descriptor = descriptors[i];
            if (!descriptor.rvaDLLName) break;
            if (descriptor.grAttrs != dlattrRva) return false;
            const char* name = ImagePointer<char>(base, size, descriptor.rvaDLLName);
            if (!name || !memchr(name, 0, size - descriptor.rvaDLLName)) return false;
            if (std::strcmp(name, "lumatext.dll") != 0) continue;
            HMODULE* bound = ImagePointer<HMODULE>(base, size, descriptor.rvaHmod);
            if (!bound || (*bound && *bound != expected) || (resolved && *bound != expected)) return false;
            if (!resolved) return true;
            for (size_t slot = 0; slot != 4096; ++slot) {
                auto* names = ImagePointer<IMAGE_THUNK_DATA>(base, size, descriptor.rvaINT, slot + 1);
                auto* addresses = ImagePointer<IMAGE_THUNK_DATA>(base, size, descriptor.rvaIAT, slot + 1);
                if (!names || !addresses) return false;
                const auto lookup = names[slot].u1.Ordinal;
                if (!lookup) return slot != 0;
                LPCSTR symbol = nullptr;
                if (IMAGE_SNAP_BY_ORDINAL(lookup)) symbol = reinterpret_cast<LPCSTR>(static_cast<ULONG_PTR>(IMAGE_ORDINAL(lookup)));
                else {
#ifdef _WIN64
                    if (lookup > MAXDWORD) return false;
#endif
                    const auto rva = static_cast<DWORD>(lookup);
                    const auto* import_entry = ImagePointer<IMAGE_IMPORT_BY_NAME>(base, size, rva);
                    if (!import_entry || !memchr(import_entry->Name, 0, size - rva - offsetof(IMAGE_IMPORT_BY_NAME, Name))) return false;
                    symbol = reinterpret_cast<LPCSTR>(import_entry->Name);
                }
                const auto procedure = GetProcAddress(expected, symbol);
                if (!procedure || addresses[slot].u1.Function != reinterpret_cast<ULONG_PTR>(procedure)) return false;
            }
            return false;
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}

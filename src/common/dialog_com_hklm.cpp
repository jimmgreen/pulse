#include "dialog_com_hklm.h"

#include <sddl.h>
#include <vector>

namespace pulse::dialog_com_hklm {
namespace {

struct RegKeyGuard {
    HKEY h = nullptr;
    ~RegKeyGuard() { if (h) RegCloseKey(h); }
};

REGSAM WowFlag(bool wow64_x86, const RegistryScope& scope) noexcept {
    if (!scope.use_wow64_views) return 0;
    return wow64_x86 ? KEY_WOW64_32KEY : KEY_WOW64_64KEY;
}

std::wstring ClsidInprocSubkey(const RegistryScope& scope, const wchar_t* clsid) {
    return scope.classes_prefix + L"\\" + clsid + L"\\InprocServer32";
}

std::wstring BackupSubkey(const RegistryScope& scope, bool wow64_x86, const wchar_t* kind) {
    return scope.backup_prefix + (wow64_x86 ? L"\\32\\" : L"\\64\\") + kind;
}

bool SamePath(const std::wstring& a, const std::wstring& b) noexcept {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

DWORD ReadRegString(HKEY key, const wchar_t* name, std::wstring& out, DWORD& type) noexcept {
    out.clear();
    type = 0;
    DWORD bytes = 0;
    DWORD err = RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes);
    if (err != ERROR_SUCCESS) return err;
    if (type != REG_SZ && type != REG_EXPAND_SZ) return ERROR_INVALID_DATATYPE;
    if (bytes == 0) return ERROR_SUCCESS;
    std::wstring buf(bytes / sizeof(wchar_t), L'\0');
    err = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(buf.data()), &bytes);
    if (err != ERROR_SUCCESS) return err;
    while (!buf.empty() && buf.back() == L'\0') buf.pop_back();
    out = std::move(buf);
    return ERROR_SUCCESS;
}

DWORD WriteRegString(HKEY key, const wchar_t* name, DWORD type, const std::wstring& value) noexcept {
    const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    return RegSetValueExW(key, name, 0, type, reinterpret_cast<const BYTE*>(value.c_str()), bytes);
}

DWORD ReadKeySddl(HKEY key, std::wstring& sddl_out) noexcept {
    sddl_out.clear();
    DWORD needed = 0;
    constexpr SECURITY_INFORMATION info = OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
    LONG err = RegGetKeySecurity(key, info, nullptr, &needed);
    if (err != ERROR_INSUFFICIENT_BUFFER || needed == 0) return static_cast<DWORD>(err);
    std::vector<BYTE> buf(needed);
    err = RegGetKeySecurity(key, info, buf.data(), &needed);
    if (err != ERROR_SUCCESS) return static_cast<DWORD>(err);
    LPWSTR sddl = nullptr;
    if (!ConvertSecurityDescriptorToStringSecurityDescriptorW(buf.data(), SDDL_REVISION_1, info, &sddl, nullptr)) {
        return GetLastError();
    }
    sddl_out = sddl;
    LocalFree(sddl);
    return ERROR_SUCCESS;
}

bool LooksLikePulseDll(const std::wstring& path) noexcept {
    auto contains_ci = [&](const wchar_t* needle) {
        const size_t nlen = wcslen(needle);
        if (path.size() < nlen) return false;
        for (size_t i = 0; i + nlen <= path.size(); ++i) {
            if (CompareStringOrdinal(path.data() + i, static_cast<int>(nlen), needle, static_cast<int>(nlen), TRUE) == CSTR_EQUAL) {
                return true;
            }
        }
        return false;
    };
    return contains_ci(L"PulseDialogStub") || contains_ci(L"PulseDialogAuto") || contains_ci(L"PulseDialogCore");
}

DWORD BackupSingle(const RegistryScope& scope, bool wow64_x86, const wchar_t* clsid, const wchar_t* kind) noexcept {
    const REGSAM wow = WowFlag(wow64_x86, scope);
    const auto backup_path = BackupSubkey(scope, wow64_x86, kind);

    // Idempotent: never overwrite an already captured original system backup!
    {
        RegKeyGuard existing;
        if (RegOpenKeyExW(scope.root, backup_path.c_str(), 0, KEY_READ | wow, &existing.h) == ERROR_SUCCESS) {
            std::wstring prev;
            DWORD prev_type = 0;
            if (ReadRegString(existing.h, L"OriginalPath", prev, prev_type) == ERROR_SUCCESS && !prev.empty()) {
                return ERROR_SUCCESS;
            }
        }
    }

    RegKeyGuard src;
    const auto inproc_path = ClsidInprocSubkey(scope, clsid);
    DWORD err = RegOpenKeyExW(scope.root, inproc_path.c_str(), 0, KEY_READ | READ_CONTROL | wow, &src.h);
    if (err != ERROR_SUCCESS) return err;

    std::wstring orig_path;
    DWORD orig_type = REG_EXPAND_SZ;
    err = ReadRegString(src.h, nullptr, orig_path, orig_type);
    if (err != ERROR_SUCCESS || orig_path.empty()) return err ? err : ERROR_INVALID_DATA;

    // Never record Pulse's own binary as the original OS backup.
    if (LooksLikePulseDll(orig_path)) return ERROR_SUCCESS;

    std::wstring threading;
    DWORD thread_type = REG_SZ;
    if (ReadRegString(src.h, L"ThreadingModel", threading, thread_type) != ERROR_SUCCESS) {
        threading = L"Apartment";
    }
    std::wstring sddl;
    ReadKeySddl(src.h, sddl);

    RegKeyGuard dst;
    err = RegCreateKeyExW(scope.root, backup_path.c_str(), 0, nullptr, 0, KEY_WRITE | wow, nullptr, &dst.h, nullptr);
    if (err != ERROR_SUCCESS) return err;

    err = WriteRegString(dst.h, L"OriginalPath", REG_SZ, orig_path);
    if (err != ERROR_SUCCESS) return err;
    err = RegSetValueExW(dst.h, L"OriginalType", 0, REG_DWORD,
                         reinterpret_cast<const BYTE*>(&orig_type), sizeof(orig_type));
    if (err != ERROR_SUCCESS) return err;
    err = WriteRegString(dst.h, L"ThreadingModel", REG_SZ, threading);
    if (err != ERROR_SUCCESS) return err;
    if (!sddl.empty()) {
        WriteRegString(dst.h, L"OriginalSddl", REG_SZ, sddl);
    }
    return ERROR_SUCCESS;
}

DWORD WriteSingle(const RegistryScope& scope, bool wow64_x86, const wchar_t* clsid, const std::wstring& stub_dll_path) noexcept {
    const REGSAM wow = WowFlag(wow64_x86, scope);
    const auto inproc_path = ClsidInprocSubkey(scope, clsid);
    RegKeyGuard key;
    DWORD err = RegOpenKeyExW(scope.root, inproc_path.c_str(), 0, KEY_SET_VALUE | wow, &key.h);
    if (err != ERROR_SUCCESS) return err;
    err = WriteRegString(key.h, nullptr, REG_SZ, stub_dll_path);
    if (err != ERROR_SUCCESS) return err;
    return WriteRegString(key.h, L"ThreadingModel", REG_SZ, L"Apartment");
}

DWORD RestoreSingle(const RegistryScope& scope, bool wow64_x86, const wchar_t* clsid, const wchar_t* kind) noexcept {
    const REGSAM wow = WowFlag(wow64_x86, scope);
    const auto backup_path = BackupSubkey(scope, wow64_x86, kind);

    RegKeyGuard bak;
    DWORD err = RegOpenKeyExW(scope.root, backup_path.c_str(), 0, KEY_READ | wow, &bak.h);
    if (err == ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
    if (err != ERROR_SUCCESS) return err;

    std::wstring orig_path;
    DWORD ignored_type = 0;
    err = ReadRegString(bak.h, L"OriginalPath", orig_path, ignored_type);
    if (err != ERROR_SUCCESS || orig_path.empty()) return err ? err : ERROR_INVALID_DATA;

    DWORD orig_type = REG_EXPAND_SZ;
    DWORD dword_size = sizeof(orig_type);
    DWORD reg_type = 0;
    if (RegQueryValueExW(bak.h, L"OriginalType", nullptr, &reg_type,
                         reinterpret_cast<BYTE*>(&orig_type), &dword_size) != ERROR_SUCCESS ||
        reg_type != REG_DWORD || (orig_type != REG_SZ && orig_type != REG_EXPAND_SZ)) {
        orig_type = REG_EXPAND_SZ;
    }

    std::wstring threading;
    if (ReadRegString(bak.h, L"ThreadingModel", threading, ignored_type) != ERROR_SUCCESS || threading.empty()) {
        threading = L"Apartment";
    }

    const auto inproc_path = ClsidInprocSubkey(scope, clsid);
    RegKeyGuard dst;
    err = RegOpenKeyExW(scope.root, inproc_path.c_str(), 0, KEY_SET_VALUE | wow, &dst.h);
    if (err != ERROR_SUCCESS) return err;

    err = WriteRegString(dst.h, nullptr, orig_type, orig_path);
    if (err != ERROR_SUCCESS) return err;
    err = WriteRegString(dst.h, L"ThreadingModel", REG_SZ, threading);
    if (err != ERROR_SUCCESS) return err;

    RegCloseKey(bak.h);
    bak.h = nullptr;
    RegDeleteKeyExW(scope.root, backup_path.c_str(), wow, 0);
    return ERROR_SUCCESS;
}

} // namespace

DWORD RegisterStub(const std::wstring& stub_dll_path, bool wow64_x86, const RegistryScope& scope) noexcept {
    try {
        if (stub_dll_path.size() < 4 || stub_dll_path[1] != L':' || stub_dll_path[2] != L'\\') {
            return ERROR_INVALID_NAME;
        }
        // Step 1: backup BOTH Open & Save before mutating either!
        DWORD err = BackupSingle(scope, wow64_x86, kClsidOpen, L"Open");
        if (err != ERROR_SUCCESS) return err;
        err = BackupSingle(scope, wow64_x86, kClsidSave, L"Save");
        if (err != ERROR_SUCCESS) return err;

        // Step 2: write Open, then Save; roll back Open if Save fails.
        err = WriteSingle(scope, wow64_x86, kClsidOpen, stub_dll_path);
        if (err != ERROR_SUCCESS) return err;
        err = WriteSingle(scope, wow64_x86, kClsidSave, stub_dll_path);
        if (err != ERROR_SUCCESS) {
            RestoreSingle(scope, wow64_x86, kClsidOpen, L"Open");
            return err;
        }
        return ERROR_SUCCESS;
    } catch (...) {
        return ERROR_GEN_FAILURE;
    }
}

DWORD RestoreBackup(bool wow64_x86, const RegistryScope& scope) noexcept {
    try {
        const DWORD e1 = RestoreSingle(scope, wow64_x86, kClsidOpen, L"Open");
        const DWORD e2 = RestoreSingle(scope, wow64_x86, kClsidSave, L"Save");
        return e1 != ERROR_SUCCESS ? e1 : e2;
    } catch (...) {
        return ERROR_GEN_FAILURE;
    }
}

ArchInspection Inspect(const std::wstring& expected_stub_path, bool wow64_x86, const RegistryScope& scope) noexcept {
    ArchInspection out{};
    try {
        const REGSAM wow = WowFlag(wow64_x86, scope);
        auto read_clsid = [&](const wchar_t* clsid, std::wstring& val) -> DWORD {
            RegKeyGuard k;
            DWORD err = RegOpenKeyExW(scope.root, ClsidInprocSubkey(scope, clsid).c_str(), 0, KEY_READ | wow, &k.h);
            if (err != ERROR_SUCCESS) return err;
            DWORD type = 0;
            return ReadRegString(k.h, nullptr, val, type);
        };
        const DWORD e1 = read_clsid(kClsidOpen, out.open_path);
        const DWORD e2 = read_clsid(kClsidSave, out.save_path);
        out.error = e1 != ERROR_SUCCESS ? e1 : e2;
        if (!expected_stub_path.empty()) {
            out.open_matches = (e1 == ERROR_SUCCESS) && SamePath(out.open_path, expected_stub_path);
            out.save_matches = (e2 == ERROR_SUCCESS) && SamePath(out.save_path, expected_stub_path);
        }
        RegKeyGuard bak_open, bak_save;
        out.backup_present =
            RegOpenKeyExW(scope.root, BackupSubkey(scope, wow64_x86, L"Open").c_str(), 0, KEY_READ | wow, &bak_open.h) == ERROR_SUCCESS &&
            RegOpenKeyExW(scope.root, BackupSubkey(scope, wow64_x86, L"Save").c_str(), 0, KEY_READ | wow, &bak_save.h) == ERROR_SUCCESS;
        return out;
    } catch (...) {
        out.error = ERROR_GEN_FAILURE;
        return out;
    }
}

} // namespace pulse::dialog_com_hklm

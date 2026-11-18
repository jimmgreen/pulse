#include "dialog_auto_registration.h"
#include "runtime_log.h"
#include <vector>
namespace pulse::dialog_auto {
namespace {
constexpr auto config = ConfigKey;
constexpr auto owner = L"PulseDialogAutoOwner";
constexpr auto token = L"Pulse.DialogAuto.v1";
constexpr const wchar_t* roots[] = {
    L"Software\\Classes\\CLSID\\{DC1C5A9C-E88A-4DDE-A5A1-60F82A20AEF7}",
    L"Software\\Classes\\CLSID\\{C0B4E2F3-BA21-4773-8DBA-335EC946EB8B}"};
// Win32 mutexes are recursive: Enable may invoke Disable during journal recovery.
struct MutationLock {
    HANDLE handle = CreateMutexW(nullptr, FALSE, L"Global\\Pulse.DialogAuto.Registration.v1");
    DWORD error = ERROR_SUCCESS;
    bool acquired = false;
    MutationLock() {
        if (!handle) { error = GetLastError(); return; }
        DWORD wait = WaitForSingleObject(handle, 30000);
        acquired = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
        if (!acquired) error = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
    }
    ~MutationLock() { if (acquired) ReleaseMutex(handle); if (handle) CloseHandle(handle); }
};
struct Key { HKEY h = nullptr; ~Key() { if (h) RegCloseKey(h); } };
DWORD Open(const wchar_t* p, Key& k, bool create = false, DWORD* disposition = nullptr, bool writable = false) {
    return create ? RegCreateKeyExW(HKEY_CURRENT_USER, p, 0, nullptr, 0, KEY_READ | KEY_WRITE | RegistryView, nullptr, &k.h, disposition)
                  : RegOpenKeyExW(HKEY_CURRENT_USER, p, 0, KEY_READ | (writable ? KEY_WRITE : 0) | RegistryView, &k.h);
}
std::wstring Read(HKEY k, const wchar_t* n, DWORD* error = nullptr) {
    DWORD size = 0, type = 0;
    DWORD e = RegQueryValueExW(k, n, nullptr, &type, nullptr, &size);
    auto fail = [&](DWORD code) { if (error) *error = code; return std::wstring{}; };
    if (e) return fail(e);
    if (type != REG_SZ || size < sizeof(wchar_t) || size > 65536 || size % sizeof(wchar_t)) return fail(ERROR_INVALID_DATA);
    std::vector<wchar_t> value(size / sizeof(wchar_t));
    const DWORD expected = size;
    e = RegQueryValueExW(k, n, nullptr, &type, reinterpret_cast<BYTE*>(value.data()), &size);
    if (e) return fail(e);
    if (type != REG_SZ || size != expected || value.back() != L'\0') return fail(ERROR_INVALID_DATA);
    for (size_t i = 0; i + 1 < value.size(); ++i) if (!value[i]) return fail(ERROR_INVALID_DATA);
    if (error) *error = ERROR_SUCCESS;
    return std::wstring(value.data(), value.size() - 1);
}
DWORD Write(HKEY k, const wchar_t* n, const std::wstring& v) {
    return RegSetValueExW(k, n, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()), static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
}
DWORD Flag(HKEY k, const wchar_t* n, DWORD v) { return RegSetValueExW(k, n, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v)); }
DWORD Number(HKEY k, const wchar_t* n) { DWORD v = 0, size = sizeof(v); RegGetValueW(k, nullptr, n, RRF_RT_REG_DWORD, nullptr, &v, &size); return v; }
bool Empty(HKEY k) { DWORD sub = 0, values = 0; return !RegQueryInfoKeyW(k, nullptr, nullptr, nullptr, &sub, nullptr, nullptr, &values, nullptr, nullptr, nullptr, nullptr) && !sub && !values; }
DWORD Validate(const std::wstring& p) {
    if (p.size() < 4 || p[1] != L':' || p[2] != L'\\' || _wcsicmp(p.substr(p.find_last_of(L'\\') + 1).c_str(), DllName)) return ERROR_INVALID_NAME;
    if (p.find_first_of(L"/\0", 0, 2) != std::wstring::npos || p.find(L':', 2) != std::wstring::npos) return ERROR_INVALID_NAME;
    size_t start = 3;
    for (size_t i = 3; i <= p.size(); ++i) {
        if (i != p.size() && p[i] != L'\\') continue;
        auto component = p.substr(start, i - start);
        if (component.empty() || component == L"." || component == L".." || component.back() == L'.' || component.back() == L' ') return ERROR_INVALID_NAME;
        start = i + 1;
    }
    for (size_t i = 3; i <= p.size(); ++i) {
        if (i != p.size() && p[i] != L'\\') continue;
        DWORD a = GetFileAttributesW(p.substr(0, i).c_str());
        if (a == INVALID_FILE_ATTRIBUTES) return GetLastError();
        if (a & FILE_ATTRIBUTE_REPARSE_POINT) return ERROR_ACCESS_DENIED;
    }
    wchar_t drive[] = { p[0], L':', L'\\', 0 };
    if (GetDriveTypeW(drive) != DRIVE_FIXED) return ERROR_BAD_PATHNAME;
    HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return GetLastError();
    IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS nt{}; DWORD got = 0;
    bool ok = ReadFile(f, &dos, sizeof(dos), &got, nullptr) && got == sizeof(dos) && dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0;
    if (ok) { LARGE_INTEGER pos{}; pos.QuadPart = dos.e_lfanew; ok = SetFilePointerEx(f, pos, nullptr, FILE_BEGIN) && ReadFile(f, &nt, sizeof(nt), &got, nullptr) && got == sizeof(nt) && nt.Signature == IMAGE_NT_SIGNATURE && nt.FileHeader.Machine == (sizeof(void*) == 8 ? IMAGE_FILE_MACHINE_AMD64 : IMAGE_FILE_MACHINE_I386) && (nt.FileHeader.Characteristics & IMAGE_FILE_DLL) && nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR_MAGIC; }
    CloseHandle(f); return ok ? ERROR_SUCCESS : ERROR_BAD_EXE_FORMAT;
}
bool Shape(HKEY key, DWORD expected_subkeys, DWORD expected_values) {
    DWORD subkeys = 0, values = 0;
    return RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS && subkeys == expected_subkeys && values == expected_values;
}
DWORD Cleanup(const std::wstring& path) {
    DWORD result = ERROR_SUCCESS;
    for (auto root : roots) {
        Key k; DWORD e = Open(root, k, false, nullptr, true);
        if (e == ERROR_FILE_NOT_FOUND) continue;
        if (e) { result = e; continue; }
        if (Read(k.h, owner) != token) { result = ERROR_ALREADY_EXISTS; continue; }
        std::wstring child = std::wstring(root) + L"\\InprocServer32";
        Key c; e = Open(child.c_str(), c, false, nullptr, true);
        if (e != ERROR_SUCCESS && e != ERROR_FILE_NOT_FOUND) { result = e; continue; }
        DWORD dll_error = ERROR_FILE_NOT_FOUND, model_error = ERROR_FILE_NOT_FOUND;
        std::wstring dll, model;
        if (c.h) { dll = Read(c.h, nullptr, &dll_error); model = Read(c.h, L"ThreadingModel", &model_error); }
        bool dll_valid = dll_error == ERROR_FILE_NOT_FOUND || (dll_error == ERROR_SUCCESS && !path.empty() && dll == path);
        bool model_valid = model_error == ERROR_FILE_NOT_FOUND || (model_error == ERROR_SUCCESS && model == L"Apartment");
        DWORD expected_values = (dll_error == ERROR_SUCCESS ? 1u : 0u) + (model_error == ERROR_SUCCESS ? 1u : 0u);
        // Validate the entire owned subtree before deleting any value. Unknown additions
        // and malformed values must leave even our matching values untouched.
        if (!dll_valid || !model_valid || !Shape(k.h, c.h ? 1u : 0u, 1) || (c.h && !Shape(c.h, 0, expected_values))) {
            result = ERROR_ALREADY_EXISTS; continue;
        }
        if (c.h) {
            if (dll_error == ERROR_SUCCESS && (e = RegDeleteValueW(c.h, nullptr))) { result = e; continue; }
            if (model_error == ERROR_SUCCESS && (e = RegDeleteValueW(c.h, L"ThreadingModel"))) { result = e; continue; }
            if (!Empty(c.h)) { result = ERROR_ALREADY_EXISTS; continue; }
            RegCloseKey(c.h); c.h = nullptr;
            e = RegDeleteKeyExW(HKEY_CURRENT_USER, child.c_str(), RegistryView, 0);
            if (e) { result = e; continue; }
        }
        if ((e = RegDeleteValueW(k.h, owner))) { result = e; continue; }
        if (!Empty(k.h)) { Write(k.h, owner, token); result = ERROR_ALREADY_EXISTS; continue; }
        e = RegDeleteKeyExW(HKEY_CURRENT_USER, root, RegistryView, 0); if (e) result = e;
    }
    return result;
}
}
Status Inspect() {
    Status s; Key c; DWORD e = Open(config, c); if (e && e != ERROR_FILE_NOT_FOUND) { s.error = e; return s; }
    s.dll_path = c.h ? Read(c.h, L"DllPath") : std::wstring{}; bool complete = !s.dll_path.empty();
    for (auto root : roots) { Key k, v; e = Open(root, k); if (e == ERROR_FILE_NOT_FOUND) { complete = false; continue; } if (e) { s.error = e; complete = false; continue; }
        if (Read(k.h, owner) != token) { s.conflict = true; complete = false; continue; }
        std::wstring sub = std::wstring(root) + L"\\InprocServer32";
        if (Open(sub.c_str(), v) || Read(v.h, nullptr) != s.dll_path || Read(v.h, L"ThreadingModel") != L"Apartment") { complete = false; s.conflict = true; }
    }
    s.enabled = complete && c.h && Number(c.h, L"Enabled") == 1; return s;
}
DWORD Disable() {
    MutationLock lock; if (lock.error) return lock.error;
    Key c; DWORD e = Open(config, c, false, nullptr, true); if (e == ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS; if (e) return e;
    if ((e = Flag(c.h, L"Enabled", 0))) return e;
    RegFlushKey(c.h); e = Cleanup(Read(c.h, L"DllPath"));
    if (!e) Flag(c.h, L"Stage", 0);
    diagnostics::runtime::Event("dialog_auto_disable", {{"error", e}}); return e;
}
DWORD DisableFor(const std::wstring& absolute_dll) {
    MutationLock lock; if (lock.error) return lock.error;
    auto normalize=[](std::wstring path) {
        if(path.starts_with(LR"(\\?\)")) path.erase(0,4);
        if(path.size()<3 || path[1]!=L':' || path[2]!=L'\\') return std::wstring{};
        std::wstring full(32768,L'\0');
        const DWORD length=GetFullPathNameW(path.c_str(),static_cast<DWORD>(full.size()),full.data(),nullptr);
        if(!length || length>=full.size()) return std::wstring{};
        full.resize(length);return full;
    };
    const auto expected=normalize(absolute_dll);
    if(expected.empty()) return ERROR_INVALID_NAME;
    Key key; DWORD error=Open(config,key);
    if(error && error!=ERROR_FILE_NOT_FOUND) return error;
    const auto recorded=key.h ? normalize(Read(key.h,L"DllPath",&error)) : std::wstring{};
    if(error && error!=ERROR_FILE_NOT_FOUND) return error;
    if(!recorded.empty() && CompareStringOrdinal(expected.c_str(),-1,recorded.c_str(),-1,TRUE)==CSTR_EQUAL) return Disable();
    // A missing/stale journal must not permit removal of a still-referenced DLL.
    for(auto root:roots) {
        Key child;const auto sub=std::wstring(root)+L"\\InprocServer32";
        const DWORD opened=Open(sub.c_str(),child);
        if(opened==ERROR_FILE_NOT_FOUND) continue;
        if(opened) return opened;
        const auto referenced=normalize(Read(child.h,nullptr));
        if(!referenced.empty() && CompareStringOrdinal(expected.c_str(),-1,referenced.c_str(),-1,TRUE)==CSTR_EQUAL) return ERROR_ALREADY_EXISTS;
    }
    return ERROR_SUCCESS;
}
DWORD Enable(const std::wstring& p) {
    MutationLock lock; if (lock.error) return lock.error;
    DWORD e = Validate(p); if (e) return e;
    Status s = Inspect(); if (s.enabled && s.dll_path == p) return ERROR_SUCCESS;
    Key c; if ((e = Open(config, c, true))) return e;
    if (Number(c.h, L"Stage")) { if ((e = Disable())) return e; }
    for (auto root : roots) { Key k; e = Open(root, k); if (!e) return ERROR_ALREADY_EXISTS; if (e != ERROR_FILE_NOT_FOUND) return e; }
    if ((e = Flag(c.h, L"Enabled", 0)) || (e = Write(c.h, L"DllPath", p)) || (e = Flag(c.h, L"Stage", 1)) || (e = RegFlushKey(c.h))) return e;
    for (auto root : roots) {
        Key k, v; DWORD disposition = 0; e = Open(root, k, true, &disposition);
        if (!e && disposition != REG_CREATED_NEW_KEY) e = ERROR_ALREADY_EXISTS;
        if (!e) e = Write(k.h, owner, token);
        std::wstring sub = std::wstring(root) + L"\\InprocServer32";
        if (!e) e = Open(sub.c_str(), v, true);
        if (!e) e = Write(v.h, nullptr, p);
        if (!e) e = Write(v.h, L"ThreadingModel", L"Apartment");
        if (e) { Cleanup(p); return e; }
    }
    e = Flag(c.h, L"Enabled", 1); if (e) Cleanup(p);
    diagnostics::runtime::Event("dialog_auto_enable", {{"error", e}}); return e;
}
bool IsEnabledFor(const std::wstring& p) noexcept { try { auto s = Inspect(); return s.enabled && !s.conflict && !s.error && s.dll_path == p; } catch (...) { return false; } }
bool PreferOutOfProcess() noexcept {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, config, 0, KEY_QUERY_VALUE | RegistryView, &key) != ERROR_SUCCESS) return true;
    DWORD type = 0, value = 1, size = sizeof(value);
    const LSTATUS read = RegQueryValueExW(key, L"Host", nullptr, &type, reinterpret_cast<BYTE*>(&value), &size);
    RegCloseKey(key);
    return read != ERROR_SUCCESS || type != REG_DWORD || size != sizeof(value) || value != 0;
}
// Remote host is the default; a REG_DWORD RemoteHost=0 opts back into the
// in-process UI. A failed remote start still falls back to in-process.
bool PreferRemoteHost() noexcept {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, config, 0, KEY_QUERY_VALUE | RegistryView, &key) != ERROR_SUCCESS) return true;
    DWORD type = 0, value = 0, size = sizeof(value);
    const LSTATUS read = RegQueryValueExW(key, L"RemoteHost", nullptr, &type, reinterpret_cast<BYTE*>(&value), &size);
    RegCloseKey(key);
    if (read != ERROR_SUCCESS || type != REG_DWORD || size != sizeof(value)) return true;
    return value != 0;
}
}




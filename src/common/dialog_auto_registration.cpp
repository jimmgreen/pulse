#include "dialog_auto_registration.h"
#include "dialog_auto_ownership.h"
#include "dialog_registration_mutex.h"
#include "runtime_log.h"
#include <vector>
#include <new>
namespace pulse::dialog_auto {
namespace {
bool ValidArchitecture(Architecture architecture) { return architecture == Architecture::X64 || architecture == Architecture::X86; }
DWORD ArchitectureError(Architecture architecture) {
    if (!ValidArchitecture(architecture)) return ERROR_INVALID_PARAMETER;
    SYSTEM_INFO system{}; GetNativeSystemInfo(&system);
    if (system.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64) return ERROR_SUCCESS;
    if (system.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL && architecture == Architecture::X86) return ERROR_SUCCESS;
    return ERROR_NOT_SUPPORTED;
}
const wchar_t* ConfigFor(Architecture architecture) { return architecture == Architecture::X64 ? L"Software\\Pulse\\DialogAuto" : L"Software\\Pulse\\DialogAuto32"; }
REGSAM ViewFor(Architecture architecture) { return architecture == Architecture::X64 ? KEY_WOW64_64KEY : KEY_WOW64_32KEY; }
constexpr auto owner = L"PulseDialogAutoOwner";
constexpr auto token = L"Pulse.DialogAuto.v1";
constexpr const wchar_t* roots[] = {
    L"Software\\Classes\\CLSID\\{DC1C5A9C-E88A-4DDE-A5A1-60F82A20AEF7}",
    L"Software\\Classes\\CLSID\\{C0B4E2F3-BA21-4773-8DBA-335EC946EB8B}"};
// Win32 mutexes are recursive: Enable may invoke Disable during journal recovery.
struct MutationLock {
    HANDLE handle = CreateRegistrationMutex();
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
DWORD Open(Architecture architecture, const wchar_t* p, Key& k, bool create = false, DWORD* disposition = nullptr, bool writable = false) {
    if (const DWORD error = ArchitectureError(architecture)) return error;
    return create ? RegCreateKeyExW(HKEY_CURRENT_USER, p, 0, nullptr, 0, KEY_READ | KEY_WRITE | ViewFor(architecture), nullptr, &k.h, disposition)
                  : RegOpenKeyExW(HKEY_CURRENT_USER, p, 0, KEY_READ | (writable ? KEY_WRITE : 0) | ViewFor(architecture), &k.h);
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
DWORD RequireZeroFlag(HKEY key, const wchar_t* name) {
    DWORD type = 0, size = 0;
    DWORD error = RegQueryValueExW(key, name, nullptr, &type, nullptr, &size);
    if (error) return error;
    if (type != REG_DWORD || size != sizeof(DWORD)) return ERROR_INVALID_DATA;
    DWORD value = 0;
    error = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size);
    if (error) return error;
    if (type != REG_DWORD || size != sizeof(DWORD)) return ERROR_INVALID_DATA;
    return value == 0 ? ERROR_SUCCESS : ERROR_ALREADY_EXISTS;
}
bool Empty(HKEY k) { DWORD sub = 0, values = 0; return !RegQueryInfoKeyW(k, nullptr, nullptr, nullptr, &sub, nullptr, nullptr, &values, nullptr, nullptr, nullptr, nullptr) && !sub && !values; }
DWORD ValidatePathSyntax(Architecture architecture, const std::wstring& p) {
    if (const DWORD error = ArchitectureError(architecture)) return error;
    if (p.size() < 4 || p[1] != L':' || p[2] != L'\\' ||
        !((p[0] >= L'A' && p[0] <= L'Z') || (p[0] >= L'a' && p[0] <= L'z')) ||
        !IsAllowedDialogDllName(architecture, p.c_str() + p.find_last_of(L'\\') + 1)) {
        return ERROR_INVALID_NAME;
    }
    if (p.find_first_of(L"/\0", 0, 2) != std::wstring::npos || p.find(L':', 2) != std::wstring::npos) return ERROR_INVALID_NAME;
    size_t start = 3;
    for (size_t i = 3; i <= p.size(); ++i) {
        if (i != p.size() && p[i] != L'\\') continue;
        auto component = p.substr(start, i - start);
        if (component.empty() || component == L"." || component == L".." || component.back() == L'.' || component.back() == L' ') return ERROR_INVALID_NAME;
        start = i + 1;
    }
    return ERROR_SUCCESS;
}
DWORD Validate(Architecture architecture, const std::wstring& p) {
    if (const DWORD error = ValidatePathSyntax(architecture, p)) return error;
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
    IMAGE_DOS_HEADER dos{}; IMAGE_FILE_HEADER header{}; DWORD got = 0, signature = 0;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && ReadFile(f, &dos, sizeof(dos), &got, nullptr) &&
        got == sizeof(dos) && dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0 &&
        static_cast<LONGLONG>(dos.e_lfanew) + static_cast<LONGLONG>(sizeof(signature) + sizeof(header)) <= size.QuadPart;
    if (ok) {
        LARGE_INTEGER pos{}; pos.QuadPart = dos.e_lfanew;
        ok = SetFilePointerEx(f, pos, nullptr, FILE_BEGIN) && ReadFile(f, &signature, sizeof(signature), &got, nullptr) &&
            got == sizeof(signature) && signature == IMAGE_NT_SIGNATURE && ReadFile(f, &header, sizeof(header), &got, nullptr) &&
            got == sizeof(header) && header.Machine == (architecture == Architecture::X64 ? IMAGE_FILE_MACHINE_AMD64 : IMAGE_FILE_MACHINE_I386) &&
            (header.Characteristics & IMAGE_FILE_DLL) &&
            static_cast<LONGLONG>(dos.e_lfanew) + static_cast<LONGLONG>(sizeof(signature) + sizeof(header) + header.SizeOfOptionalHeader) <= size.QuadPart;
    }
    if (ok && architecture == Architecture::X64) {
        IMAGE_OPTIONAL_HEADER64 optional{};
        ok = header.SizeOfOptionalHeader >= sizeof(optional) && ReadFile(f, &optional, sizeof(optional), &got, nullptr) &&
            got == sizeof(optional) && optional.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    } else if (ok) {
        IMAGE_OPTIONAL_HEADER32 optional{};
        ok = header.SizeOfOptionalHeader >= sizeof(optional) && ReadFile(f, &optional, sizeof(optional), &got, nullptr) &&
            got == sizeof(optional) && optional.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    }
    CloseHandle(f); return ok ? ERROR_SUCCESS : ERROR_BAD_EXE_FORMAT;
}
bool Shape(HKEY key, DWORD expected_subkeys, DWORD expected_values) {
    DWORD subkeys = 0, values = 0;
    return RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS && subkeys == expected_subkeys && values == expected_values;
}
DWORD Cleanup(Architecture architecture, const std::wstring& path) {
    DWORD result = ERROR_SUCCESS;
    for (auto root : roots) {
        Key k; DWORD e = Open(architecture, root, k, false, nullptr, true);
        if (e == ERROR_FILE_NOT_FOUND) continue;
        if (e) { result = e; continue; }
        if (Read(k.h, owner) != token) { result = ERROR_ALREADY_EXISTS; continue; }
        std::wstring child = std::wstring(root) + L"\\InprocServer32";
        Key c; e = Open(architecture, child.c_str(), c, false, nullptr, true);
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
            e = RegDeleteKeyExW(HKEY_CURRENT_USER, child.c_str(), ViewFor(architecture), 0);
            if (e) { result = e; continue; }
        }
        if ((e = RegDeleteValueW(k.h, owner))) { result = e; continue; }
        if (!Empty(k.h)) { Write(k.h, owner, token); result = ERROR_ALREADY_EXISTS; continue; }
        e = RegDeleteKeyExW(HKEY_CURRENT_USER, root, ViewFor(architecture), 0); if (e) result = e;
    }
    return result;
}
}
const wchar_t* DllNameFor(Architecture architecture) noexcept {
    if (architecture == Architecture::X64) return L"PulseDialogAuto64.dll";
    if (architecture == Architecture::X86) return L"PulseDialogAuto32.dll";
    return nullptr;
}
const wchar_t* StubDllNameFor(Architecture architecture) noexcept {
    if (architecture == Architecture::X64) return L"PulseDialogStub64.dll";
    if (architecture == Architecture::X86) return L"PulseDialogStub32.dll";
    return nullptr;
}
bool IsAllowedDialogDllName(Architecture architecture, const wchar_t* file_name) noexcept {
    if (!file_name || !*file_name) return false;
    const wchar_t* auto_name = DllNameFor(architecture);
    const wchar_t* stub_name = StubDllNameFor(architecture);
    return (auto_name && _wcsicmp(file_name, auto_name) == 0) ||
           (stub_name && _wcsicmp(file_name, stub_name) == 0);
}
Status Inspect(Architecture architecture) {
    if (const DWORD error = ArchitectureError(architecture)) return {false, false, error, {}};
    MutationLock lock;
    if (lock.error) {
        diagnostics::runtime::TextEvent("dialog_auto_inspect_failed",
            {{"stage", lock.handle ? L"mutex_wait" : L"mutex_open"}},
            {{"error", lock.error}, {"architecture", static_cast<uint64_t>(architecture)}},
            diagnostics::runtime::Level::Warning);
        return {false, false, lock.error, {}};
    }
    Status s; Key c; DWORD e = Open(architecture, ConfigFor(architecture), c); if (e && e != ERROR_FILE_NOT_FOUND) { s.error = e; return s; }
    s.dll_path = c.h ? Read(c.h, L"DllPath") : std::wstring{}; bool complete = !s.dll_path.empty();
    for (auto root : roots) { Key k, v; e = Open(architecture, root, k); if (e == ERROR_FILE_NOT_FOUND) { complete = false; continue; } if (e) { s.error = e; complete = false; continue; }
        if (Read(k.h, owner) != token) { s.conflict = true; complete = false; continue; }
        std::wstring sub = std::wstring(root) + L"\\InprocServer32";
        if (Open(architecture, sub.c_str(), v) || Read(v.h, nullptr) != s.dll_path || Read(v.h, L"ThreadingModel") != L"Apartment") { complete = false; s.conflict = true; }
    }
    s.enabled = complete && c.h && Number(c.h, L"Enabled") == 1; return s;
}
namespace {
struct MissingJournal {
    Architecture architecture;
    std::wstring path;
    DWORD stage = 0;
    bool stage_present = false;
};
DWORD ReadFlag(HKEY key, const wchar_t* name, DWORD& value) {
    DWORD type = 0, bytes = sizeof(value);
    const DWORD error = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &bytes);
    if (error) return error;
    return type == REG_DWORD && bytes == sizeof(value) ? ERROR_SUCCESS : ERROR_INVALID_DATA;
}
DWORD MissingJournalWitness(Architecture architecture, MissingJournal& witness,
    const MissingJournal* expected = nullptr, bool restoring = false) {
    Key config;
    DWORD error = Open(architecture, ConfigFor(architecture), config);
    if (error) return error;
    const auto recorded = Read(config.h, L"DllPath", &error);
    if (error != ERROR_FILE_NOT_FOUND && !(restoring && expected && !error && recorded == expected->path))
        return error ? error : ERROR_ALREADY_EXISTS;
    DWORD enabled = 0;
    if ((error = ReadFlag(config.h, L"Enabled", enabled))) return error;
    if (enabled != 1) return ERROR_ALREADY_EXISTS;
    witness.architecture = architecture;
    error = ReadFlag(config.h, L"Stage", witness.stage);
    witness.stage_present = error != ERROR_FILE_NOT_FOUND;
    if (error && error != ERROR_FILE_NOT_FOUND) return error;
    if (witness.stage_present && witness.stage > 1) return ERROR_ALREADY_EXISTS;
    for (const auto root : roots) {
        Key key, server;
        if ((error = Open(architecture, root, key))) return error;
        if (Read(key.h, owner) != token || !Shape(key.h, 1, 1)) return ERROR_ALREADY_EXISTS;
        const auto subkey = std::wstring(root) + L"\\InprocServer32";
        if ((error = Open(architecture, subkey.c_str(), server))) return error;
        if (!Shape(server.h, 0, 2) || Read(server.h, L"ThreadingModel") != L"Apartment") return ERROR_ALREADY_EXISTS;
        const auto path = Read(server.h, nullptr, &error);
        if (error) return error;
        if (path.empty() || (!witness.path.empty() && witness.path != path)) return ERROR_ALREADY_EXISTS;
        witness.path = path;
    }
    if (expected && (witness.path != expected->path || witness.stage_present != expected->stage_present ||
        (witness.stage_present && witness.stage != expected->stage))) return ERROR_REVISION_MISMATCH;
    // Recovery of our own metadata write must remain possible if the old file
    // reappears. Forward repair still requires a genuinely retired local DLL.
    if (!restoring && !IsRetiredDisabledPulseDialogDll(architecture, Status{false, false, 0, witness.path}))
        return ERROR_ALREADY_EXISTS;
    return ERROR_SUCCESS;
}
}
JournalRepairResult RepairMissingRegistrationJournals(const std::vector<Architecture>& architectures) {
    JournalRepairResult result;
    MutationLock lock;
    if (lock.error) { result.error = lock.error; return result; }
    std::vector<MissingJournal> pending;
    size_t attempted = 0;
    try {
        auto repair = [&]() -> DWORD {
            if (architectures.empty() || architectures.size() > 2) return ERROR_INVALID_PARAMETER;
            pending.reserve(architectures.size());
            for (size_t i = 0; i < architectures.size(); ++i) {
                const auto architecture = architectures[i];
                if (const DWORD error = ArchitectureError(architecture)) return error;
                if (i && architecture == architectures[0]) return ERROR_INVALID_PARAMETER;
                const auto status = Inspect(architecture);
                if (status.error) return status.error;
                if (!status.conflict) continue;
                MissingJournal witness{architecture};
                if (const DWORD error = MissingJournalWitness(architecture, witness)) return error;
                pending.push_back(std::move(witness));
            }
            for (const auto& item : pending) {
                MissingJournal current{item.architecture};
                if (const DWORD error = MissingJournalWitness(item.architecture, current, &item)) return error;
                Key key;
                DWORD error = Open(item.architecture, ConfigFor(item.architecture), key, false, nullptr, true);
                if (error) return error;
                ++attempted;
                if ((error = Write(key.h, L"DllPath", item.path)) || (error = RegFlushKey(key.h))) return error;
                const auto observed = Inspect(item.architecture);
                if (observed.error) return observed.error;
                if (!observed.enabled || observed.conflict || observed.dll_path != item.path) return ERROR_REVISION_MISMATCH;
                ++result.repaired;
            }
            return ERROR_SUCCESS;
        };
        result.error = repair();
    } catch (const std::bad_alloc&) { result.error = ERROR_NOT_ENOUGH_MEMORY; }
    catch (...) { result.error = ERROR_UNHANDLED_EXCEPTION; }
    if (result.error) {
        for (size_t i = attempted; i > 0; --i) {
            const auto& item = pending[i - 1];
            DWORD error = ERROR_SUCCESS;
            try {
                MissingJournal current{item.architecture};
                error = MissingJournalWitness(item.architecture, current, &item, true);
                Key key;
                if (!error) error = Open(item.architecture, ConfigFor(item.architecture), key, false, nullptr, true);
                if (!error) {
                    error = RegDeleteValueW(key.h, L"DllPath");
                    if (error == ERROR_FILE_NOT_FOUND) error = ERROR_SUCCESS;
                    if (!error) error = RegFlushKey(key.h);
                }
            } catch (...) { error = ERROR_UNHANDLED_EXCEPTION; }
            if (error && !result.rollback_error) result.rollback_error = error;
        }
        if (!result.rollback_error) result.repaired = 0;
    }
    diagnostics::runtime::Event("dialog_auto_journal_repair", {{"error", result.error},
        {"rollback_error", result.rollback_error}, {"repaired", result.repaired}});
    return result;
}
DWORD Disable(Architecture architecture) {
    if (const DWORD error = ArchitectureError(architecture)) return error;
    MutationLock lock; if (lock.error) return lock.error;
    Key c; DWORD e = Open(architecture, ConfigFor(architecture), c, false, nullptr, true); if (e == ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS; if (e) return e;
    if ((e = Flag(c.h, L"Enabled", 0))) return e;
    RegFlushKey(c.h); e = Cleanup(architecture, Read(c.h, L"DllPath"));
    if (!e) Flag(c.h, L"Stage", 0);
    diagnostics::runtime::Event("dialog_auto_disable", {{"error", e}}); return e;
}
DWORD DisableFor(Architecture architecture, const std::wstring& absolute_dll) {
    if (const DWORD error = ArchitectureError(architecture)) return error;
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
    Key key; DWORD error=Open(architecture, ConfigFor(architecture),key);
    if(error && error!=ERROR_FILE_NOT_FOUND) return error;
    const auto recorded=key.h ? normalize(Read(key.h,L"DllPath",&error)) : std::wstring{};
    if(error && error!=ERROR_FILE_NOT_FOUND) return error;
    if(!recorded.empty() && CompareStringOrdinal(expected.c_str(),-1,recorded.c_str(),-1,TRUE)==CSTR_EQUAL) return Disable(architecture);
    // A missing/stale journal must not permit removal of a still-referenced DLL.
    for(auto root:roots) {
        Key child;const auto sub=std::wstring(root)+L"\\InprocServer32";
        const DWORD opened=Open(architecture, sub.c_str(),child);
        if(opened==ERROR_FILE_NOT_FOUND) continue;
        if(opened) return opened;
        const auto referenced=normalize(Read(child.h,nullptr));
        if(!referenced.empty() && CompareStringOrdinal(expected.c_str(),-1,referenced.c_str(),-1,TRUE)==CSTR_EQUAL) return ERROR_ALREADY_EXISTS;
    }
    return ERROR_SUCCESS;
}
namespace {
DWORD EnableImpl(Architecture architecture, const std::wstring& p, bool* mutation_attempted) {
    if (mutation_attempted) *mutation_attempted = false;
    MutationLock lock; if (lock.error) return lock.error;
    DWORD e = Validate(architecture, p); if (e) return e;
    Status s = Inspect(architecture); if (s.enabled && s.dll_path == p) return ERROR_SUCCESS;
    if (mutation_attempted) *mutation_attempted = true;
    Key c; if ((e = Open(architecture, ConfigFor(architecture), c, true))) return e;
    if (Number(c.h, L"Stage")) { if ((e = Disable(architecture))) return e; }
    for (auto root : roots) { Key k; e = Open(architecture, root, k); if (!e) return ERROR_ALREADY_EXISTS; if (e != ERROR_FILE_NOT_FOUND) return e; }
    if ((e = Flag(c.h, L"Enabled", 0)) || (e = Write(c.h, L"DllPath", p)) || (e = Flag(c.h, L"Stage", 1)) || (e = RegFlushKey(c.h))) return e;
    for (auto root : roots) {
        Key k, v; DWORD disposition = 0; e = Open(architecture, root, k, true, &disposition);
        if (!e && disposition != REG_CREATED_NEW_KEY) e = ERROR_ALREADY_EXISTS;
        if (!e) e = Write(k.h, owner, token);
        std::wstring sub = std::wstring(root) + L"\\InprocServer32";
        if (!e) e = Open(architecture, sub.c_str(), v, true);
        if (!e) e = Write(v.h, nullptr, p);
        if (!e) e = Write(v.h, L"ThreadingModel", L"Apartment");
        if (e) { Cleanup(architecture, p); return e; }
    }
    e = Flag(c.h, L"Enabled", 1); if (e) Cleanup(architecture, p);
    diagnostics::runtime::Event("dialog_auto_enable", {{"error", e}}); return e;
}
}
DWORD Enable(Architecture architecture, const std::wstring& p) {
    return EnableImpl(architecture, p, nullptr);
}
DWORD EnableIfUnclaimed(Architecture architecture, const std::wstring& p) {
    MutationLock lock; if (lock.error) return lock.error;
    DWORD error = Validate(architecture, p); if (error) return error;
    Key key; error = Open(architecture, ConfigFor(architecture), key);
    if (error && error != ERROR_FILE_NOT_FOUND) return error;
    if (key.h) {
        const auto recorded = Read(key.h, L"DllPath", &error);
        if (error && error != ERROR_FILE_NOT_FOUND) return error;
        if (!recorded.empty() && recorded != p) return ERROR_ALREADY_EXISTS;
    }
    return Enable(architecture, p);
}
namespace {
DWORD RebindDisabled(Architecture architecture, const std::wstring& expected_old_dll,
    const std::wstring& new_dll, bool retired, bool* mutation_attempted) {
    if (mutation_attempted) *mutation_attempted = false;
    MutationLock lock;
    if (lock.error) return lock.error;
    DWORD error = ValidatePathSyntax(architecture, expected_old_dll);
    if (error) return error;
    if ((error = Validate(architecture, new_dll))) return error;
    Key key;
    if ((error = Open(architecture, ConfigFor(architecture), key))) return error;
    const auto recorded = Read(key.h, L"DllPath", &error);
    if (error) return error;
    if (recorded.size() != expected_old_dll.size() ||
        CompareStringOrdinal(recorded.c_str(), static_cast<int>(recorded.size()),
            expected_old_dll.c_str(), static_cast<int>(expected_old_dll.size()), TRUE) != CSTR_EQUAL) {
        return ERROR_ALREADY_EXISTS;
    }
    if ((error = RequireZeroFlag(key.h, L"Enabled")) || (error = RequireZeroFlag(key.h, L"Stage"))) return error;
    for (auto root : roots) {
        Key existing;
        error = Open(architecture, root, existing);
        if (!error) return ERROR_ALREADY_EXISTS;
        if (error != ERROR_FILE_NOT_FOUND) return error;
    }
    // Enable uses this same recursive mutation mutex. The old journal receipt,
    // both empty CLSID slots and the new registration remain one locked action.
    if (retired && !IsRetiredDisabledPulseDialogDll(architecture, Status{false, false, 0, recorded}))
        return ERROR_ALREADY_EXISTS;
    return EnableImpl(architecture, new_dll, mutation_attempted);
}
}
DWORD EnableIfDisabledFor(Architecture architecture, const std::wstring& expected_old_dll, const std::wstring& new_dll) {
    return RebindDisabled(architecture, expected_old_dll, new_dll, false, nullptr);
}
DWORD EnableIfRetiredDisabledFor(Architecture architecture, const std::wstring& expected_old_dll,
    const std::wstring& new_dll, bool* mutation_attempted) {
    return RebindDisabled(architecture, expected_old_dll, new_dll, true, mutation_attempted);
}
Status Inspect() { return Inspect(CurrentArchitecture); }
DWORD Enable(const std::wstring& path) { return Enable(CurrentArchitecture, path); }
DWORD EnableIfUnclaimed(const std::wstring& path) { return EnableIfUnclaimed(CurrentArchitecture, path); }
DWORD EnableIfDisabledFor(const std::wstring& old_path, const std::wstring& new_path) { return EnableIfDisabledFor(CurrentArchitecture, old_path, new_path); }
DWORD Disable() { return Disable(CurrentArchitecture); }
DWORD DisableFor(const std::wstring& path) { return DisableFor(CurrentArchitecture, path); }
bool IsEnabledFor(const std::wstring& p) noexcept { try { auto s = Inspect(); return s.enabled && !s.conflict && !s.error && s.dll_path == p; } catch (...) { return false; } }
bool PreferOutOfProcess() noexcept {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, ConfigKey, 0, KEY_QUERY_VALUE | RegistryView, &key) != ERROR_SUCCESS) return true;
    DWORD type = 0, value = 1, size = sizeof(value);
    const LSTATUS read = RegQueryValueExW(key, L"Host", nullptr, &type, reinterpret_cast<BYTE*>(&value), &size);
    RegCloseKey(key);
    return read != ERROR_SUCCESS || type != REG_DWORD || size != sizeof(value) || value != 0;
}
// Remote host is the default; a REG_DWORD RemoteHost=0 opts back into the
// in-process UI. A failed remote start still falls back to in-process.
bool PreferRemoteHost() noexcept {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, ConfigKey, 0, KEY_QUERY_VALUE | RegistryView, &key) != ERROR_SUCCESS) return true;
    DWORD type = 0, value = 0, size = sizeof(value);
    const LSTATUS read = RegQueryValueExW(key, L"RemoteHost", nullptr, &type, reinterpret_cast<BYTE*>(&value), &size);
    RegCloseKey(key);
    if (read != ERROR_SUCCESS || type != REG_DWORD || size != sizeof(value)) return true;
    return value != 0;
}
}




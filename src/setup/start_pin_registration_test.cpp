#include "setup_common.h"
#include "setup_registry.h"
#include "setup_dialog_versions.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>

namespace fake {
const std::wstring clsid = L"Software\\Classes\\CLSID\\{B3C9ED45-7A51-48A2-9DF1-D46BA187A312}";
const std::wstring server = clsid + L"\\InprocServer32";
const std::wstring extension = L"Software\\Classes\\.pulse-start-pin";
const std::wstring progid = L"Software\\Classes\\Pulse.StartPin.Request";
const std::wstring shell = progid + L"\\shell";
const std::wstring verb = shell + L"\\PulseStartPin";
const std::wstring command = verb + L"\\command";
const std::wstring runtime = L"C:\\fixture\\DialogBridge\\verified-runtime\\pulse_start_pin.dll";

struct Value {
    DWORD type = REG_NONE;
    std::vector<BYTE> bytes;
    bool operator==(const Value&) const = default;
};
struct Key {
    REGSAM allowed = KEY_ALL_ACCESS;
    std::map<std::wstring, Value> values;
    bool operator==(const Key&) const = default;
};
struct Handle { std::wstring path; REGSAM access; };
std::map<std::wstring, Key> keys;
std::map<HKEY, Handle> handles;
std::vector<std::wstring> create_attempts, created, deleted;
ULONG_PTR next_handle = 0;
unsigned set_calls = 0, fail_set_at = 0, notifications = 0;
bool set_failure_seen = false;
std::wstring deny_create, race_create;
Key raced_key;

Value String(const wchar_t* value, DWORD type = REG_SZ) {
    const auto* first = reinterpret_cast<const BYTE*>(value);
    return {type, {first, first + (wcslen(value) + 1) * sizeof(wchar_t)}};
}

LSTATUS Bind(const std::wstring& path, REGSAM access, PHKEY output) {
    const auto found = keys.find(path);
    if (found == keys.end()) return ERROR_FILE_NOT_FOUND;
    const REGSAM rights = access & ~(KEY_WOW64_64KEY | KEY_WOW64_32KEY);
    if (!(access & KEY_WOW64_64KEY) || (rights & ~found->second.allowed)) return ERROR_ACCESS_DENIED;
    *output = reinterpret_cast<HKEY>(++next_handle);
    handles.emplace(*output, Handle{path, rights});
    return ERROR_SUCCESS;
}

LSTATUS WINAPI Open(HKEY root, LPCWSTR path, DWORD, REGSAM access, PHKEY output) {
    if (root != HKEY_LOCAL_MACHINE) return ERROR_INVALID_HANDLE;
    return Bind(path, access, output);
}

LSTATUS WINAPI Create(HKEY root, LPCWSTR path, DWORD, LPWSTR, DWORD, REGSAM access,
                     const LPSECURITY_ATTRIBUTES, PHKEY output, LPDWORD disposition) {
    if (root != HKEY_LOCAL_MACHINE) return ERROR_INVALID_HANDLE;
    const std::wstring name(path);
    create_attempts.push_back(name);
    if (name == deny_create) return ERROR_ACCESS_DENIED;
    if (!keys.contains(name) && name == race_create) keys.emplace(name, raced_key);
    if (keys.contains(name)) {
        *disposition = REG_OPENED_EXISTING_KEY;
        return Bind(name, access, output);
    }
    const auto separator = name.find_last_of(L'\\');
    const auto parent = keys.find(name.substr(0, separator));
    if (separator == std::wstring::npos || parent == keys.end()) return ERROR_PATH_NOT_FOUND;
    if (!(parent->second.allowed & KEY_CREATE_SUB_KEY)) return ERROR_ACCESS_DENIED;
    keys.emplace(name, Key{});
    created.push_back(name);
    *disposition = REG_CREATED_NEW_KEY;
    return Bind(name, access, output);
}

LSTATUS WINAPI Close(HKEY handle) {
    return handles.erase(handle) ? ERROR_SUCCESS : ERROR_INVALID_HANDLE;
}

LSTATUS WINAPI Query(HKEY handle, LPCWSTR name, LPDWORD, LPDWORD type, LPBYTE data, LPDWORD size) {
    const auto opened = handles.find(handle);
    if (opened == handles.end()) return ERROR_INVALID_HANDLE;
    if (!(opened->second.access & KEY_QUERY_VALUE)) return ERROR_ACCESS_DENIED;
    const auto& values = keys.at(opened->second.path).values;
    const auto found = values.find(name);
    if (found == values.end()) return ERROR_FILE_NOT_FOUND;
    if (type) *type = found->second.type;
    const DWORD required = static_cast<DWORD>(found->second.bytes.size());
    const DWORD capacity = *size;
    *size = required;
    if (!data) return ERROR_SUCCESS;
    if (capacity < required) return ERROR_MORE_DATA;
    std::copy(found->second.bytes.begin(), found->second.bytes.end(), data);
    return ERROR_SUCCESS;
}

LSTATUS WINAPI Set(HKEY handle, LPCWSTR name, DWORD, DWORD type, const BYTE* data, DWORD size) {
    const auto opened = handles.find(handle);
    if (opened == handles.end()) return ERROR_INVALID_HANDLE;
    if (!(opened->second.access & KEY_SET_VALUE)) return ERROR_ACCESS_DENIED;
    if (++set_calls == fail_set_at) { set_failure_seen = true; return ERROR_ACCESS_DENIED; }
    keys.at(opened->second.path).values[name] = {type, {data, data + size}};
    return ERROR_SUCCESS;
}

LSTATUS WINAPI DeleteValue(HKEY handle, LPCWSTR name) {
    const auto opened = handles.find(handle);
    if (opened == handles.end()) return ERROR_INVALID_HANDLE;
    if (!(opened->second.access & KEY_SET_VALUE)) return ERROR_ACCESS_DENIED;
    return keys.at(opened->second.path).values.erase(name) ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND;
}

LSTATUS WINAPI DeleteKey(HKEY root, LPCWSTR name, REGSAM view, DWORD) {
    if (root != HKEY_LOCAL_MACHINE || view != KEY_WOW64_64KEY) return ERROR_INVALID_PARAMETER;
    const std::wstring prefix = std::wstring(name) + L"\\";
    for (const auto& entry : keys) if (entry.first.starts_with(prefix)) return ERROR_ACCESS_DENIED;
    if (!keys.erase(name)) return ERROR_FILE_NOT_FOUND;
    deleted.emplace_back(name);
    return ERROR_SUCCESS;
}

LSTATUS WINAPI DeleteTree(HKEY, LPCWSTR) { return ERROR_CALL_NOT_IMPLEMENTED; }
void WINAPI Notify(LONG, UINT, LPCVOID, LPCVOID) { ++notifications; }

void Reset(bool complete = true) {
    keys.clear(); handles.clear(); create_attempts.clear(); created.clear(); deleted.clear();
    next_handle = set_calls = fail_set_at = notifications = 0;
    set_failure_seen = false; deny_create.clear(); race_create.clear(); raced_key = {};
    keys[L"Software"].allowed = KEY_READ;
    keys[L"Software\\Classes"].allowed = KEY_READ;
    keys[L"Software\\Classes\\CLSID"].allowed = KEY_READ;
    keys[clsid].allowed = KEY_READ | KEY_CREATE_SUB_KEY;
    keys[progid].allowed = KEY_READ | KEY_CREATE_SUB_KEY;
    keys[progid].values[L"Unrelated"] = {REG_BINARY, {1, 3, 5, 7}};
    keys[extension].allowed = KEY_QUERY_VALUE | KEY_SET_VALUE;
    keys[extension].values[L""] = {REG_BINARY, {9, 8, 7}};
    if (!complete) return;
    keys[server].allowed = KEY_QUERY_VALUE | KEY_SET_VALUE;
    keys[server].values[L""] = String(L"%OLD_RUNTIME%\\pin.dll", REG_EXPAND_SZ);
    keys[server].values[L"ThreadingModel"] = String(L"Both");
    keys[shell].allowed = KEY_READ;
    keys[verb].allowed = KEY_READ | KEY_SET_VALUE;
    keys[verb].values[L""] = String(L"Old label");
    keys[command].allowed = KEY_QUERY_VALUE | KEY_SET_VALUE;
    keys[command].values[L""] = String(L"old command");
    keys[command].values[L"DelegateExecute"] = String(L"{OLD-OWNER}");
}
} // namespace fake

namespace pulse::setup {
std::wstring InstalledRuntimeFile(const std::wstring&, const wchar_t*) { return fake::runtime; }
bool IsDialogVersionFile(const std::wstring&) { return false; }
std::optional<std::wstring> ReadString(HKEY, REGSAM, const wchar_t*, const wchar_t*) { return std::nullopt; }
} // namespace pulse::setup

// The production registration code runs against a permission-aware registry
// model. Every registry and shell-notification boundary is intercepted; these
// tests cannot read or mutate the machine's real HKLM state.
#define RegOpenKeyExW fake::Open
#define RegCreateKeyExW fake::Create
#define RegCloseKey fake::Close
#define RegQueryValueExW fake::Query
#define RegSetValueExW fake::Set
#define RegDeleteValueW fake::DeleteValue
#define RegDeleteKeyExW fake::DeleteKey
#define RegDeleteTreeW fake::DeleteTree
#define SHChangeNotify fake::Notify
#include "start_pin_registration.cpp"

int main() {
    unsigned checks = 0, failures = 0;
    const auto check = [&](bool ok, const char* label) {
        ++checks; if (!ok) ++failures;
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    };
    const auto run = [](std::wstring& error) { return pulse::setup::RegisterStartPin(L"C:\\fixture", error); };
    {
        fake::Reset();
        std::wstring error;
        check(run(error) && error.empty(), "read-only existing ancestors and query/set-only leaves permit registration");
        check(fake::keys[fake::server].values[L""] == fake::String(fake::runtime.c_str()) &&
            fake::keys[fake::server].values[L"ThreadingModel"] == fake::String(L"Apartment") &&
            fake::keys[fake::extension].values[L""] == fake::String(L"Pulse.StartPin.Request") &&
            fake::keys[fake::verb].values[L""] == fake::String(L"Pulse") &&
            fake::keys[fake::command].values[L""] == fake::String(L"") &&
            fake::keys[fake::command].values[L"DelegateExecute"] == fake::String(L"{B3C9ED45-7A51-48A2-9DF1-D46BA187A312}"),
            "successful registration writes all six intended values");
        check(fake::create_attempts.empty() && fake::handles.empty(), "existing keys are opened without creation and all handles close");
    }
    {
        fake::Reset();
        const auto before = fake::keys;
        fake::fail_set_at = 6;
        std::wstring error;
        check(!run(error) && fake::set_failure_seen && !error.empty(), "late value write failure is reported");
        check(fake::keys == before && fake::deleted.empty() && fake::handles.empty(),
            "rollback restores exact previous value bytes/types and never deletes existing keys");
    }
    {
        fake::Reset(false);
        const auto before = fake::keys;
        fake::fail_set_at = 6;
        std::wstring error;
        check(!run(error) && fake::set_failure_seen, "registration reaches final write after creating missing descendants");
        check(fake::created == std::vector<std::wstring>{fake::server, fake::shell, fake::verb, fake::command} &&
            fake::deleted == std::vector<std::wstring>{fake::command, fake::verb, fake::shell, fake::server},
            "rollback removes every newly created key in child-before-parent order");
        check(fake::keys == before && fake::handles.empty(), "new-key rollback preserves pre-existing ancestors and values");
    }
    {
        fake::Reset(false);
        const auto before = fake::keys;
        fake::deny_create = fake::verb;
        std::wstring error;
        check(!run(error) && fake::created == std::vector<std::wstring>{fake::server, fake::shell},
            "failed creation records only keys actually created by this attempt");
        check(fake::keys == before && fake::handles.empty(), "creation failure also rolls back earlier values and new keys");
    }
    {
        fake::Reset();
        fake::keys[L"Software\\Classes"].allowed = 0;
        const auto before = fake::keys;
        std::wstring error;
        check(!run(error) && fake::create_attempts.empty(), "access denial is not retried as missing-key creation");
        check(fake::keys == before && fake::handles.empty(), "unreadable ancestor leaves registry model unchanged");
    }
    {
        fake::Reset();
        fake::raced_key = fake::keys.at(fake::server);
        fake::keys.erase(fake::server);
        fake::race_create = fake::server;
        auto expected = fake::keys;
        expected.emplace(fake::server, fake::raced_key);
        fake::fail_set_at = 6;
        std::wstring error;
        check(!run(error) && fake::set_failure_seen && fake::create_attempts == std::vector<std::wstring>{fake::server},
            "open/create race accepts an existing key without claiming it as newly created");
        check(fake::keys == expected && fake::deleted.empty() && fake::handles.empty(),
            "rollback preserves a key that appeared between open and create");
    }
    std::printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}

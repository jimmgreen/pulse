#include <windows.h>
#include "../common/dialog_auto_registration.h"
#include "../common/runtime_log.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <cstdio>

// This backend models views explicitly. It makes no OS registry calls and does
// not pretend RegOverridePredefKey private paths are two redirected CLSID hives.
namespace fixture {
struct Value { DWORD type = 0; std::vector<BYTE> bytes; };
struct Node { REGSAM view = 0; std::wstring path; std::map<std::wstring, Value> values; };
using Id = std::pair<REGSAM, std::wstring>;
std::map<Id, std::shared_ptr<Node>> nodes;
std::vector<std::shared_ptr<Node>> lifetime;
unsigned registry_calls = 0, mutation_calls = 0, depth = 0, max_depth = 0;
bool mutex_name_ok = true;
WORD processor = PROCESSOR_ARCHITECTURE_AMD64;
const HANDLE mutex = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(0x50414d01u));
std::wstring Lower(std::wstring value) {
    for (auto& c : value) if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
    return value;
}
Node* As(HKEY key) { return reinterpret_cast<Node*>(key); }
Id Key(HKEY parent, LPCWSTR path, REGSAM flags) {
    REGSAM view = flags & (KEY_WOW64_64KEY | KEY_WOW64_32KEY);
    std::wstring prefix;
    if (parent != HKEY_CURRENT_USER) { prefix = As(parent)->path; if (!view) view = As(parent)->view; }
    if (view != KEY_WOW64_64KEY && view != KEY_WOW64_32KEY) throw std::runtime_error("missing explicit registry view");
    if (path && *path) { if (!prefix.empty()) prefix += L'\\'; prefix += path; }
    return {view, Lower(prefix)};
}
LSTATUS WINAPI Create(HKEY parent, LPCWSTR path, DWORD, LPWSTR, DWORD, REGSAM flags, const LPSECURITY_ATTRIBUTES, PHKEY out, LPDWORD disposition) {
    ++mutation_calls;
    ++registry_calls; const auto id = Key(parent, path, flags); auto found = nodes.find(id);
    if (found == nodes.end()) {
        auto node = std::make_shared<Node>(); node->view = id.first; node->path = id.second;
        lifetime.push_back(node); found = nodes.emplace(id, std::move(node)).first;
        if (disposition) *disposition = REG_CREATED_NEW_KEY;
    } else if (disposition) *disposition = REG_OPENED_EXISTING_KEY;
    *out = reinterpret_cast<HKEY>(found->second.get()); return ERROR_SUCCESS;
}
LSTATUS WINAPI Open(HKEY parent, LPCWSTR path, DWORD, REGSAM flags, PHKEY out) {
    ++registry_calls; const auto found = nodes.find(Key(parent, path, flags));
    if (found == nodes.end()) return ERROR_FILE_NOT_FOUND;
    *out = reinterpret_cast<HKEY>(found->second.get()); return ERROR_SUCCESS;
}
LSTATUS WINAPI Close(HKEY) { return ERROR_SUCCESS; }
LSTATUS WINAPI Set(HKEY key, LPCWSTR name, DWORD, DWORD type, const BYTE* bytes, DWORD size) {
    ++mutation_calls;
    ++registry_calls; As(key)->values[Lower(name ? name : L"")] = {type, std::vector<BYTE>(bytes, bytes + size)}; return ERROR_SUCCESS;
}
LSTATUS WINAPI Query(HKEY key, LPCWSTR name, LPDWORD, LPDWORD type, LPBYTE data, LPDWORD size) {
    ++registry_calls; const auto& values = As(key)->values; const auto found = values.find(Lower(name ? name : L""));
    if (found == values.end()) return ERROR_FILE_NOT_FOUND;
    if (type) *type = found->second.type;
    if (!size) return ERROR_INVALID_PARAMETER;
    const DWORD needed = static_cast<DWORD>(found->second.bytes.size()); const DWORD capacity = *size; *size = needed;
    if (!data) return ERROR_SUCCESS;
    if (capacity < needed) return ERROR_MORE_DATA;
    if (needed) std::memcpy(data, found->second.bytes.data(), needed);
    return ERROR_SUCCESS;
}
LSTATUS WINAPI Get(HKEY key, LPCWSTR subkey, LPCWSTR name, DWORD flags, LPDWORD type, PVOID data, LPDWORD size) {
    if (subkey && *subkey) return ERROR_NOT_SUPPORTED;
    DWORD actual = 0; const auto result = Query(key, name, nullptr, &actual, static_cast<LPBYTE>(data), size);
    if (!result && (flags & RRF_RT_REG_DWORD) && actual != REG_DWORD) return ERROR_UNSUPPORTED_TYPE;
    if (type) *type = actual; return result;
}
DWORD Children(Node* node) {
    DWORD count = 0; const auto prefix = node->path + L'\\';
    for (const auto& [id, child] : nodes) if (id.first == node->view && id.second.starts_with(prefix) && id.second.find(L'\\', prefix.size()) == std::wstring::npos) ++count;
    return count;
}
LSTATUS WINAPI Info(HKEY key, LPWSTR, LPDWORD, LPDWORD, LPDWORD subkeys, LPDWORD, LPDWORD, LPDWORD values, LPDWORD, LPDWORD, LPDWORD, PFILETIME) {
    ++registry_calls; if (subkeys) *subkeys = Children(As(key)); if (values) *values = static_cast<DWORD>(As(key)->values.size()); return ERROR_SUCCESS;
}
LSTATUS WINAPI DeleteValue(HKEY key, LPCWSTR name) {
    ++mutation_calls;
    ++registry_calls; return As(key)->values.erase(Lower(name ? name : L"")) ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND;
}
LSTATUS WINAPI DeleteKey(HKEY parent, LPCWSTR path, REGSAM flags, DWORD) {
    ++mutation_calls;
    ++registry_calls; const auto found = nodes.find(Key(parent, path, flags)); if (found == nodes.end()) return ERROR_FILE_NOT_FOUND;
    if (!found->second->values.empty() || Children(found->second.get())) return ERROR_ACCESS_DENIED;
    nodes.erase(found); return ERROR_SUCCESS;
}
LSTATUS WINAPI Flush(HKEY) { return ERROR_SUCCESS; }
HANDLE WINAPI CreateMutex(LPSECURITY_ATTRIBUTES, BOOL, LPCWSTR name) {
    mutex_name_ok &= name && std::wstring(name) == L"Global\\Pulse.DialogAuto.Registration.v1"; return mutex;
}
DWORD WINAPI Wait(HANDLE handle, DWORD milliseconds) {
    if (handle != mutex) return ::WaitForSingleObject(handle, milliseconds);
    ++depth; max_depth = (std::max)(max_depth, depth); return WAIT_OBJECT_0;
}
BOOL WINAPI Release(HANDLE handle) { if (handle != mutex || !depth) return FALSE; --depth; return TRUE; }
BOOL WINAPI CloseHandle(HANDLE handle) { return handle == mutex ? TRUE : ::CloseHandle(handle); }
void WINAPI NativeInfo(LPSYSTEM_INFO out) { SYSTEM_INFO value{}; value.wProcessorArchitecture = processor; *out = value; }
}

#define RegCreateKeyExW fixture::Create
#define RegOpenKeyExW fixture::Open
#define RegCloseKey fixture::Close
#define RegSetValueExW fixture::Set
#define RegQueryValueExW fixture::Query
#define RegGetValueW fixture::Get
#define RegQueryInfoKeyW fixture::Info
#define RegDeleteValueW fixture::DeleteValue
#define RegDeleteKeyExW fixture::DeleteKey
#define RegFlushKey fixture::Flush
#define CreateMutexW fixture::CreateMutex
#define WaitForSingleObject fixture::Wait
#define ReleaseMutex fixture::Release
#define CloseHandle fixture::CloseHandle
#define GetNativeSystemInfo fixture::NativeInfo
#include "../common/dialog_auto_registration.cpp"
#undef RegCreateKeyExW
#undef RegOpenKeyExW
#undef RegCloseKey
#undef RegSetValueExW
#undef RegQueryValueExW
#undef RegGetValueW
#undef RegQueryInfoKeyW
#undef RegDeleteValueW
#undef RegDeleteKeyExW
#undef RegFlushKey
#undef CreateMutexW
#undef WaitForSingleObject
#undef ReleaseMutex
#undef CloseHandle
#undef GetNativeSystemInfo

namespace pulse::diagnostics::runtime { void Event(const char*, std::initializer_list<Field>, Level) noexcept {} }
namespace {
unsigned checks = 0, failures = 0;
void Check(bool value, const char* text) { ++checks; failures += !value; std::printf("[%s] %s\n", value ? "PASS" : "FAIL", text); }
void Pe(const std::filesystem::path& path, WORD machine, bool pe64, bool wrong_magic = false, bool truncate = false) {
    IMAGE_DOS_HEADER dos{}; dos.e_magic = IMAGE_DOS_SIGNATURE; dos.e_lfanew = 0x80;
    IMAGE_FILE_HEADER file{}; file.Machine = machine; file.Characteristics = IMAGE_FILE_DLL;
    file.SizeOfOptionalHeader = static_cast<WORD>(pe64 ? sizeof(IMAGE_OPTIONAL_HEADER64) : sizeof(IMAGE_OPTIONAL_HEADER32));
    std::vector<BYTE> bytes(0x80 + sizeof(DWORD) + sizeof(file) + file.SizeOfOptionalHeader);
    const DWORD signature = IMAGE_NT_SIGNATURE;
    const WORD magic = wrong_magic ? static_cast<WORD>(pe64 ? IMAGE_NT_OPTIONAL_HDR32_MAGIC : IMAGE_NT_OPTIONAL_HDR64_MAGIC) :
        static_cast<WORD>(pe64 ? IMAGE_NT_OPTIONAL_HDR64_MAGIC : IMAGE_NT_OPTIONAL_HDR32_MAGIC);
    std::memcpy(bytes.data(), &dos, sizeof(dos)); std::memcpy(bytes.data() + 0x80, &signature, sizeof(signature));
    std::memcpy(bytes.data() + 0x84, &file, sizeof(file)); std::memcpy(bytes.data() + 0x84 + sizeof(file), &magic, sizeof(magic));
    if (truncate) bytes.resize(0x84 + sizeof(file) + sizeof(magic));
    std::ofstream output(path, std::ios::binary | std::ios::trunc); output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}
}
#if !defined(PULSE_REGISTRATION_ARCH_FIXTURE_ONLY)
int main() {
    using namespace pulse::dialog_auto;
    const auto root = std::filesystem::absolute(L"bench_data") / (L"registration-arch-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!std::filesystem::create_directory(root)) return 2;
    std::filesystem::create_directory(root / L"other");
    const auto p64 = root / DllNameFor(Architecture::X64), p32 = root / DllNameFor(Architecture::X86);
    const auto other = root / L"other" / DllNameFor(Architecture::X86);
    Pe(p64, IMAGE_FILE_MACHINE_AMD64, true); Pe(p32, IMAGE_FILE_MACHINE_I386, false); Pe(other, IMAGE_FILE_MACHINE_I386, false);
    Check(Enable(Architecture::X64, p64.wstring()) == ERROR_SUCCESS, "explicit x64 validates PE32+ and selects its fake registry view");
    Check(Enable(Architecture::X86, p32.wstring()) == ERROR_SUCCESS, "explicit x86 validates PE32 independent of caller pointer size");
    Check(Inspect(Architecture::X64).enabled && Inspect(Architecture::X64).dll_path == p64 && Inspect(Architecture::X86).enabled && Inspect(Architecture::X86).dll_path == p32,
        "both architecture configurations and CLSIDs coexist in the explicit backend");
    const auto native = CurrentArchitecture == Architecture::X64 ? p64 : p32;
    const auto opposite = CurrentArchitecture == Architecture::X64 ? Architecture::X86 : Architecture::X64;
    Check(Inspect().dll_path == native && Enable(native.wstring()) == ERROR_SUCCESS, "legacy no-architecture APIs retain caller architecture behavior");
    Check(DisableFor(native.wstring()) == ERROR_SUCCESS && !Inspect().enabled && Inspect(opposite).enabled, "legacy owned disable cannot mutate the other architecture");
    Check(Enable(native.wstring()) == ERROR_SUCCESS, "legacy enable restores only its own architecture");
    Check(EnableIfUnclaimed(Architecture::X86, p32.wstring()) == ERROR_SUCCESS, "explicit strict enable is recursively locked and idempotent");
    Check(EnableIfUnclaimed(Architecture::X86, other.wstring()) == ERROR_ALREADY_EXISTS && Inspect(Architecture::X86).dll_path == p32,
        "strict cross-architecture enable preserves another bundle journal");
    Check(DisableFor(Architecture::X86, other.wstring()) == ERROR_SUCCESS && Inspect(Architecture::X86).enabled,
        "unrelated uninstall cannot disable a registered architecture");
    Check(Disable(Architecture::X86) == ERROR_SUCCESS && !Inspect(Architecture::X86).enabled && Inspect(Architecture::X64).enabled,
        "explicit x86 disable leaves x64 enabled");
    const unsigned before_bad = fixture::registry_calls;
    Pe(p32, IMAGE_FILE_MACHINE_AMD64, false);
    Check(Enable(Architecture::X86, p32.wstring()) == ERROR_BAD_EXE_FORMAT && fixture::registry_calls == before_bad, "wrong PE machine is rejected before registry access");
    Pe(p32, IMAGE_FILE_MACHINE_I386, false, true);
    Check(Enable(Architecture::X86, p32.wstring()) == ERROR_BAD_EXE_FORMAT && fixture::registry_calls == before_bad, "PE32 machine with PE32+ magic is rejected");
    Pe(p32, IMAGE_FILE_MACHINE_I386, false, false, true);
    Check(Enable(Architecture::X86, p32.wstring()) == ERROR_BAD_EXE_FORMAT && fixture::registry_calls == before_bad, "truncated declared optional header is rejected");
    Pe(p32, IMAGE_FILE_MACHINE_I386, false);
    const auto invalid = static_cast<Architecture>(99);
    Check(!DllNameFor(invalid) && Inspect(invalid).error == ERROR_INVALID_PARAMETER && Enable(invalid, p32.wstring()) == ERROR_INVALID_PARAMETER &&
        EnableIfUnclaimed(invalid, p32.wstring()) == ERROR_INVALID_PARAMETER && Disable(invalid) == ERROR_INVALID_PARAMETER && DisableFor(invalid, p32.wstring()) == ERROR_INVALID_PARAMETER && fixture::registry_calls == before_bad,
        "invalid architecture never aliases x86 or touches registry");
    fixture::processor = PROCESSOR_ARCHITECTURE_INTEL;
    Check(Inspect(Architecture::X64).error == ERROR_NOT_SUPPORTED && Enable(Architecture::X64, p64.wstring()) == ERROR_NOT_SUPPORTED &&
        Disable(Architecture::X64) == ERROR_NOT_SUPPORTED && fixture::registry_calls == before_bad,
        "native 32-bit OS rejects x64 instead of allowing an ignored WOW64 view flag");
    Check(Enable(Architecture::X86, p32.wstring()) == ERROR_SUCCESS, "native 32-bit OS policy preserves x86 support");
    fixture::processor = PROCESSOR_ARCHITECTURE_ARM64;
    const unsigned before_arm = fixture::registry_calls;
    Check(Inspect(Architecture::X86).error == ERROR_NOT_SUPPORTED && Inspect(Architecture::X64).error == ERROR_NOT_SUPPORTED && fixture::registry_calls == before_arm,
        "ARM64 policy is explicitly unsupported rather than extrapolated from pointer size");
    fixture::processor = PROCESSOR_ARCHITECTURE_AMD64;
    Check(fixture::mutex_name_ok && fixture::depth == 0 && fixture::max_depth >= 3, "all architecture mutations use the same balanced recursive global mutex");
    DeleteFileW(p64.c_str()); DeleteFileW(p32.c_str()); DeleteFileW(other.c_str());
    RemoveDirectoryW((root / L"other").c_str()); RemoveDirectoryW(root.c_str());
    std::printf("REGISTRATION_ARCH caller_bits=%zu checks=%u failures=%u real_registry_calls=0\n", sizeof(void*) * 8, checks, failures);
    return failures ? 1 : 0;
}
#endif

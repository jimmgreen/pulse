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
#include <functional>
#include <winioctl.h>

// Independent fault-injection regression backend. This backend models views explicitly. It makes no OS registry calls and does
// not pretend RegOverridePredefKey private paths are two redirected CLSID hives.
namespace fixture {
struct Value { DWORD type = 0; std::vector<BYTE> bytes; bool operator==(const Value&) const = default; };
struct Node { REGSAM view = 0; std::wstring path; std::map<std::wstring, Value> values; };
using Id = std::pair<REGSAM, std::wstring>;
std::map<Id, std::shared_ptr<Node>> nodes;
std::vector<std::shared_ptr<Node>> lifetime;
unsigned registry_calls = 0, mutation_calls = 0, depth = 0, max_depth = 0;
bool mutex_name_ok = true, writes_locked = true;
unsigned dll_writes = 0, fail_dll_write = 0, flush_calls = 0, fail_flush_call = 0;
bool fail_rollback_delete = false;
std::function<void()> on_write_failure;
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
    ++registry_calls;
    writes_locked &= depth > 0;
    if (Lower(name ? name : L"") == L"dllpath" && ++dll_writes == fail_dll_write) {
        if (on_write_failure) on_write_failure();
        return ERROR_ACCESS_DENIED;
    }
    As(key)->values[Lower(name ? name : L"")] = {type, std::vector<BYTE>(bytes, bytes + size)}; return ERROR_SUCCESS;
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
    ++registry_calls;
    writes_locked &= depth > 0;
    if (fail_rollback_delete && Lower(name ? name : L"") == L"dllpath") return ERROR_ACCESS_DENIED;
    return As(key)->values.erase(Lower(name ? name : L"")) ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND;
}
LSTATUS WINAPI DeleteKey(HKEY parent, LPCWSTR path, REGSAM flags, DWORD) {
    ++mutation_calls;
    ++registry_calls; const auto found = nodes.find(Key(parent, path, flags)); if (found == nodes.end()) return ERROR_FILE_NOT_FOUND;
    if (!found->second->values.empty() || Children(found->second.get())) return ERROR_ACCESS_DENIED;
    nodes.erase(found); return ERROR_SUCCESS;
}
LSTATUS WINAPI Flush(HKEY) { return ++flush_calls == fail_flush_call ? ERROR_WRITE_FAULT : ERROR_SUCCESS; }
HANDLE WINAPI CreateMutex(LPSECURITY_ATTRIBUTES, BOOL, LPCWSTR name) {
    mutex_name_ok &= name && std::wstring(name) == L"Global\\Pulse.DialogAuto.Registration.v1"; return mutex;
}
HANDLE WINAPI CreateMutexEx(LPSECURITY_ATTRIBUTES, LPCWSTR name, DWORD, DWORD) {
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
#define CreateMutexExW fixture::CreateMutexEx
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
#undef CreateMutexExW
#undef WaitForSingleObject
#undef ReleaseMutex
#undef CloseHandle
#undef GetNativeSystemInfo

namespace pulse::diagnostics::runtime {
void Event(const char*, std::initializer_list<Field>, Level) noexcept {}
void TextEvent(const char*, std::initializer_list<TextField>, std::initializer_list<Field>, Level) noexcept {}
}
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
namespace {
using pulse::dialog_auto::Architecture;
using pulse::dialog_auto::DllNameFor;
using pulse::dialog_auto::RepairMissingRegistrationJournals;
constexpr auto owner_name = L"PulseDialogAutoOwner";
constexpr auto owner_token = L"Pulse.DialogAuto.v1";
constexpr const wchar_t* clsids[] = {
    L"Software\\Classes\\CLSID\\{DC1C5A9C-E88A-4DDE-A5A1-60F82A20AEF7}",
    L"Software\\Classes\\CLSID\\{C0B4E2F3-BA21-4773-8DBA-335EC946EB8B}"};
const std::vector<Architecture> both{Architecture::X64, Architecture::X86};
REGSAM View(Architecture a) { return a == Architecture::X64 ? KEY_WOW64_64KEY : KEY_WOW64_32KEY; }
std::wstring Config(Architecture a) { return a == Architecture::X64 ? L"Software\\Pulse\\DialogAuto" : L"Software\\Pulse\\DialogAuto32"; }
fixture::Id Id(Architecture a, const std::wstring& path) { return {View(a), fixture::Lower(path)}; }
fixture::Node& Node(Architecture a, const std::wstring& path) {
    const auto id = Id(a, path);
    auto it = fixture::nodes.find(id);
    if (it == fixture::nodes.end()) {
        auto node = std::make_shared<fixture::Node>(); node->view = id.first; node->path = id.second;
        fixture::lifetime.push_back(node); it = fixture::nodes.emplace(id, node).first;
    }
    return *it->second;
}
void Text(fixture::Node& node, const wchar_t* name, const std::wstring& text, DWORD type = REG_SZ) {
    const auto* bytes = reinterpret_cast<const BYTE*>(text.c_str());
    node.values[fixture::Lower(name)] = {type, {bytes, bytes + (text.size() + 1) * sizeof(wchar_t)}};
}
void Number(fixture::Node& node, const wchar_t* name, DWORD number, DWORD type = REG_DWORD) {
    const auto* bytes = reinterpret_cast<const BYTE*>(&number);
    node.values[fixture::Lower(name)] = {type, {bytes, bytes + sizeof(number)}};
}
std::wstring ValueText(const fixture::Node& node, const wchar_t* name) {
    const auto it = node.values.find(fixture::Lower(name));
    if (it == node.values.end() || it->second.type != REG_SZ || it->second.bytes.size() < sizeof(wchar_t)) return {};
    std::wstring text(it->second.bytes.size() / sizeof(wchar_t), L'\0');
    std::memcpy(text.data(), it->second.bytes.data(), text.size() * sizeof(wchar_t));
    if (!text.empty() && !text.back()) text.pop_back();
    return text;
}
using Snapshot = std::map<fixture::Id, std::map<std::wstring, fixture::Value>>;
Snapshot Capture() {
    Snapshot result;
    for (const auto& [id, node] : fixture::nodes) result[id] = node->values;
    return result;
}
void Reset() {
    fixture::nodes.clear(); fixture::lifetime.clear(); fixture::registry_calls = fixture::mutation_calls = 0;
    fixture::dll_writes = fixture::fail_dll_write = fixture::flush_calls = fixture::fail_flush_call = 0; fixture::on_write_failure = {};
    fixture::fail_rollback_delete = false; fixture::writes_locked = true;
    fixture::processor = PROCESSOR_ARCHITECTURE_AMD64;
}
void Seed(Architecture a, const std::wstring& path) {
    auto& config = Node(a, Config(a)); Number(config, L"Enabled", 1); Number(config, L"Stage", 0);
    Number(config, L"Host", 0); Number(config, L"RemoteHost", 1); Text(config, L"PreservePreference", L"opaque");
    for (const auto* clsid : clsids) {
        Text(Node(a, clsid), owner_name, owner_token);
        auto& server = Node(a, std::wstring(clsid) + L"\\InprocServer32");
        Text(server, L"", path); Text(server, L"ThreadingModel", L"Apartment");
    }
}
void Paths(Architecture a, const std::wstring& path) {
    for (const auto* clsid : clsids) Text(Node(a, std::wstring(clsid) + L"\\InprocServer32"), L"", path);
}
bool Junction(const std::filesystem::path& link, const std::filesystem::path& target) {
    if (!CreateDirectoryW(link.c_str(), nullptr)) return false;
    HANDLE handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    struct JunctionData {
        DWORD tag; WORD size, reserved, substitute_offset, substitute_length, print_offset, print_length;
        wchar_t paths[2048];
    } data{};
    const std::wstring substitute = L"\\??\\" + target.wstring(), print = target.wstring();
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    data.substitute_length = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    data.print_offset = static_cast<WORD>(data.substitute_length + sizeof(wchar_t));
    data.print_length = static_cast<WORD>(print.size() * sizeof(wchar_t));
    data.size = static_cast<WORD>(8 + data.print_offset + data.print_length + sizeof(wchar_t));
    std::memcpy(data.paths, substitute.c_str(), data.substitute_length + sizeof(wchar_t));
    std::memcpy(reinterpret_cast<BYTE*>(data.paths) + data.print_offset, print.c_str(), data.print_length + sizeof(wchar_t));
    DWORD returned = 0;
    const bool ok = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &data, data.size + 8,
        nullptr, 0, &returned, nullptr) != FALSE;
    const auto error = GetLastError(); CloseHandle(handle);
    if (!ok) std::printf("JUNCTION_ERROR=%lu\n", error);
    return ok;
}
void RejectUnchanged(const char* label, const std::vector<Architecture>& architectures = both) {
    const auto before = Capture(); const auto mutations = fixture::mutation_calls;
    const auto result = RepairMissingRegistrationJournals(architectures);
    if (!result.error || result.rollback_error || result.repaired || Capture() != before || fixture::mutation_calls != mutations)
        std::printf("DETAIL error=%lu rollback=%lu repaired=%u mutation_delta=%u equal=%d\n", result.error, result.rollback_error,
            result.repaired, fixture::mutation_calls - mutations, Capture() == before);
    Check(result.error != ERROR_SUCCESS && !result.rollback_error && !result.repaired && Capture() == before && fixture::mutation_calls == mutations, label);
}
}
int main() {
    using namespace pulse::dialog_auto;
    const auto root = std::filesystem::absolute(L"bench_data") / (L"missing-journal-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!std::filesystem::create_directory(root)) return 2;
    const auto bridge = root / L"Pulse" / L"DialogBridge";
    std::filesystem::create_directories(bridge / L"x64"); std::filesystem::create_directory(bridge / L"x86");
    const auto p64 = bridge / L"x64" / DllNameFor(Architecture::X64);
    const auto p32 = bridge / L"x86" / DllNameFor(Architecture::X86);
    const auto seed_both = [&] { Reset(); Seed(Architecture::X64, p64.wstring()); Seed(Architecture::X86, p32.wstring()); };
    seed_both();
    Check(Inspect(Architecture::X64).conflict && Inspect(Architecture::X86).conflict, "missing DllPath reproduces original conflict on both architectures");
    auto expected = Capture();
    for (const auto a : both) {
        auto& node = Node(a, Config(a)); Text(node, L"DllPath", a == Architecture::X64 ? p64.wstring() : p32.wstring());
        expected[Id(a, Config(a))] = node.values; node.values.erase(L"dllpath");
    }
    auto result = RepairMissingRegistrationJournals(both);
    Check(!result.error && !result.rollback_error && result.repaired == 2 && Capture() == expected && fixture::mutation_calls == 2,
        "dual architecture repair adds exactly two DllPath values and preserves every other registry byte");
    Check(Inspect(Architecture::X64).enabled && !Inspect(Architecture::X64).conflict && Inspect(Architecture::X86).enabled && !Inspect(Architecture::X86).conflict,
        "repaired journals are readable by production Inspect");
    const auto repaired = Capture(); const auto mutations = fixture::mutation_calls;
    result = RepairMissingRegistrationJournals(both);
    Check(!result.error && !result.rollback_error && !result.repaired && Capture() == repaired && fixture::mutation_calls == mutations,
        "repeated repair is a zero-write no-op");
    Check(fixture::writes_locked && fixture::depth == 0 && fixture::mutex_name_ok, "all repair mutations hold the balanced shared global registration mutex");
    for (const auto a : both) {
        const auto path = a == Architecture::X64 ? p64 : p32;
        for (const int stage : {-1, 0, 1}) {
            Reset(); Seed(a, path.wstring());
            auto& config = Node(a, Config(a));
            if (stage < 0) config.values.erase(L"stage"); else Number(config, L"Stage", static_cast<DWORD>(stage));
            result = RepairMissingRegistrationJournals({a});
            Check(!result.error && !result.rollback_error && result.repaired == 1 && ValueText(config, L"DllPath") == path.wstring(),
                stage < 0 ? "missing Stage is accepted for requested architecture" : stage == 0 ? "Stage=0 is accepted for requested architecture" : "Stage=1 is accepted for requested architecture");
        }
        for (const auto& path_variant : std::vector<std::filesystem::path>{bridge / DllNameFor(a), bridge / std::wstring(64, L'a') / DllNameFor(a), bridge / StubDllNameFor(a)}) {
            Reset(); Seed(a, path_variant.wstring()); result = RepairMissingRegistrationJournals({a});
            Check(!result.error && result.repaired == 1, "flat, digest, and shared-stub historical layouts repair for each architecture");
        }
    }
    const std::vector<std::pair<const char*, std::function<void()>>> refusals{
        {"missing config refuses without mutation", [&] { fixture::nodes.erase(Id(Architecture::X86, Config(Architecture::X86))); }},
        {"missing Enabled refuses without mutation", [&] { Node(Architecture::X86, Config(Architecture::X86)).values.erase(L"enabled"); }},
        {"Enabled=0 refuses without mutation", [&] { Number(Node(Architecture::X86, Config(Architecture::X86)), L"Enabled", 0); }},
        {"Enabled=2 refuses without mutation", [&] { Number(Node(Architecture::X86, Config(Architecture::X86)), L"Enabled", 2); }},
        {"Enabled type mismatch refuses without mutation", [&] { Number(Node(Architecture::X86, Config(Architecture::X86)), L"Enabled", 1, REG_BINARY); }},
        {"Enabled malformed size refuses without mutation", [&] { Node(Architecture::X86, Config(Architecture::X86)).values[L"enabled"].bytes.pop_back(); }},
        {"Stage=2 refuses without mutation", [&] { Number(Node(Architecture::X86, Config(Architecture::X86)), L"Stage", 2); }},
        {"Stage type mismatch refuses without mutation", [&] { Number(Node(Architecture::X86, Config(Architecture::X86)), L"Stage", 0, REG_BINARY); }},
        {"existing empty DllPath is never interpreted as missing", [&] { Text(Node(Architecture::X86, Config(Architecture::X86)), L"DllPath", L""); }},
        {"DllPath wrong type is never interpreted as missing", [&] { Number(Node(Architecture::X86, Config(Architecture::X86)), L"DllPath", 0); }},
        {"missing owner refuses without mutation", [&] { Node(Architecture::X86, clsids[1]).values.erase(fixture::Lower(owner_name)); }},
        {"foreign owner refuses without mutation", [&] { Text(Node(Architecture::X86, clsids[1]), owner_name, L"Foreign"); }},
        {"owner wrong type refuses without mutation", [&] { Text(Node(Architecture::X86, clsids[1]), owner_name, owner_token, REG_EXPAND_SZ); }},
        {"one-sided CLSID remains untouched", [&] { fixture::nodes.erase(Id(Architecture::X86, clsids[1])); fixture::nodes.erase(Id(Architecture::X86, std::wstring(clsids[1]) + L"\\InprocServer32")); }},
        {"missing server subtree remains untouched", [&] { fixture::nodes.erase(Id(Architecture::X86, std::wstring(clsids[1]) + L"\\InprocServer32")); }},
        {"different COM paths refuse without mutation", [&] { Text(Node(Architecture::X86, std::wstring(clsids[1]) + L"\\InprocServer32"), L"", (bridge / DllNameFor(Architecture::X86)).wstring()); }},
        {"case-only differing COM paths refuse without mutation", [&] { auto path = p32.wstring(); path[0] = path[0] == L'C' ? L'c' : L'C'; Text(Node(Architecture::X86, std::wstring(clsids[1]) + L"\\InprocServer32"), L"", path); }},
        {"wrong architecture DLL refuses without mutation", [&] { Paths(Architecture::X86, p64.wstring()); }},
        {"wrong architecture directory refuses without mutation", [&] { Paths(Architecture::X86, (bridge / L"x64" / DllNameFor(Architecture::X86)).wstring()); }},
        {"unknown historical layout refuses without mutation", [&] { Paths(Architecture::X86, (root / DllNameFor(Architecture::X86)).wstring()); }},
        {"non-Apartment model refuses without mutation", [&] { Text(Node(Architecture::X86, std::wstring(clsids[1]) + L"\\InprocServer32"), L"ThreadingModel", L"Both"); }},
        {"missing threading model refuses without mutation", [&] { Node(Architecture::X86, std::wstring(clsids[1]) + L"\\InprocServer32").values.erase(L"threadingmodel"); }},
        {"COM path wrong type refuses without mutation", [&] { Text(Node(Architecture::X86, std::wstring(clsids[1]) + L"\\InprocServer32"), L"", p32.wstring(), REG_EXPAND_SZ); }},
        {"extra CLSID value refuses without mutation", [&] { Number(Node(Architecture::X86, clsids[1]), L"Foreign", 1); }},
        {"extra CLSID child refuses without mutation", [&] { Node(Architecture::X86, std::wstring(clsids[1]) + L"\\Foreign"); }},
        {"extra server value refuses without mutation", [&] { Number(Node(Architecture::X86, std::wstring(clsids[1]) + L"\\InprocServer32"), L"Foreign", 1); }},
        {"extra server child refuses without mutation", [&] { Node(Architecture::X86, std::wstring(clsids[1]) + L"\\InprocServer32\\Foreign"); }},
        {"UNC path refuses without mutation", [&] { Paths(Architecture::X86, L"\\\\localhost\\missing\\Pulse\\DialogBridge\\PulseDialogAuto32.dll"); }},
        {"device-prefixed path refuses without mutation", [&] { Paths(Architecture::X86, L"\\\\?\\" + p32.wstring()); }},
        {"missing ancestor cannot hide invalid suffix", [&] { Paths(Architecture::X86, (root / L"absent" / L".." / L"DialogBridge" / DllNameFor(Architecture::X86)).wstring()); }}
    };
    for (const auto& [label, change] : refusals) { seed_both(); change(); RejectUnchanged(label); }
    seed_both(); Pe(p32, IMAGE_FILE_MACHINE_I386, false); RejectUnchanged("live old DLL refuses repair of either architecture");
    Check(DeleteFileW(p32.c_str()) != FALSE, "live old DLL fixture removed");
    const auto junction = root / L"junction", target = root / L"junction-target";
    std::filesystem::create_directory(target);
    const bool junction_ok = Junction(junction, target);
    Check(junction_ok && (GetFileAttributesW(junction.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT), "real NTFS junction fixture exists");
    if (junction_ok) {
        seed_both(); Paths(Architecture::X86, (junction / L"Pulse" / L"DialogBridge" / DllNameFor(Architecture::X86)).wstring());
        RejectUnchanged("real reparse ancestor refuses even when old component is missing");
    }
    Check(RemoveDirectoryW(junction.c_str()) != FALSE && RemoveDirectoryW(target.c_str()) != FALSE, "junction link and empty target clean up independently");
    seed_both(); const auto before = Capture(); fixture::fail_dll_write = 2;
    result = RepairMissingRegistrationJournals(both);
    Check(result.error == ERROR_ACCESS_DENIED && !result.rollback_error && Capture() == before && fixture::dll_writes == 2,
        "second architecture write failure rolls back only the newly added first journal");
    Check(fixture::writes_locked && fixture::depth == 0, "rollback remains inside shared registration lock");
    seed_both(); fixture::fail_flush_call = 2; const auto before_flush = Capture();
    result = RepairMissingRegistrationJournals(both);
    Check(result.error == ERROR_WRITE_FAULT && !result.rollback_error && !result.repaired && Capture() == before_flush,
        "second architecture flush failure restores both original journals");
    seed_both(); fixture::fail_dll_write = 1; const auto before_first = Capture();
    result = RepairMissingRegistrationJournals(both);
    Check(result.error == ERROR_ACCESS_DENIED && !result.rollback_error && Capture() == before_first,
        "first write failure preserves all original values");
    seed_both(); fixture::fail_dll_write = 2; fixture::fail_rollback_delete = true;
    result = RepairMissingRegistrationJournals(both);
    Check(result.error == ERROR_ACCESS_DENIED && result.rollback_error == ERROR_ACCESS_DENIED && ValueText(Node(Architecture::X64, Config(Architecture::X64)), L"DllPath") == p64.wstring(),
        "rollback deletion failure is reported and does not erase surviving recovery evidence");
    seed_both(); fixture::fail_dll_write = 2;
    fixture::on_write_failure = [&] { Text(Node(Architecture::X64, Config(Architecture::X64)), L"DllPath", L"C:\\Foreign\\Component.dll"); };
    result = RepairMissingRegistrationJournals(both);
    Check(result.error == ERROR_ACCESS_DENIED && result.rollback_error && ValueText(Node(Architecture::X64, Config(Architecture::X64)), L"DllPath") == L"C:\\Foreign\\Component.dll",
        "rollback preserves externally replaced DllPath and reports ownership loss");
    seed_both(); fixture::fail_dll_write = 2;
    fixture::on_write_failure = [&] { Text(Node(Architecture::X64, clsids[0]), owner_name, L"Foreign"); };
    result = RepairMissingRegistrationJournals(both);
    Check(result.error == ERROR_ACCESS_DENIED && result.rollback_error && ValueText(Node(Architecture::X64, Config(Architecture::X64)), L"DllPath") == p64.wstring() && ValueText(Node(Architecture::X64, clsids[0]), owner_name) == L"Foreign",
        "rollback rechecks COM ownership and preserves foreign takeover");
    seed_both(); fixture::fail_dll_write = 2;
    fixture::on_write_failure = [&] { Number(Node(Architecture::X64, Config(Architecture::X64)), L"Stage", 1); };
    result = RepairMissingRegistrationJournals(both);
    Check(result.error == ERROR_ACCESS_DENIED && result.rollback_error && Node(Architecture::X64, Config(Architecture::X64)).values.contains(L"dllpath"),
        "rollback rechecks exact original journal metadata before deleting");
    Reset(); result = RepairMissingRegistrationJournals(both);
    Check(!result.error && !result.repaired && !fixture::mutation_calls && fixture::nodes.empty(), "clean unregistered architectures are a zero-write no-op");
    seed_both(); RejectUnchanged("invalid architecture is rejected before any write", {Architecture::X64, static_cast<Architecture>(99)});
    Reset(); fixture::processor = PROCESSOR_ARCHITECTURE_INTEL;
    RejectUnchanged("unsupported x64 request is rejected before any write", both);
    Check(fixture::depth == 0 && fixture::mutex_name_ok, "all rejection and fault paths release the correct global mutex");
    RemoveDirectoryW((bridge / L"x64").c_str()); RemoveDirectoryW((bridge / L"x86").c_str());
    RemoveDirectoryW(bridge.c_str()); RemoveDirectoryW((root / L"Pulse").c_str()); RemoveDirectoryW(root.c_str());
    std::printf("MISSING_JOURNAL caller_bits=%zu checks=%u failures=%u real_registry_calls=0 filesystem=real fault_injection=registry\n", sizeof(void*) * 8, checks, failures);
    return failures ? 1 : 0;
}

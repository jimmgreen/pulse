#include <windows.h>
#include <winioctl.h>
#include <filesystem>
#include <string>

namespace fixture { extern unsigned depth; }
namespace retired_fs {
std::wstring observed_old;
unsigned locked_reads = 0, unlocked_reads = 0;
DWORD WINAPI Attributes(LPCWSTR path) {
    if (!observed_old.empty() && path && observed_old == path) {
        if (fixture::depth) ++locked_reads;
        else ++unlocked_reads;
    }
    return ::GetFileAttributesW(path);
}
}

// All registry APIs are redirected by this backend, including native endpoint
// implementation calls. Filesystem validation still uses real isolated files.
#define GetFileAttributesW retired_fs::Attributes
#define PULSE_REGISTRATION_ARCH_FIXTURE_ONLY
#include "dialog_auto_registration_arch_test.cpp"
#include "../common/dialog_auto_ownership.h"
#undef GetFileAttributesW

namespace {
using pulse::dialog_auto::Architecture;
namespace fs = std::filesystem;
using Snapshot = std::map<fixture::Id, std::map<std::wstring, std::pair<DWORD, std::vector<BYTE>>>>;
Snapshot State() {
    Snapshot result;
    for (const auto& [id, node] : fixture::nodes) {
        auto& values = result[id];
        for (const auto& [name, value] : node->values) values[name] = {value.type, value.bytes};
    }
    return result;
}
REGSAM View(Architecture arch) { return arch == Architecture::X64 ? KEY_WOW64_64KEY : KEY_WOW64_32KEY; }
const wchar_t* Config(Architecture arch) { return arch == Architecture::X64 ? L"Software\\Pulse\\DialogAuto" : L"Software\\Pulse\\DialogAuto32"; }
fixture::Node& ConfigNode(Architecture arch) { return *fixture::nodes.at({View(arch), fixture::Lower(Config(arch))}); }
void Reset() {
    fixture::nodes.clear(); fixture::lifetime.clear(); fixture::processor = PROCESSOR_ARCHITECTURE_AMD64;
    retired_fs::observed_old.clear(); retired_fs::locked_reads = retired_fs::unlocked_reads = 0;
}
void Flag(Architecture arch, const wchar_t* name, DWORD value, DWORD type = REG_DWORD, DWORD size = sizeof(DWORD)) {
    std::vector<BYTE> bytes(size, 0);
    std::memcpy(bytes.data(), &value, (std::min)(size, static_cast<DWORD>(sizeof(value))));
    ConfigNode(arch).values[fixture::Lower(name)] = {type, std::move(bytes)};
}
void RecordedPath(Architecture arch, const std::wstring& path) {
    auto& value = ConfigNode(arch).values[L"dllpath"];
    value.type = REG_SZ;
    const auto* bytes = reinterpret_cast<const BYTE*>(path.c_str());
    value.bytes.assign(bytes, bytes + (path.size() + 1) * sizeof(wchar_t));
}
void Disabled(Architecture arch, const std::wstring& path) {
    Reset();
    HKEY key = nullptr;
    fixture::Create(HKEY_CURRENT_USER, Config(arch), 0, nullptr, 0, KEY_READ | KEY_WRITE | View(arch), nullptr, &key, nullptr);
    RecordedPath(arch, path);
    Flag(arch, L"Enabled", 0); Flag(arch, L"Stage", 0);
    Flag(arch, L"Host", 0); Flag(arch, L"RemoteHost", 1);
    ConfigNode(arch).values[L"binarypreference"] = {REG_BINARY, {0, 0xff, 0x80, 7}};
    const auto preferences = std::wstring(Config(arch)) + L"\\Preferences";
    fixture::Create(HKEY_CURRENT_USER, preferences.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE | View(arch), nullptr, &key, nullptr);
    const BYTE opaque[]{0, 0xf0, 0x7f};
    fixture::Set(key, L"Nested", 0, REG_BINARY, opaque, sizeof(opaque));
}
DWORD Rebind(Architecture arch, const std::wstring& old_path, const std::wstring& new_path) {
#if defined(PULSE_RETIRED_REBIND_BASELINE)
    (void)old_path;
    return pulse::dialog_auto::EnableIfUnclaimed(arch, new_path);
#else
    bool mutation_attempted = true;
    const DWORD result = pulse::dialog_auto::EnableIfRetiredDisabledFor(arch, old_path, new_path, &mutation_attempted);
    Check(mutation_attempted == (result == ERROR_SUCCESS),
        "native endpoint reports whether this fixture call entered the mutation phase");
    return result;
#endif
}
bool Candidate(Architecture arch, const std::wstring& path) {
    pulse::dialog_auto::Status status;
    status.dll_path = path;
    return pulse::dialog_auto::IsRetiredDisabledPulseDialogDll(arch, status);
}
template<class Call>
void RejectWithoutMutation(Call call, DWORD expected, const char* label) {
    const auto before = State();
    const unsigned mutations = fixture::mutation_calls;
    const DWORD result = call();
    std::printf("REJECT result=%lu expected=%lu mutation_delta=%u\n", result, expected, fixture::mutation_calls - mutations);
    Check(result == expected && State() == before && fixture::mutation_calls == mutations, label);
}
template<class Call>
void RejectInvalidWithoutMutation(Call call, const char* label) {
    const auto before = State();
    const unsigned mutations = fixture::mutation_calls;
    const DWORD result = call();
    std::printf("INVALID result=%lu mutation_delta=%u\n", result, fixture::mutation_calls - mutations);
    Check(result != ERROR_SUCCESS && State() == before && fixture::mutation_calls == mutations, label);
}
bool Junction(const fs::path& link, const fs::path& target) {
    if (!fs::create_directory(link)) return false;
    const std::wstring substitute = L"\\??\\" + target.wstring();
    const std::wstring printable = target.wstring();
    struct Header { DWORD tag; WORD length, reserved, sub_offset, sub_length, print_offset, print_length; };
    static_assert(sizeof(Header) == 16);
    const size_t string_bytes = (substitute.size() + printable.size() + 2) * sizeof(wchar_t);
    std::vector<BYTE> buffer(sizeof(Header) + string_bytes, 0);
    auto* header = reinterpret_cast<Header*>(buffer.data());
    header->tag = IO_REPARSE_TAG_MOUNT_POINT;
    header->length = static_cast<WORD>(8 + string_bytes);
    header->sub_length = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    header->print_offset = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t));
    header->print_length = static_cast<WORD>(printable.size() * sizeof(wchar_t));
    std::memcpy(buffer.data() + sizeof(Header), substitute.c_str(), (substitute.size() + 1) * sizeof(wchar_t));
    std::memcpy(buffer.data() + sizeof(Header) + header->print_offset, printable.c_str(), (printable.size() + 1) * sizeof(wchar_t));
    HANDLE handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    DWORD returned = 0;
    const BOOL result = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.data(), static_cast<DWORD>(buffer.size()),
        nullptr, 0, &returned, nullptr);
    CloseHandle(handle);
    return result != FALSE;
}
}

int main() {
    using namespace pulse::dialog_auto;
    const auto bench = fs::weakly_canonical(fs::absolute(L"bench_data"));
    const auto root = bench / (L"dialog-retired-rebind-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(bench);
    if (!fs::create_directory(root)) return 2;
    fs::create_directories(root / L"new");
    fs::create_directories(root / L"wrong");
    fs::create_directories(root / L"old-install" / L"DialogBridge");
    const auto junction = root / L"junction-install";
    const bool junction_ready = Junction(junction, root / L"old-install");
    Check(junction_ready, "real isolated junction fixture is created");
    const std::wstring digest(64, L'a');
    for (const auto arch : {Architecture::X64, Architecture::X86}) {
        const bool x64 = arch == Architecture::X64;
        const auto leaf = std::wstring(DllNameFor(arch));
        const auto stub = std::wstring(StubDllNameFor(arch));
        const auto bridge = root / L"old-install" / L"DialogBridge";
        const auto old_path = bridge / leaf;
        const auto new_path = root / L"new" / leaf;
        const auto wrong_path = root / L"wrong" / leaf;
        Pe(new_path, x64 ? IMAGE_FILE_MACHINE_AMD64 : IMAGE_FILE_MACHINE_I386, x64);
        Pe(wrong_path, x64 ? IMAGE_FILE_MACHINE_I386 : IMAGE_FILE_MACHINE_AMD64, !x64);
        std::printf("ARCH tested=%s caller_bits=%zu\n", x64 ? "x64" : "x86", sizeof(void*) * 8);
        const std::vector<fs::path> historical{
            old_path, bridge / (x64 ? L"x64" : L"x86") / leaf,
            bridge / digest / leaf, root / L"common" / L"Pulse" / L"DialogBridge" / stub,
            root / L"deleted-install" / L"DialogBridge" / digest / leaf};
        for (const auto& old : historical) {
            Disabled(arch, old.wstring());
            Check(Candidate(arch, old.wstring()), "missing exact historical layout is a retired disabled candidate");
            RejectWithoutMutation([&] { return EnableIfUnclaimed(arch, new_path.wstring()); }, ERROR_ALREADY_EXISTS,
                "unchanged strict entry still rejects the nonempty retired journal");
            const auto before = State();
            retired_fs::observed_old = old.wstring();
            const DWORD result = Rebind(arch, old.wstring(), new_path.wstring());
            retired_fs::observed_old.clear();
            std::printf("REBIND result=%lu old=%ls new=%ls\n", result, old.c_str(), new_path.c_str());
            Check(result == ERROR_SUCCESS && Inspect(arch).enabled && Inspect(arch).dll_path == new_path.wstring(),
                "native retired endpoint enables the replacement architecture DLL");
            if (old == old_path) Check(retired_fs::locked_reads != 0 && retired_fs::unlocked_reads == 0,
                "native endpoint checks old DLL absence while holding the mutation mutex");
            const auto after = State();
            const auto config_id = fixture::Id{View(arch), fixture::Lower(Config(arch))};
            for (const auto* name : {L"host", L"remotehost", L"binarypreference"}) {
                Check(after.at(config_id).at(name) == before.at(config_id).at(name), "rebind preserves opaque and host preferences exactly");
            }
            const auto preference_id = fixture::Id{View(arch), fixture::Lower(std::wstring(Config(arch)) + L"\\Preferences")};
            Check(after.at(preference_id) == before.at(preference_id), "rebind preserves nested preferences exactly");
            RejectWithoutMutation([&] { return Rebind(arch, old.wstring(), new_path.wstring()); }, ERROR_ALREADY_EXISTS,
                "repeated retired rebind is rejected without any registry mutation");
        }
        Disabled(arch, old_path.wstring());
        Check(Candidate(arch, old_path.wstring()), "retired candidate exists before an old file reappears");
        Pe(old_path, x64 ? IMAGE_FILE_MACHINE_AMD64 : IMAGE_FILE_MACHINE_I386, x64);
        Check(!Candidate(arch, old_path.wstring()), "existing old DLL is never retired");
        RejectWithoutMutation([&] { return Rebind(arch, old_path.wstring(), new_path.wstring()); }, ERROR_ALREADY_EXISTS,
            "native endpoint rejects a file which reappeared after candidate selection");
        Check(EnableIfDisabledFor(arch, old_path.wstring(), new_path.wstring()) == ERROR_SUCCESS,
            "existing same-owner disabled helper retains its previous existing-file behavior");
        fs::remove(old_path);

        const std::vector<std::wstring> invalid{
            (root / L"arbitrary" / leaf).wstring(),
            (bridge / (x64 ? L"x86" : L"x64") / leaf).wstring(),
            (bridge / (digest + L"a") / leaf).wstring(),
            (bridge / (L"g" + digest.substr(1)) / leaf).wstring(),
            (bridge / stub).wstring(),
            (bridge / L"x64" / stub).wstring(),
            (bridge / digest / stub).wstring(),
            (bridge / digest / L"extra" / leaf).wstring(),
            (bridge / (x64 ? L"PulseDialogAuto32.dll" : L"PulseDialogAuto64.dll")).wstring(),
            (bridge / leaf).wstring() + L".bak",
            (bridge / L"absent" / L".." / digest / leaf).wstring(),
            (root / L"absent." / L"DialogBridge" / digest / leaf).wstring(),
            (root / L"absent " / L"DialogBridge" / digest / leaf).wstring(),
            (root / L"absent?" / L"DialogBridge" / digest / leaf).wstring(),
            (root / L"missing" / L"CON.txt" / L"DialogBridge" / leaf).wstring(),
            (root / L"missing" / L"COM1" / L"DialogBridge" / leaf).wstring(),
            (root / L"missing" / L"LPT9" / L"DialogBridge" / leaf).wstring(),
            (root / L"missing" / L"COM\u00b9" / L"DialogBridge" / leaf).wstring(),
            (root / L"absent" / L"DialogBridge" / (digest + L" ") / leaf).wstring(),
            L"C:relative\\DialogBridge\\" + leaf,
            L"\\\\server\\share\\DialogBridge\\" + leaf,
            L"\\\\?\\C:\\DialogBridge\\" + leaf,
            (bridge / leaf).wstring() + L":stream",
            (root / L"absent").wstring() + L"\\\\DialogBridge\\" + leaf,
            (root / L"absent").wstring() + L"/DialogBridge/" + leaf,
            (root / L"absent").wstring() + std::wstring(1, L'\0') + L"\\DialogBridge\\" + leaf,
            (root / L"absent").wstring() + std::wstring(1, L'\x01') + L"\\DialogBridge\\" + leaf};
        for (const auto& path : invalid) {
            Disabled(arch, path);
            Check(!Candidate(arch, path), "invalid architecture, layout, path or suffix is not retired");
            RejectInvalidWithoutMutation([&] { return Rebind(arch, path, new_path.wstring()); },
                "native endpoint rejects invalid historical identity without mutation");
        }
        if (junction_ready) {
            const auto path = (junction / L"DialogBridge" / digest / leaf).wstring();
            Disabled(arch, path);
            Check(!Candidate(arch, path), "missing DLL beneath an existing reparse ancestor is not retired");
            RejectWithoutMutation([&] { return Rebind(arch, path, new_path.wstring()); }, ERROR_ALREADY_EXISTS,
                "native endpoint rejects a reparse ancestor without mutation");
        }
        for (const auto* flag : {L"Enabled", L"Stage"}) {
            Disabled(arch, old_path.wstring()); Flag(arch, flag, 1);
            RejectWithoutMutation([&] { return Rebind(arch, old_path.wstring(), new_path.wstring()); }, ERROR_ALREADY_EXISTS,
                "nonzero Enabled or Stage is rejected");
            Disabled(arch, old_path.wstring()); Flag(arch, flag, 0, REG_BINARY);
            RejectWithoutMutation([&] { return Rebind(arch, old_path.wstring(), new_path.wstring()); }, ERROR_INVALID_DATA,
                "wrong typed zero is not a disabled DWORD receipt");
            for (DWORD size : {0u, 2u, 8u}) {
                Disabled(arch, old_path.wstring()); Flag(arch, flag, 0, REG_DWORD, size);
                RejectWithoutMutation([&] { return Rebind(arch, old_path.wstring(), new_path.wstring()); }, ERROR_INVALID_DATA,
                    "wrong DWORD size is rejected without modification");
            }
            Disabled(arch, old_path.wstring()); ConfigNode(arch).values.erase(fixture::Lower(flag));
            RejectWithoutMutation([&] { return Rebind(arch, old_path.wstring(), new_path.wstring()); }, ERROR_FILE_NOT_FOUND,
                "missing Enabled or Stage is not inferred as zero");
        }
        for (const auto* id : {L"{DC1C5A9C-E88A-4DDE-A5A1-60F82A20AEF7}", L"{C0B4E2F3-BA21-4773-8DBA-335EC946EB8B}"}) {
            for (bool foreign_value : {false, true}) {
                Disabled(arch, old_path.wstring());
                HKEY key = nullptr;
                const auto path = std::wstring(L"Software\\Classes\\CLSID\\") + id;
                fixture::Create(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE | View(arch), nullptr, &key, nullptr);
                if (foreign_value) {
                    const BYTE foreign[]{1, 2, 3};
                    fixture::Set(key, L"Foreign", 0, REG_BINARY, foreign, sizeof(foreign));
                }
                RejectWithoutMutation([&] { return Rebind(arch, old_path.wstring(), new_path.wstring()); }, ERROR_ALREADY_EXISTS,
                    "empty or foreign CLSID root blocks native rebind");
            }
        }
        Disabled(arch, old_path.wstring());
        Check(Candidate(arch, old_path.wstring()), "candidate is established before old journal identity changes");
        RecordedPath(arch, (bridge / digest / leaf).wstring());
        RejectWithoutMutation([&] { return Rebind(arch, old_path.wstring(), new_path.wstring()); }, ERROR_ALREADY_EXISTS,
            "native endpoint rechecks changed old journal identity");
        Disabled(arch, old_path.wstring()); ConfigNode(arch).values[L"dllpath"].type = REG_EXPAND_SZ;
        RejectWithoutMutation([&] { return Rebind(arch, old_path.wstring(), new_path.wstring()); }, ERROR_INVALID_DATA,
            "old identity requires exact REG_SZ storage");
        Disabled(arch, old_path.wstring()); ConfigNode(arch).values[L"dllpath"].bytes.pop_back();
        RejectWithoutMutation([&] { return Rebind(arch, old_path.wstring(), new_path.wstring()); }, ERROR_INVALID_DATA,
            "malformed old registry path is rejected");
        Disabled(arch, old_path.wstring());
        RejectWithoutMutation([&] { return Rebind(arch, old_path.wstring(), wrong_path.wstring()); }, ERROR_BAD_EXE_FORMAT,
            "wrong replacement PE architecture is rejected");
        Reset();
        RejectWithoutMutation([&] { return Rebind(arch, old_path.wstring(), new_path.wstring()); }, ERROR_FILE_NOT_FOUND,
            "absent journal does not qualify as retired-disabled receipt");
        Status status; status.dll_path = old_path.wstring(); status.enabled = true;
        Check(!IsRetiredDisabledPulseDialogDll(arch, status), "enabled status is not a retired-disabled candidate");
        status.enabled = false; status.conflict = true;
        Check(!IsRetiredDisabledPulseDialogDll(arch, status), "conflicting status is not a retired-disabled candidate");
        status.conflict = false; status.error = ERROR_ACCESS_DENIED;
        Check(!IsRetiredDisabledPulseDialogDll(arch, status), "error status is not a retired-disabled candidate");
    }
    Check(fixture::mutex_name_ok && fixture::depth == 0 && fixture::max_depth >= 3,
        "native endpoint keeps the shared recursive registration mutex balanced");
    if (junction_ready) Check(RemoveDirectoryW(junction.c_str()) != FALSE, "isolated junction is removed without following its target");
    const auto resolved_root = fs::weakly_canonical(root);
    const bool own_root = resolved_root.parent_path() == bench &&
        resolved_root.filename().wstring().starts_with(L"dialog-retired-rebind-") &&
        !(GetFileAttributesW(root.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT);
    Check(own_root, "recursive cleanup target resolves to the uniquely owned bench_data child");
    if (own_root) fs::remove_all(root);
    Check(!fs::exists(root), "all isolated file fixtures are removed");
    std::printf("RETIRED_REBIND checks=%u failures=%u caller_bits=%zu real_registry_calls=0\n", checks, failures, sizeof(void*) * 8);
    return failures ? 1 : 0;
}

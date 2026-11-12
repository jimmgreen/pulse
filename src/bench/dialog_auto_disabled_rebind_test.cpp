// Reuse the existing explicit-view, all-registry-APIs-injected backend. Its old
// main is excluded; no real HKCU key or native registration helper is touched.
#define PULSE_REGISTRATION_ARCH_FIXTURE_ONLY
#include "dialog_auto_registration_arch_test.cpp"
#include <tuple>

namespace {
using pulse::dialog_auto::Architecture;
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
}
void Flag(Architecture arch, const wchar_t* name, DWORD value, DWORD type = REG_DWORD, DWORD size = sizeof(DWORD)) {
    std::vector<BYTE> bytes(size, 0);
    std::memcpy(bytes.data(), &value, (std::min)(size, static_cast<DWORD>(sizeof(value))));
    ConfigNode(arch).values[fixture::Lower(name)] = {type, std::move(bytes)};
}
DWORD Rebind(Architecture arch, const std::wstring& old_path, const std::wstring& new_path) {
#if defined(PULSE_DISABLED_REBIND_BASELINE)
    (void)old_path;
    return pulse::dialog_auto::EnableIfUnclaimed(arch, new_path);
#else
    return pulse::dialog_auto::EnableIfDisabledFor(arch, old_path, new_path);
#endif
}
void Disabled(Architecture arch, const std::filesystem::path& old_path) {
    using namespace pulse::dialog_auto;
    Reset();
    Check(Enable(arch, old_path.wstring()) == ERROR_SUCCESS && DisableFor(arch, old_path.wstring()) == ERROR_SUCCESS,
        "fixture establishes an old typed disabled journal with both CLSID roots absent");
    ConfigNode(arch).values[L"binarypreference"] = {REG_BINARY, {0, 0xff, 0x80, 7}};
    HKEY preferences = nullptr;
    const auto path = std::wstring(Config(arch)) + L"\\Preferences";
    fixture::Create(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE | View(arch), nullptr, &preferences, nullptr);
    const BYTE opaque[]{0, 0xf0, 0x7f};
    fixture::Set(preferences, L"Nested", 0, REG_BINARY, opaque, sizeof(opaque));
}
template<class Call>
void RejectWithoutMutation(Call call, DWORD expected, const char* label) {
    const auto before = State();
    const unsigned mutations = fixture::mutation_calls;
    const DWORD result = call();
    std::printf("REJECT result=%lu expected=%lu mutation_delta=%u\n", result, expected, fixture::mutation_calls - mutations);
    Check(result == expected && State() == before && fixture::mutation_calls == mutations, label);
}
}
int main() {
    namespace fs = std::filesystem;
    using namespace pulse::dialog_auto;
    const auto root = fs::absolute(L"disabled-rebind-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!fs::create_directory(root)) return 2;
    for (const auto* leaf : {L"old", L"new", L"other", L"wrong"}) fs::create_directory(root / leaf);
    for (const auto arch : {Architecture::X64, Architecture::X86}) {
        const bool x64 = arch == Architecture::X64;
        const auto old_path = root / L"old" / DllNameFor(arch), new_path = root / L"new" / DllNameFor(arch);
        const auto other_path = root / L"other" / DllNameFor(arch), wrong_path = root / L"wrong" / DllNameFor(arch);
        for (const auto& path : {old_path, new_path, other_path}) Pe(path, x64 ? IMAGE_FILE_MACHINE_AMD64 : IMAGE_FILE_MACHINE_I386, x64);
        Pe(wrong_path, x64 ? IMAGE_FILE_MACHINE_I386 : IMAGE_FILE_MACHINE_AMD64, !x64);
        std::printf("ARCH tested=%s caller_bits=%zu\n", x64 ? "x64" : "x86", sizeof(void*) * 8);
        Disabled(arch, old_path);
        RejectWithoutMutation([&] { return EnableIfUnclaimed(arch, new_path.wstring()); }, ERROR_ALREADY_EXISTS,
            "old strict endpoint still refuses nonempty old disabled journal");
        const auto preferences = ConfigNode(arch).values[L"binarypreference"].bytes;
        const DWORD rebound = Rebind(arch, old_path.wstring(), new_path.wstring());
        std::printf("REBIND result=%lu old=%ls new=%ls\n", rebound, old_path.c_str(), new_path.c_str());
        Check(rebound == ERROR_SUCCESS && Inspect(arch).enabled && Inspect(arch).dll_path == new_path,
            "expected-disabled rebind atomically enables the new architecture DLL");
        Check(ConfigNode(arch).values[L"binarypreference"].bytes == preferences &&
            fixture::nodes.contains({View(arch), fixture::Lower(std::wstring(Config(arch)) + L"\\Preferences")}),
            "rebind preserves opaque configuration values and nested preferences");
        Disabled(arch, old_path);
        RejectWithoutMutation([&] { return Rebind(arch, other_path.wstring(), new_path.wstring()); }, ERROR_ALREADY_EXISTS,
            "wrong expected old path leaves every typed byte and root unchanged");
        for (const auto* flag : {L"Enabled", L"Stage"}) {
            Disabled(arch, old_path); Flag(arch, flag, 1);
            RejectWithoutMutation([&] { return Rebind(arch, old_path.wstring(), new_path.wstring()); }, ERROR_ALREADY_EXISTS,
                "enabled or unfinished stage cannot be rebound");
            Disabled(arch, old_path); Flag(arch, flag, 0, REG_BINARY);
            RejectWithoutMutation([&] { return Rebind(arch, old_path.wstring(), new_path.wstring()); }, ERROR_INVALID_DATA,
                "zero bytes with wrong registry type are not a disabled DWORD receipt");
            Disabled(arch, old_path); Flag(arch, flag, 0, REG_DWORD, 8);
            RejectWithoutMutation([&] { return Rebind(arch, old_path.wstring(), new_path.wstring()); }, ERROR_INVALID_DATA,
                "oversized DWORD receipt fails without a mutation attempt");
            Disabled(arch, old_path); ConfigNode(arch).values.erase(fixture::Lower(flag));
            RejectWithoutMutation([&] { return Rebind(arch, old_path.wstring(), new_path.wstring()); }, ERROR_FILE_NOT_FOUND,
                "missing disabled receipt cannot be inferred as zero");
        }
        Disabled(arch, old_path); ConfigNode(arch).values[L"dllpath"].type = REG_EXPAND_SZ;
        RejectWithoutMutation([&] { return Rebind(arch, old_path.wstring(), new_path.wstring()); }, ERROR_INVALID_DATA,
            "old path must be exact well-formed REG_SZ, not expandable text");
        for (const auto* id : {L"{DC1C5A9C-E88A-4DDE-A5A1-60F82A20AEF7}", L"{C0B4E2F3-BA21-4773-8DBA-335EC946EB8B}"}) {
            Disabled(arch, old_path);
            HKEY foreign = nullptr;
            const auto path = std::wstring(L"Software\\Classes\\CLSID\\") + id;
            fixture::Create(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE | View(arch), nullptr, &foreign, nullptr);
            RejectWithoutMutation([&] { return Rebind(arch, old_path.wstring(), new_path.wstring()); }, ERROR_ALREADY_EXISTS,
                "even an empty uncleared CLSID root blocks rebind");
        }
        Disabled(arch, old_path);
        const auto reads = fixture::registry_calls;
        RejectWithoutMutation([&] { return Rebind(arch, old_path.wstring(), wrong_path.wstring()); }, ERROR_BAD_EXE_FORMAT,
            "wrong new PE architecture fails before any registry access");
        Check(fixture::registry_calls == reads, "PE mismatch does not inspect or create a journal");
        Reset();
        RejectWithoutMutation([&] { return Rebind(arch, old_path.wstring(), new_path.wstring()); }, ERROR_FILE_NOT_FOUND,
            "original absence is not an expected-disabled receipt");
        Check(EnableIfUnclaimed(arch, new_path.wstring()) == ERROR_SUCCESS && Inspect(arch).enabled,
            "original absence still uses the unchanged strict unclaimed entry");
        DeleteFileW(old_path.c_str()); DeleteFileW(new_path.c_str()); DeleteFileW(other_path.c_str()); DeleteFileW(wrong_path.c_str());
    }
    Check(fixture::mutex_name_ok && fixture::depth == 0 && fixture::max_depth >= 3,
        "all preconditions and enable share the same balanced recursive registration mutex");
    for (const auto* leaf : {L"old", L"new", L"other", L"wrong"}) RemoveDirectoryW((root / leaf).c_str());
    RemoveDirectoryW(root.c_str());
    std::printf("DISABLED_REBIND checks=%u failures=%u caller_bits=%zu real_registry_calls=0\n", checks, failures, sizeof(void*) * 8);
    return failures ? 1 : 0;
}

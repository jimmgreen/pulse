#include <windows.h>
#include <shlobj.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {
std::wstring fixture_dir;
HRESULT WINAPI FixtureFolder(HWND, int, HANDLE, DWORD, LPWSTR out) {
    return wcscpy_s(out, MAX_PATH, fixture_dir.c_str()) == 0 ? S_OK : E_FAIL;
}
}

// Keep cache writes isolated from the machine's ProgramData cache.
#define SHGetFolderPathW FixtureFolder
#include "../app/shell_verbs.cpp"
#undef SHGetFolderPathW

namespace {
int failures = 0;
void Check(bool ok, const char* name) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n';
    if (!ok) ++failures;
}

struct Fixture {
    HKEY registry = nullptr;
    std::wstring registry_path;
    bool directory_created = false;
    bool registry_created = false;
    bool Init() {
        const std::wstring suffix = std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64());
        std::error_code error;
        const auto root = std::filesystem::absolute(L"bench_data", error);
        if (error) {
            std::cerr << "[ERROR] resolve bench_data: " << error.value() << '\n';
            return false;
        }
        std::filesystem::create_directories(root, error);
        if (error) {
            std::cerr << "[ERROR] create bench_data: " << error.value() << '\n';
            return false;
        }
        fixture_dir = (root / (L"pulse-static-menu-icon-" + suffix)).wstring();
        if (!CreateDirectoryW(fixture_dir.c_str(), nullptr)) {
            std::wcerr << L"[ERROR] create fixture directory: " << GetLastError()
                       << L" path=" << fixture_dir << L'\n';
            return false;
        }
        directory_created = true;
        registry_path = L"Software\\PulseStaticMenuIconTest-" + suffix;
        DWORD disposition = 0;
        const LONG result = RegCreateKeyExW(HKEY_CURRENT_USER, registry_path.c_str(), 0,
            nullptr, REG_OPTION_VOLATILE, KEY_READ | KEY_WRITE, nullptr, &registry, &disposition);
        if (result != ERROR_SUCCESS) {
            std::wcerr << L"[ERROR] create fixture registry key: " << result
                       << L" path=HKCU\\" << registry_path << L'\n';
            return false;
        }
        registry_created = disposition == REG_CREATED_NEW_KEY;
        if (!registry_created) {
            std::cerr << "[ERROR] fixture registry key unexpectedly exists\n";
            return false;
        }
        return true;
    }
    ~Fixture() {
        if (registry) RegCloseKey(registry);
        if (registry_created) {
            const LONG result = RegDeleteTreeW(HKEY_CURRENT_USER, registry_path.c_str());
            if (result != ERROR_SUCCESS)
                std::cerr << "[ERROR] clean fixture registry key: " << result << '\n';
        }
        if (directory_created) {
            std::error_code error;
            std::filesystem::remove_all(fixture_dir, error);
            if (error) std::cerr << "[ERROR] clean fixture directory: " << error.value() << '\n';
        }
    }
    bool Put(const std::wstring& key_path, const wchar_t* name, const std::wstring& value,
             DWORD type = REG_SZ) const {
        HKEY key = nullptr;
        const LONG create_result = RegCreateKeyExW(registry, key_path.c_str(), 0, nullptr,
            REG_OPTION_VOLATILE, KEY_SET_VALUE, nullptr, &key, nullptr);
        if (create_result != ERROR_SUCCESS) {
            std::wcerr << L"[ERROR] create fixture verb key: " << create_result
                       << L" path=" << key_path << L'\n';
            return false;
        }
        const LONG result = RegSetValueExW(key, name, 0, type,
            reinterpret_cast<const BYTE*>(value.c_str()),
            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
        if (result != ERROR_SUCCESS)
            std::wcerr << L"[ERROR] write fixture registry value: " << result
                       << L" path=" << key_path << L'\n';
        return result == ERROR_SUCCESS;
    }
};

bool SameIcon(const pulse::MenuIcon& left, const pulse::MenuIcon& right) {
    return left.width == right.width && left.height == right.height && left.bgra == right.bgra;
}

std::vector<uint8_t> ReadBytes(const std::wstring& path) {
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void WriteBytes(const std::wstring& path, const std::vector<uint8_t>& bytes) {
    std::ofstream file(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}
}

int main() {
    using namespace pulse::app;
    Fixture fixture;
    if (!fixture.Init()) {
        Check(false, "create isolated fixtures");
        return 1;
    }

    const std::wstring icon_location = L"%SystemRoot%\\System32\\shell32.dll,0";
    Check(fixture.Put(L"shell\\fixture", nullptr, L"&Fixture action") &&
          fixture.Put(L"shell\\fixture\\command", nullptr, L"cmd.exe /c echo fixture") &&
          fixture.Put(L"shell\\fixture", L"Icon", icon_location, REG_EXPAND_SZ),
          "create registry verb with Icon");
    StaticVerb verb;
    Check(FillStaticVerbFromKey(fixture.registry, L"shell\\fixture", L"fixture", true,
              false, verb) && verb.icon.Valid() && verb.mnemonic == L'F' &&
          verb.display == L"Fixture action", "registry Icon and mnemonic load together");
    Check(fixture.Put(L"shell\\fixture", L"Icon", L"Z:\\missing-menu-icon-fixture.dll,0") &&
          FillStaticVerbFromKey(fixture.registry, L"shell\\fixture", L"fixture", true,
              false, verb) && !verb.icon.Valid(), "missing icon preserves usable verb");

    wchar_t system_dir[MAX_PATH]{};
    GetSystemDirectoryW(system_dir, MAX_PATH);
    const std::wstring app_path = std::wstring(system_dir) + L"\\cmd.exe";
    Check(fixture.Put(L"Applications\\pulse-menu-icon-test.exe\\shell\\open\\command",
              nullptr, L"\"" + app_path + L"\" \"%1\""), "create isolated OpenWith app");
    const LONG override_result = RegOverridePredefKey(HKEY_CLASSES_ROOT, fixture.registry);
    StaticVerb open_with;
    const bool open_with_ok = override_result == ERROR_SUCCESS &&
        ResolveOpenWithApp(L"pulse-menu-icon-test.exe", open_with);
    if (override_result == ERROR_SUCCESS) RegOverridePredefKey(HKEY_CLASSES_ROOT, nullptr);
    Check(open_with_ok && open_with.app_path == app_path && open_with.icon.Valid(),
          "OpenWith executable loads its icon");

    verb.verb = L"fixture";
    verb.display = L"Fixture action";
    verb.mnemonic = L'F';
    verb.icon = pulse::LoadMenuIcon(icon_location);
    StaticVerb child;
    child.verb = L"child";
    child.display = L"Child action";
    child.mnemonic = L'C';
    child.icon = open_with.icon;
    verb.children.push_back(child);
    StaticVerb plain;
    plain.verb = L"plain";
    plain.display = L"Plain action";
    std::unordered_map<std::wstring, std::vector<StaticVerb>> cache = {
        {L".fixture", {verb}}, {L".plain", {plain}}};
    Check(SaveMachineStaticVerbCache(cache), "save icon-bearing machine cache fixture");
    std::unordered_map<std::wstring, std::vector<StaticVerb>> loaded;
    const bool loaded_ok = LoadMachineStaticVerbCache(loaded);
    const auto entry = loaded.find(L".fixture");
    Check(loaded_ok && entry != loaded.end() && entry->second.size() == 1 &&
          entry->second[0].mnemonic == verb.mnemonic &&
          SameIcon(entry->second[0].icon, verb.icon) &&
          entry->second[0].children.size() == 1 &&
          entry->second[0].children[0].mnemonic == child.mnemonic &&
          SameIcon(entry->second[0].children[0].icon, child.icon),
          "cache hit retains parent and child mnemonics and icon pixels");
    const auto plain_entry = loaded.find(L".plain");
    Check(loaded_ok && plain_entry != loaded.end() && plain_entry->second.size() == 1 &&
          plain_entry->second[0].display == plain.display &&
          !plain_entry->second[0].icon.Valid(), "cache retains verbs without icons");

    const std::wstring cache_path = MachineStaticVerbCachePath();
    const auto valid_bytes = ReadBytes(cache_path);
    auto damaged = valid_bytes;
    if (damaged.size() >= 8) damaged[4] = 2;
    WriteBytes(cache_path, damaged);
    Check(!LoadMachineStaticVerbCache(loaded) && loaded.empty(), "v2 cache rejected for regeneration");
    damaged = valid_bytes;
    if (damaged.size() >= 8) damaged[4] = 5;
    WriteBytes(cache_path, damaged);
    Check(!LoadMachineStaticVerbCache(loaded) && loaded.empty(), "v5 cache rejected for regeneration");
    damaged = valid_bytes;
    if (!damaged.empty()) damaged.pop_back();
    WriteBytes(cache_path, damaged);
    Check(!LoadMachineStaticVerbCache(loaded) && loaded.empty(), "truncated icon cache rejected");
    damaged = valid_bytes;
    damaged.push_back(0);
    WriteBytes(cache_path, damaged);
    Check(!LoadMachineStaticVerbCache(loaded) && loaded.empty(), "trailing cache bytes rejected");

    std::vector<uint8_t> icon_bytes;
    PutU32(icon_bytes, 65);
    PutU32(icon_bytes, 1);
    PutU32(icon_bytes, 260);
    icon_bytes.resize(icon_bytes.size() + 260);
    const uint8_t* icon_read = icon_bytes.data();
    pulse::MenuIcon icon;
    Check(!GetIcon(icon_read, icon_bytes.data() + icon_bytes.size(), icon),
          "oversized icon dimensions rejected before allocation");
    icon_bytes.clear();
    PutU32(icon_bytes, 32);
    PutU32(icon_bytes, 32);
    PutU32(icon_bytes, 0xffffffff);
    icon_read = icon_bytes.data();
    Check(!GetIcon(icon_read, icon_bytes.data() + icon_bytes.size(), icon),
          "malformed icon length rejected before allocation");
    cache[L".fixture"][0].command.assign(4097, L'x');
    Check(!SaveMachineStaticVerbCache(cache), "oversized cache string rejected");
    std::vector<uint8_t> full(pulse::app::kMaxStaticVerbCacheBytes - 2);
    Check(!PutU32(full, 0) && full.size() == pulse::app::kMaxStaticVerbCacheBytes - 2,
          "cache output bound enforced before growth");
    return failures == 0 ? 0 : 1;
}

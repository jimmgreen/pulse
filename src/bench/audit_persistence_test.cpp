#include "../app/context_menu_prefs.h"
#include "../index/index_config.h"
#include "../common/utf8_file.h"
#include <filesystem>
#include <algorithm>
#include <cstdio>
#include <vector>
#include <windows.h>

namespace pulse::app {
static std::wstring fixture;
std::wstring GetPulseDataDir() { return fixture; }
}
static int failures = 0;
static void Check(bool ok, const char* name) {
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    failures += !ok;
}
int main() {
    using namespace pulse;
    const auto dir = std::filesystem::absolute(L"bench_data/audit-persistence-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(dir);
    app::fixture = dir.wstring();
    const auto file = app::fixture + L"\\context_menu.json";
    app::ContextMenuPrefs original;
    original.share = true;
    original.SetItemEnabled(L"h:{11111111-2222-3333-4444-555555555555}", false);
    Check(original.Save(), "AUD-014 save nondefault fixture");
    std::wstring before;
    ReadUtf8File(file, before);
    HANDLE lock = CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(lock != INVALID_HANDLE_VALUE, "AUD-014 exclusive fixture lock");
    app::ContextMenuPrefs loaded;
    Check(!loaded.Load() && loaded.load_failed, "AUD-014 locked read fails");
    if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
    Check(!loaded.Save(), "AUD-014 unlock cannot overwrite defaults");
    std::wstring after;
    ReadUtf8File(file, after);
    Check(before == after, "AUD-014 bytes retained");
    Check(loaded.Load() && !loaded.load_failed && loaded.share && loaded.Save(), "AUD-014 reread unlocks persistence");
    WriteUtf8FileAtomic(file, L"{\"share\":false,");
    Check(!loaded.Load() && loaded.share && !loaded.Save(), "AUD-014 truncated JSON retains live state and blocks save");
    HANDLE invalid = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    const unsigned char bytes[] = {0xff, 0xfe, 0xff};
    DWORD written = 0;
    Check(invalid != INVALID_HANDLE_VALUE && WriteFile(invalid, bytes, sizeof(bytes), &written, nullptr), "AUD-014 write invalid UTF8 fixture");
    if (invalid != INVALID_HANDLE_VALUE) CloseHandle(invalid);
    Check(!loaded.Load() && !loaded.Save(), "AUD-014 invalid UTF8 blocks save");
    DeleteFileW(file.c_str());
    Check(loaded.Load() && !loaded.load_failed && loaded.Save(), "AUD-014 missing file can initialize");

    const auto machine = app::fixture + L"\\index-config.json";
    const std::wstring valid = LR"({"index_path":"C:\\custom","excluded_paths":["C:\\private]data","D:\\secret"],"excluded_volume_ids":["volume-a"]})";
    WriteUtf8FileAtomic(machine, valid);
    index::IndexConfig config;
    Check(index::LoadIndexConfigFrom(machine, L"default", config) && config.excluded_paths.size() == 2 && config.index_path == L"C:\\custom", "AUD-015 load explicit isolated machine config");
    const auto paths = config.excluded_paths;
    const auto truncated = valid.substr(0, valid.find(L"secret") + 3);
    WriteUtf8FileAtomic(machine, truncated);
    Check(!index::LoadIndexConfigFrom(machine, L"default", config) && config.load_failed && config.excluded_paths == paths && config.index_path == L"C:\\custom", "AUD-015 truncated array retains previous config");
    Check(!index::SaveMachineConfig(config), "AUD-015 failed config cannot reach machine save");
    ReadUtf8File(machine, after);
    Check(after == truncated, "AUD-015 corrupt source remains unchanged");
    WriteUtf8FileAtomic(machine, valid);
    Check(index::LoadIndexConfigFrom(machine, L"default", config) && !config.load_failed, "AUD-015 successful reread clears failure");
    DeleteFileW(machine.c_str());
    Check(index::LoadIndexConfigFrom(machine, L"default", config) && config.index_path == L"default", "AUD-015 missing config initializes defaults");

    // System folder exclusions: configs from older releases get the defaults
    // and stay unmarked so the service persists them and rebuilds once.
    const std::vector<std::wstring> default_groups{L"windows", L"temp", L"old", L"node_modules"};
    Check(config.exclude_system && config.system_groups == default_groups && !config.system_groups_saved,
          "SYS-001 missing config uses default system groups");
    WriteUtf8FileAtomic(machine, valid);
    Check(index::LoadIndexConfigFrom(machine, L"default", config) && config.exclude_system &&
          config.system_groups == default_groups && !config.system_groups_saved,
          "SYS-002 config without system keys is unmarked with defaults");
    WriteUtf8FileAtomic(machine, LR"({"exclude_system":false,"system_exclusion_groups":["temp","bogus","temp","programdata"]})");
    Check(index::LoadIndexConfigFrom(machine, L"default", config) && !config.exclude_system &&
          config.system_groups == std::vector<std::wstring>{L"temp", L"programdata", L"node_modules"} &&
          config.system_groups_saved,
          "SYS-003 saved groups drop unknown and duplicate names; node_modules keeps its old behaviour");
    WriteUtf8FileAtomic(machine, LR"({"system_exclusion_groups":[]})");
    Check(index::LoadIndexConfigFrom(machine, L"default", config) &&
          config.system_groups == std::vector<std::wstring>{L"node_modules"} &&
          config.system_groups_saved, "SYS-004 legacy empty group list still hides node_modules");
    WriteUtf8FileAtomic(machine, LR"({"system_exclusion_groups":[],"system_exclusion_known":["windows","temp","old","programdata","node_modules"]})");
    Check(index::LoadIndexConfigFrom(machine, L"default", config) && config.system_groups.empty() &&
          config.system_groups_saved && !index::HidesNodeModules(config),
          "SYS-004 empty saved group list stays empty once node_modules was offered");
    WriteUtf8FileAtomic(machine, LR"({"system_exclusion_groups":["windows"],"system_exclusion_known":["windows","temp","old","programdata"]})");
    Check(index::LoadIndexConfigFrom(machine, L"default", config) &&
          config.system_groups == std::vector<std::wstring>{L"windows", L"node_modules"},
          "SYS-010 groups newer than the saved file start at their default");
    DeleteFileW(machine.c_str());
    {
        wchar_t windows[MAX_PATH]{};
        GetSystemWindowsDirectoryW(windows, MAX_PATH);
        auto has = [](const std::vector<std::wstring>& paths, const std::wstring& value) {
            return std::any_of(paths.begin(), paths.end(), [&](const std::wstring& p) {
                return CompareStringOrdinal(p.c_str(), -1, value.c_str(), -1, TRUE) == CSTR_EQUAL;
            });
        };
        index::IndexConfig defaults;
        const auto system_paths = index::SystemExclusionPaths(defaults);
        const std::wstring windir = windows;
        const std::wstring drive = windir.substr(0, 2);
        wchar_t local[MAX_PATH]{};
        const bool have_temp = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH) > 0;
        Check(has(system_paths, windir) && has(system_paths, windir + L"\\Temp") &&
              has(system_paths, drive + L"\\Windows.old") &&
              (!have_temp || has(system_paths, std::wstring(local) + L"\\Temp")),
              "SYS-005 defaults cover Windows, temp folders and old installs");
        wchar_t data[MAX_PATH]{};
        GetEnvironmentVariableW(L"ProgramData", data, MAX_PATH);
        Check(!has(system_paths, data), "SYS-006 ProgramData is kept by default");
        index::IndexConfig only_data;
        only_data.system_groups = {L"programdata"};
        const auto data_paths = index::SystemExclusionPaths(only_data);
        Check(data_paths.size() == 1 && has(data_paths, data), "SYS-007 ProgramData group adds only ProgramData");
        index::IndexConfig off;
        off.exclude_system = false;
        Check(index::SystemExclusionPaths(off).empty(), "SYS-008 switch off excludes nothing");
        Check(index::IsSystemExclusionGroup(L"old") && !index::IsSystemExclusionGroup(L"all") &&
              !index::IsSystemExclusionGroup(L"Windows"), "SYS-009 group names are exact");
        Check(index::IsSystemExclusionGroup(L"node_modules") && index::HidesNodeModules(defaults) &&
              !index::HidesNodeModules(off) && !index::HidesNodeModules(only_data),
              "SYS-011 node_modules group follows the master switch and its own checkbox");
        Check(std::none_of(system_paths.begin(), system_paths.end(), [](const std::wstring& p) {
                  return p.find(L"node_modules") != std::wstring::npos;
              }), "SYS-012 node_modules hides by folder name, not by fixed path");
    }
    std::filesystem::remove_all(dir);
    return failures ? 1 : 0;
}

#include "../app/settings_controller.h"
#include "../app/address_bar_command.h"
#include "../app/app_model.h"
#include "../app/session.h"
#include "../app/context_menu_controller.h"
#include "../app/single_instance_coordinator.h"
#include "../app/tray_controller.h"
#include "../app/blank_pane_click.h"
#include "../app/shell_registry_debounce.h"
#include "../app/default_file_manager.h"
#include "../app/shell_window_plan.h"
#include "../app/last_tab_close.h"
#include "../app/startup_launch.h"
#include "../app/unc_probe_scheduler.h"
#include "../common/localization.h"
#include "../common/path_utils.h"
#include "../ui/panel_metrics.h"
#include "../ui/FluentTokens.h"
#include "../index/index_client.h"
#include "../index/network_agent_client.h"
#include "../app/entry_sort.h"

#include <shlwapi.h>
#include <shlobj.h>
#include <cstdio>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <string>
#include <chrono>
#include <algorithm>
#include <vector>
#include <thread>

namespace pulse::app {

std::wstring GetPulseDataDir() {
    return {};
}
struct SettingsControllerTestPeer {
    static bool Start(SettingsController& controller, SettingsTask task,
                      SettingsTaskOperation operation, SettingsTaskCompletion completion) {
        return controller.StartTask(std::move(task), std::move(operation),
                                    std::move(completion));
    }
};

} // namespace pulse::app

namespace {

bool Report(const char* name, bool passed) {
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", name);
    return passed;
}

bool TestShellRegistryDebounce() {
    using pulse::app::ShellRegistryDebounce;
    bool passed = true;
    {
        ShellRegistryDebounce idle;
        passed &= Report("shell registry debounce: idle waits forever and never flushes",
            idle.WaitMs(0) == ShellRegistryDebounce::kIdle && !idle.TakeDue(100000) &&
            !idle.Pending());
    }
    {
        ShellRegistryDebounce single;
        single.Note(1000);
        const bool waits = single.WaitMs(1000) == ShellRegistryDebounce::kQuietMs;
        const bool early = !single.TakeDue(1000 + ShellRegistryDebounce::kQuietMs - 1);
        const bool due = single.TakeDue(1000 + ShellRegistryDebounce::kQuietMs);
        const bool once = !single.TakeDue(1000 + 10 * ShellRegistryDebounce::kQuietMs) &&
            single.WaitMs(50000) == ShellRegistryDebounce::kIdle;
        passed &= Report("shell registry debounce: one change flushes once after the quiet period",
            waits && early && due && once);
    }
    {
        // Mirrors the watch loop: Note() then TakeDue() on every change, plus a
        // TakeDue() on the wait timeout once the burst ends.
        ShellRegistryDebounce burst;
        constexpr uint64_t kStep = 20;  // 50 changes per second, 30 s long
        constexpr uint64_t kEnd = 30000;
        int flushes = 0;
        uint64_t previous = 0;
        bool spaced = true;
        for (uint64_t t = 0; t <= kEnd; t += kStep) {
            burst.Note(t);
            if (burst.TakeDue(t)) {
                if (flushes > 0 &&
                    (t - previous < ShellRegistryDebounce::kMaxDelayMs ||
                     t - previous > ShellRegistryDebounce::kMaxDelayMs + kStep))
                    spaced = false;
                previous = t;
                ++flushes;
            }
        }
        const int during = flushes;
        const uint32_t tail = burst.WaitMs(kEnd);
        const bool tail_flush = tail > 0 && tail <= ShellRegistryDebounce::kQuietMs &&
            burst.TakeDue(kEnd + tail);
        // 1500 changes collapse into flushes at 10.00 s and 20.02 s, plus the tail.
        passed &= Report("shell registry debounce: a 30 s burst flushes every 10 s, then once at the end",
            during == 2 && spaced && tail_flush && !burst.Pending());
    }
    return passed;
}

namespace takeover_test {

constexpr wchar_t kWinECommand[] =
    L"Software\\Classes\\CLSID\\{52205fd8-5dfb-447d-801a-d0b52f2e83e1}\\shell\\opennewwindow\\command";
constexpr wchar_t kThisPcClsid[] =
    L"Software\\Classes\\CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}";
constexpr wchar_t kThisPcShell[] =
    L"Software\\Classes\\CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\shell";
constexpr wchar_t kThisPcCommand[] =
    L"Software\\Classes\\CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\shell\\open\\command";
constexpr wchar_t kForeign[] = L"\"C:\\Other\\fm.exe\" \"%1\"";

std::wstring Read(const wchar_t* key, const wchar_t* name) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_QUERY_VALUE, &h) != ERROR_SUCCESS) return L"<absent>";
    wchar_t value[1024]{};
    DWORD bytes = sizeof(value) - sizeof(wchar_t);
    DWORD type = 0;
    const LONG st = RegQueryValueExW(h, name, nullptr, &type, reinterpret_cast<LPBYTE>(value), &bytes);
    RegCloseKey(h);
    return st == ERROR_SUCCESS ? std::wstring(value) : L"<none>";
}

bool Write(const wchar_t* key, const wchar_t* name, const std::wstring& value) {
    HKEY h = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS)
        return false;
    const LONG st = RegSetValueExW(h, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                                   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
    return st == ERROR_SUCCESS;
}

bool KeyExists(const wchar_t* key) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
    RegCloseKey(h);
    return true;
}

bool SnapshotExists(const wchar_t* group) {
    const std::wstring key = std::wstring(L"Software\\Pulse\\ShellIntegration\\Backups\\v1\\") + group;
    DWORD bytes = 0;
    return RegGetValueW(HKEY_CURRENT_USER, key.c_str(), L"Snapshot", RRF_RT_REG_BINARY,
        nullptr, nullptr, &bytes) == ERROR_SUCCESS && bytes != 0;
}

} // namespace takeover_test

bool TestDefaultFileManager() {
    using namespace pulse::app;
    using namespace takeover_test;
    bool passed = true;
    {
        passed &= Report("default file manager: This PC launch arguments are recognized",
            IsThisPcArgument(L"::{20d04fe0-3aea-1069-a2d8-08002b30309d}") &&
            IsThisPcArgument(L"\"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}\"") &&
            IsThisPcArgument(L"shell:::{20D04FE0-3AEA-1069-A2D8-08002B30309D}") &&
            IsThisPcArgument(L"shell:MyComputerFolder") &&
            IsThisPcArgument(L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\") &&
            !IsThisPcArgument(L"") && !IsThisPcArgument(L"C:\\") &&
            !IsThisPcArgument(L"::{645FF040-5081-101B-9F08-00AA002F954E}") &&
            !IsThisPcArgument(L"MyComputer") && !IsThisPcArgument(L"shell:"));
        AppPrefs flags;
        flags.persist = false;
        const bool off = DefaultFileManagerState(flags) == DefaultManagerState::Off;
        flags.take_over_win_e = true;
        const bool partial = DefaultFileManagerState(flags) == DefaultManagerState::Partial;
        flags.open_folders_in_pulse = flags.take_over_this_pc = true;
        passed &= Report("default file manager: state is off / partial / full from the three takeovers",
            off && partial && DefaultFileManagerState(flags) == DefaultManagerState::Full);
        flags.take_over_this_pc = false;
        flags.open_folders_in_pulse = false;
        const std::wstring summary = DefaultFileManagerSummary(flags);
        passed &= Report("default file manager: a partial takeover names what Explorer still opens",
            summary.find(pulse::l10n::Get(pulse::l10n::StringId::SettingsTakeoverFolders)) != std::wstring::npos &&
            summary.find(pulse::l10n::Get(pulse::l10n::StringId::ThisPc)) != std::wstring::npos &&
            summary.find(L"Win+E") == std::wstring::npos && summary.find(L"%s") == std::wstring::npos &&
            DefaultFileManagerSummary(AppPrefs{}) ==
                pulse::l10n::Get(pulse::l10n::StringId::SettingsDefaultManagerDesc));
        passed &= Report("default file manager: settings text is localized",
            pulse::l10n::Get(pulse::l10n::StringId::SettingsDefaultManager) ==
                L"\u8BBE\u4E3A\u9ED8\u8BA4\u6587\u4EF6\u7BA1\u7406\u5668" &&
            !pulse::l10n::Get(pulse::l10n::StringId::SettingsThisPc).empty() &&
            !pulse::l10n::Get(pulse::l10n::StringId::SettingsThisPcDesc).empty());
    }

    // Registry round trip, with HKCU redirected to a scratch key for this
    // process so the real associations are never touched.
    const std::wstring scratch_path =
        L"Software\\PulseTest\\DefaultFileManager-" + std::to_wstring(GetCurrentProcessId());
    HKEY scratch = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, scratch_path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS,
                        nullptr, &scratch, nullptr) != ERROR_SUCCESS ||
        RegOverridePredefKey(HKEY_CURRENT_USER, scratch) != ERROR_SUCCESS) {
        if (scratch) RegCloseKey(scratch);
        return Report("default file manager: scratch registry is available", false);
    }
    wchar_t module[MAX_PATH]{};
    GetModuleFileNameW(nullptr, module, MAX_PATH);
    const std::wstring exe = module;
    {
        // Older folder-only registrations may lack values now required for a
        // complete registration. Migration must retain only that selected scope.
        Write(L"Software\\Classes\\Directory\\shell\\open\\command", nullptr, FolderOpenCommandLine(exe));
        Write(L"Software\\Classes\\Directory\\shell", nullptr, L"open");
        AppPrefs legacy;
        legacy.Load();
        passed &= Report("default file manager: incomplete legacy folder scope stays selected",
            legacy.integration_enabled && legacy.integration_folders && !legacy.integration_win_e &&
            !legacy.integration_this_pc && legacy.integration_incomplete);
        RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\Directory");
    }
    {
        // Another file manager owns Win+E and This PC beforehand.
        Write(kWinECommand, nullptr, kForeign);
        Write(kThisPcCommand, nullptr, kForeign);
        Write(kThisPcShell, nullptr, L"openfm");
        AppPrefs prefs;
        prefs.Load();
        const bool starts_off = DefaultFileManagerState(prefs) == DefaultManagerState::Off;
        const bool applied = ApplyDefaultFileManager(prefs, true);
        AppPrefs reread;
        reread.Load();
        passed &= Report("default file manager: turning on takes over folders, Win+E and This PC",
            starts_off && applied && DefaultFileManagerState(reread) == DefaultManagerState::Full &&
            Read(L"Software\\Classes\\Directory\\shell", nullptr) == L"open" &&
            Read(kWinECommand, nullptr) == L"\"" + exe + L"\"" &&
            Read(kThisPcCommand, nullptr) == L"\"" + exe + L"\" \"" + kThisPcParsingName + L"\"" &&
            Read(kThisPcShell, nullptr) == L"open");
        passed &= Report("default file manager: the other manager's commands are kept as backups",
            SnapshotExists(L"WinE") && SnapshotExists(L"ThisPc"));

        ApplyThisPcOpen(reread, false);
        AppPrefs partial;
        partial.Load();
        const bool is_partial = DefaultFileManagerState(partial) == DefaultManagerState::Partial &&
            Read(kThisPcCommand, nullptr) == kForeign;
        // The switch treats partial as off and fills in the missing part.
        ApplyDefaultFileManager(partial, DefaultFileManagerState(partial) != DefaultManagerState::Full);
        AppPrefs filled;
        filled.Load();
        passed &= Report("default file manager: a partial takeover is completed by the switch",
            is_partial && DefaultFileManagerState(filled) == DefaultManagerState::Full);

        const bool removed = ApplyDefaultFileManager(filled, false);
        AppPrefs off;
        off.Load();
        passed &= Report("default file manager: turning off restores the other manager's commands",
            removed && DefaultFileManagerState(off) == DefaultManagerState::Off &&
            Read(kWinECommand, nullptr) == kForeign && !SnapshotExists(L"WinE") &&
            Read(kThisPcCommand, nullptr) == kForeign && !SnapshotExists(L"ThisPc") &&
            Read(kThisPcShell, nullptr) == L"openfm" &&
            Read(kThisPcCommand, L"DelegateExecute") == L"<none>" &&
            !KeyExists(L"Software\\Classes\\Directory\\shell\\open"));

        // Off again with nobody else's command: leaves it alone entirely.
        const bool untouched = ApplyThisPcOpen(off, false) && Read(kThisPcCommand, nullptr) == kForeign;
        passed &= Report("default file manager: turning off never removes another program's verb", untouched);
    }
    {
        // The other manager opens This PC through a DelegateExecute handler.
        RegDeleteTreeW(HKEY_CURRENT_USER, kThisPcClsid);
        constexpr wchar_t kForeignDelegate[] = L"{11111111-2222-3333-4444-555555555555}";
        Write(kThisPcCommand, nullptr, kForeign);
        Write(kThisPcCommand, L"DelegateExecute", kForeignDelegate);
        AppPrefs prefs;
        prefs.Load();
        const bool on = ApplyThisPcOpen(prefs, true) && Read(kThisPcCommand, L"DelegateExecute").empty() &&
            SnapshotExists(L"ThisPc");
        const bool off = ApplyThisPcOpen(prefs, false);
        passed &= Report("default file manager: turning off restores the other manager's DelegateExecute",
            on && off && Read(kThisPcCommand, nullptr) == kForeign &&
            Read(kThisPcCommand, L"DelegateExecute") == kForeignDelegate &&
            !SnapshotExists(L"ThisPc"));
    }
    {
        // Clean machine: on then off leaves no This PC / Win+E keys behind.
        RegDeleteTreeW(HKEY_CURRENT_USER, kThisPcClsid);
        RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\CLSID\\{52205fd8-5dfb-447d-801a-d0b52f2e83e1}");
        AppPrefs prefs;
        prefs.Load();
        const bool on = ApplyDefaultFileManager(prefs, true) && ReadThisPcOpen(exe);
        const bool off = ApplyDefaultFileManager(prefs, false) && !ReadThisPcOpen(exe);
        passed &= Report("default file manager: on and off on a clean profile leaves no keys behind",
            on && off && !KeyExists(kThisPcClsid) &&
            !KeyExists(L"Software\\Classes\\CLSID\\{52205fd8-5dfb-447d-801a-d0b52f2e83e1}\\shell"));
    }
    RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
    RegCloseKey(scratch);
    RegDeleteTreeW(HKEY_CURRENT_USER, scratch_path.c_str());
    SHDeleteEmptyKeyW(HKEY_CURRENT_USER, L"Software\\PulseTest");
    passed &= Report("default file manager: scratch registry is removed",
        !takeover_test::KeyExists(scratch_path.c_str()) &&
        !takeover_test::KeyExists(L"Software\\PulseTest"));
    return passed;
}

bool TestShellWindowPlan() {
    using namespace pulse::app;
    bool passed = true;
    passed &= Report("shell windows: drives, shares and This PC are shell folders; Pulse views are not",
        IsShellWindowPath(L"") && IsShellWindowPath(L"C:\\") && IsShellWindowPath(L"d:\\Work") &&
        IsShellWindowPath(L"\\\\server\\share\\dir") && !IsShellWindowPath(L"pulse:search?q=a") &&
        !IsShellWindowPath(L"pulse:settings") && !IsShellWindowPath(L"\\\\?\\C:\\x"));

    using K = ShellWindowActionKind;
    const std::vector<ShellWindowEntry> current = {{1, L"C:\\A"}, {2, L"C:\\B"}, {3, L""}};
    const std::vector<ShellWindowEntry> wanted = {{2, L"c:\\b"}, {3, L"D:\\"}, {4, L"C:\\New"}};
    const std::vector<ShellWindowAction> expected = {
        {K::Revoke, 1, L"C:\\A"}, {K::Navigate, 3, L"D:\\"}, {K::Register, 4, L"C:\\New"}};
    passed &= Report("shell windows: closed panes revoke first, case-only changes stay, moves navigate",
        PlanShellWindowChanges(current, wanted) == expected);
    passed &= Report("shell windows: same set plans nothing; empty set revokes all",
        PlanShellWindowChanges(current, current).empty() &&
        PlanShellWindowChanges(current, {}).size() == 3 &&
        PlanShellWindowChanges({}, current).size() == 3 &&
        PlanShellWindowChanges({}, current)[0].kind == K::Register);

    std::wstring folder, leaf;
    const bool file = SplitShellItemPath(L"C:\\Users\\a\\b.txt", folder, leaf) &&
        folder == L"C:\\Users\\a" && leaf == L"b.txt";
    const bool top = SplitShellItemPath(L"C:\\Users", folder, leaf) && folder == L"C:\\" && leaf == L"Users";
    const bool share = SplitShellItemPath(L"\\\\srv\\share\\x.doc", folder, leaf) &&
        folder == L"\\\\srv\\share" && leaf == L"x.doc";
    const bool trailing = SplitShellItemPath(L"D:\\dir\\sub\\", folder, leaf) && folder == L"D:\\dir" &&
        leaf == L"sub";
    const bool roots = !SplitShellItemPath(L"C:\\", folder, leaf) && folder.empty() && leaf.empty() &&
        !SplitShellItemPath(L"\\\\srv\\share", folder, leaf) && !SplitShellItemPath(L"", folder, leaf);
    passed &= Report("shell windows: selected items split into folder and name; roots have no parent",
        file && top && share && trailing && roots);

    using S = ExplorerTakeoverStep;
    auto step = [](unsigned age, bool ready, bool supported, size_t selected) {
        ExplorerWindowProbe probe;
        probe.age_ms = age;
        probe.view_ready = ready;
        probe.supported = supported;
        probe.selected = selected;
        return DecideExplorerTakeover(probe);
    };
    passed &= Report("explorer takeover: waits for the view, then gives up on it",
        step(0, false, false, 0) == S::Wait && step(kExplorerViewTimeoutMs - 1, false, false, 0) == S::Wait &&
        step(kExplorerViewTimeoutMs, false, false, 0) == S::Leave);
    passed &= Report("explorer takeover: virtual locations are left to File Explorer",
        step(50, true, false, 0) == S::Leave && step(50, true, false, 2) == S::Leave);
    passed &= Report("explorer takeover: a selection is taken at once, a plain folder after the grace",
        step(50, true, true, 1) == S::Take && step(50, true, true, 0) == S::Wait &&
        step(kExplorerSelectionGraceMs, true, true, 0) == S::Take);
    passed &= Report("explorer takeover: setting has a label and description",
        !pulse::l10n::Get(pulse::l10n::StringId::SettingsExplorerWindows).empty() &&
        !pulse::l10n::Get(pulse::l10n::StringId::SettingsExplorerWindowsDesc).empty());
    return passed;
}

bool TestAddressBarCommands() {
    using pulse::app::AddressBarProgram;
    using pulse::app::ParseAddressBarCommand;
    auto is = [](const wchar_t* text, AddressBarProgram program, const wchar_t* args) {
        const auto command = ParseAddressBarCommand(text);
        return command.program == program && command.args == args;
    };
    bool passed = true;
    passed &= Report("address bar: cmd / powershell / pwsh / wt are recognized",
        is(L"cmd", AddressBarProgram::Cmd, L"") &&
        is(L"powershell", AddressBarProgram::PowerShell, L"") &&
        is(L"pwsh", AddressBarProgram::Pwsh, L"") &&
        is(L"wt", AddressBarProgram::WindowsTerminal, L""));
    passed &= Report("address bar: case, .exe suffix and surrounding spaces are ignored",
        is(L"  CMD.exe ", AddressBarProgram::Cmd, L"") &&
        is(L"PowerShell.EXE", AddressBarProgram::PowerShell, L"") &&
        is(L"\x3000WT\x3000", AddressBarProgram::WindowsTerminal, L""));
    passed &= Report("address bar: arguments after the program are kept",
        is(L"cmd /k dir", AddressBarProgram::Cmd, L"/k dir") &&
        is(L"wt   -p Ubuntu  ", AddressBarProgram::WindowsTerminal, L"-p Ubuntu"));
    passed &= Report("address bar: paths and other words still navigate",
        is(L"", AddressBarProgram::None, L"") &&
        is(L"C:\\cmd", AddressBarProgram::None, L"") &&
        is(L"cmd\\sub", AddressBarProgram::None, L"") &&
        is(L"cmdx", AddressBarProgram::None, L"") &&
        is(L"wtf", AddressBarProgram::None, L"") &&
        is(L".exe", AddressBarProgram::None, L"") &&
        is(L"\"cmd\"", AddressBarProgram::None, L"") &&
        is(L"D:\\Projects\\wt", AddressBarProgram::None, L""));
    passed &= Report("address bar: program executables",
        std::wstring(pulse::app::AddressBarProgramExe(AddressBarProgram::Cmd)) == L"cmd.exe" &&
        std::wstring(pulse::app::AddressBarProgramExe(AddressBarProgram::Pwsh)) == L"pwsh.exe" &&
        std::wstring(pulse::app::AddressBarProgramExe(AddressBarProgram::WindowsTerminal)) == L"wt.exe" &&
        pulse::app::AddressBarProgramExe(AddressBarProgram::None) == nullptr);
    return passed;
}

// #54: %VAR% and shell: shortcuts typed in the address bar.
bool TestAddressShortcuts() {
    using pulse::app::ResolveAddressShortcut;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    auto env = [](const wchar_t* name) {
        wchar_t buf[32768]{};
        const DWORD n = GetEnvironmentVariableW(name, buf, ARRAYSIZE(buf));
        return n > 0 && n < ARRAYSIZE(buf) ? std::wstring(buf, n) : std::wstring();
    };
    auto known = [](REFKNOWNFOLDERID id) {
        PWSTR raw = nullptr;
        std::wstring out;
        if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &raw)) && raw) out = raw;
        CoTaskMemFree(raw);
        return out;
    };
    auto is = [](const wchar_t* text, const std::wstring& path) {
        const auto r = ResolveAddressShortcut(text);
        return r.resolved && r.path == path;
    };
    auto plain = [](const wchar_t* text) { return !ResolveAddressShortcut(text).resolved; };

    // An isolated variable, so the check never depends on the machine's own.
    SetEnvironmentVariableW(L"PULSE_TEST_ALIAS", L"C:\\PulseAlias");
    const std::wstring temp = env(L"TEMP");
    const std::wstring startup = known(FOLDERID_Startup);
    const std::wstring sendto = known(FOLDERID_SendTo);
    bool passed = true;
    passed &= Report("address shortcut: %VAR% expands, case-insensitively, with a subpath",
        !temp.empty() && is(L"%temp%", temp) && is(L"%TEMP%", temp) &&
        is(L"%Temp%\\sub", temp + L"\\sub") &&
        is(L"%PULSE_TEST_ALIAS%\\docs", L"C:\\PulseAlias\\docs") &&
        is(L"  \"%PULSE_TEST_ALIAS%\"  ", L"C:\\PulseAlias"));
    passed &= Report("address shortcut: shell: folders resolve to their filesystem path",
        !startup.empty() && is(L"shell:startup", startup) && is(L"Shell:Startup", startup) &&
        is(L"shell:startup\\", startup) && is(L"shell:startup\\Sub", startup + L"\\Sub") &&
        is(L"shell:startup/Sub/", startup + L"\\Sub") &&
        !sendto.empty() && is(L"shell:sendto", sendto));
    passed &= Report("address shortcut: This PC and the recycle bin open Pulse's own views",
        is(L"shell:MyComputerFolder", L"") &&
        is(L"shell:::{20D04FE0-3AEA-1069-A2D8-08002B30309D}", L"") &&
        is(L"shell:RecycleBinFolder", L"pulse:recycle") &&
        is(L"shell:::{645FF040-5081-101B-9F08-00AA002F954E}", L"pulse:recycle"));
    passed &= Report("address shortcut: plain paths, unknown names and stray % are left alone",
        plain(L"C:\\Windows") && plain(L"\\\\server\\share") && plain(L"") &&
        plain(L"%PULSE_NO_SUCH_VARIABLE%") && plain(L"100%") &&
        plain(L"shell:") && plain(L"shell:PulseNoSuchFolder") &&
        ResolveAddressShortcut(L" C:\\Windows ").path == L"C:\\Windows");
    SetEnvironmentVariableW(L"PULSE_TEST_ALIAS", nullptr);
    if (SUCCEEDED(com)) CoUninitialize();
    return passed;
}

// #78: a selected row must stand out from its pane far more than a hovered one.
bool TestSelectionTokens() {
    using namespace pulse::ui;
    auto luma = [](D2D1_COLOR_F c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; };
    auto lift = [&](const Theme& t, D2D1_COLOR_F fill) {
        return std::fabs(luma(BlendOver(fill, t.surface_card)) - luma(t.surface_card));
    };
    bool stands_out = true, fades = true, outline = true;
    for (bool dark : {true, false}) {
        // The accent from the #78 screenshot, Windows blue, purple, green, magenta.
        for (uint32_t rgb : {0x4466A8u, 0x0078D4u, 0x8764B8u, 0x2F7D5Bu, 0xC239B3u}) {
            const Theme t = MakeTheme(dark, HexColor(rgb));
            stands_out &= lift(t, t.list_selected_start) > 1.5f * lift(t, t.fill_hover);
            fades &= t.list_selected_end.a > 0.0f && t.list_selected_end.a < t.list_selected_start.a;
            outline &= t.list_selected_outline.a == 1.0f &&
                lift(t, t.list_selected_outline) > lift(t, t.list_selected_start);
        }
    }
    bool passed = true;
    passed &= Report("selection: the gradient start stands out well beyond hover in both themes",
        stands_out);
    passed &= Report("selection: the gradient fades toward the end but never disappears", fades);
    passed &= Report("selection: the outline is opaque and stronger than the fill", outline);
    return passed;
}

// A tab handed to another Pulse window arrives as its own message, and it is the
// receiving window that decides to open a tab of its own even when it already
// shows that folder. The sink records what the sender put on the wire.
struct TabTransferSink {
    std::wstring path;
    pulse::app::SingleInstanceCoordinator::TabTransfer transfer;
    ULONG_PTR message_id = 0;
    bool decoded = false;
};

LRESULT CALLBACK TabTransferSinkProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_COPYDATA) {
        auto* sink = reinterpret_cast<TabTransferSink*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lparam);
        std::wstring path;
        if (sink) sink->message_id = data->dwData;
        if (sink &&
            pulse::app::SingleInstanceCoordinator::DecodeTabTransfer(data, sink->transfer)) {
            sink->decoded = true;
            sink->path = sink->transfer.path;
            return TRUE;
        }
        if (sink && pulse::app::SingleInstanceCoordinator::DecodeTabTransfer(data, path)) {
            sink->decoded = true;
            sink->path = path;
            return TRUE;
        }
        return FALSE;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

// This PC: the view mode is remembered on its own and drives keep letter order.
bool TestThisPc() {
    using pulse::ui::SortColumn;
    using pulse::ui::SortDirection;
    using pulse::ui::ViewMode;
    bool passed = true;
    pulse::app::AppPrefs prefs;
    prefs.persist = false;
    passed &= Report("this pc: no saved view until one is chosen",
        !prefs.folder_views.Find(L""));
    const bool saved = prefs.folder_views.Set(L"", ViewMode::Tiles);
    passed &= Report("this pc: tiles are kept apart from folders and virtual views",
        saved && prefs.folder_views.Find(L"") == ViewMode::Tiles &&
        !prefs.folder_views.Find(L"C:\\") && !prefs.folder_views.Find(L"pulse:recent") &&
        !prefs.folder_views.Set(L"", ViewMode::Tiles));
    prefs.folder_views.ApplyToAll(ViewMode::List);
    passed &= Report("this pc: apply to all folders leaves This PC alone",
        prefs.folder_views.Find(L"") == ViewMode::Tiles &&
        prefs.folder_views.Default() == ViewMode::List);
    pulse::app::AppPrefs reloaded;
    reloaded.persist = false;
    reloaded.FromJson(prefs.ToJson());
    passed &= Report("this pc: view choice survives a restart",
        reloaded.folder_views.Find(L"") == ViewMode::Tiles);
    reloaded.FromJson(L"{\"folder_view_this_pc\":-1}");
    passed &= Report("this pc: an unset choice stays unset", !reloaded.folder_views.Find(L""));

    // Labels from the reporting machine; sorted as text they read C, G, F, D, E.
    const wchar_t* labels[] = { L"Win11", L"新加卷", L"资料安装盘", L"项目盘", L"软件池" };
    std::vector<pulse::fs::DirEntry> drives;
    for (int i = 4; i >= 0; --i) {
        const wchar_t letter = static_cast<wchar_t>(L'C' + i);
        pulse::fs::DirEntry e;
        e.name = std::wstring(labels[i]) + L" (" + letter + L":)";
        e.full_path = std::wstring(L"\\\\?\\") + letter + L":\\";
        e.is_dir = true;
        e.attrs = FILE_ATTRIBUTE_DIRECTORY;
        e.drive_type = DRIVE_FIXED;
        drives.push_back(std::move(e));
    }
    drives[1].drive_type = DRIVE_REMOVABLE;  // F: is a different drive kind
    const auto order = [](std::vector<pulse::fs::DirEntry> entries, SortColumn column,
                          SortDirection direction) {
        std::sort(entries.begin(), entries.end(), [&](const auto& a, const auto& b) {
            return pulse::app::EntryLess(a, b, column, direction,
                                         pulse::app::FolderSortMode::FoldersFirst);
        });
        std::wstring letters;
        for (const auto& e : entries) letters += e.full_path[4];
        return letters;
    };
    passed &= Report("this pc: name order follows drive letters, not volume labels",
        order(drives, SortColumn::Name, SortDirection::Asc) == L"CDEFG");
    passed &= Report("this pc: descending name order reverses the letters",
        order(drives, SortColumn::Name, SortDirection::Desc) == L"GFEDC");
    passed &= Report("this pc: equal sizes and dates fall back to letter order",
        order(drives, SortColumn::Size, SortDirection::Asc) == L"CDEFG" &&
        order(drives, SortColumn::Mtime, SortDirection::Asc) == L"CDEFG");
    passed &= Report("this pc: type order groups drive kinds, letters within",
        order(drives, SortColumn::Type, SortDirection::Asc) == L"FCDEG");
    std::vector<pulse::fs::DirEntry> plain(2);
    plain[0].name = L"Zeta (C:)";
    plain[1].name = L"Alpha (D:)";
    passed &= Report("this pc: ordinary entries still sort by name",
        pulse::app::EntryLess(plain[1], plain[0], SortColumn::Name, SortDirection::Asc,
                              pulse::app::FolderSortMode::FoldersFirst));
    return passed;
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring(argv[1]) == L"--default-manager") {
        pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
        return TestDefaultFileManager() ? 0 : 1;
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--integration-settings") {
        using namespace pulse::app;
        AppPrefs prefs;
        prefs.persist = false;
        ContextMenuPrefs context;
        context.persist = false;
        pulse::index::IndexClient index;
        pulse::index::NetworkAgentClient network;
        SettingsController settings;
        settings.BindUi(prefs, context, index, network, {});
        settings.IntegrationAction(2);
        bool ok = Report("off master saves scope without changing associations",
            !prefs.integration_win_e && !prefs.take_over_win_e && !prefs.open_folders_in_pulse);
        settings.IntegrationAction(4);
        ok &= Report("experimental choice alone does not enable master",
            prefs.take_over_explorer_windows && !prefs.integration_enabled && settings.IntegrationState() == 0);
        settings.IntegrationAction(0);
        ok &= Report("master applies exactly selected scopes",
            prefs.open_folders_in_pulse && !prefs.take_over_win_e && prefs.take_over_this_pc &&
            settings.IntegrationState() == 1);
        prefs.take_over_win_e = true;
        ok &= Report("external state mismatch is shown as partial", settings.IntegrationState() == 2);
        settings.IntegrationAction(5);
        ok &= Report("retry reconciles selected scopes", !prefs.take_over_win_e && settings.IntegrationState() == 1);
        prefs.integration_incomplete = true;
        ok &= Report("incomplete owned association remains visible even outside selected scope",
            settings.IntegrationState() == 2 && settings.IntegrationCanRestore());
        prefs.integration_incomplete = false;
        settings.IntegrationAction(6);
        ok &= Report("restore disables all mechanisms while preserving scope choices",
            !prefs.integration_enabled && !prefs.open_folders_in_pulse && !prefs.take_over_win_e &&
            !prefs.take_over_this_pc && prefs.integration_folders && !prefs.integration_win_e &&
            prefs.take_over_explorer_windows && settings.IntegrationState() == 0);
        AppPrefs loaded;
        loaded.FromJson(prefs.ToJson());
        loaded.MigrateIntegration();
        ok &= Report("disabled master and retained choices round trip",
            loaded.integration_configured && !loaded.integration_enabled && loaded.integration_folders &&
            !loaded.integration_win_e && loaded.take_over_explorer_windows);
        loaded.FromJson(L"{}");
        loaded.take_over_win_e = true;
        loaded.MigrateIntegration();
        ok &= Report("legacy partial scope is migrated without adding scopes",
            loaded.integration_enabled && loaded.integration_win_e && !loaded.integration_folders &&
            !loaded.integration_this_pc);
        AppPrefs experimental;
        experimental.FromJson(L"{\"take_over_explorer_windows\":true}");
        experimental.MigrateIntegration();
        ok &= Report("legacy experiment migrates without enabling registry scopes",
            experimental.integration_enabled && !experimental.integration_folders &&
            !experimental.integration_win_e && !experimental.integration_this_pc);
        prefs.persist = true; // This test's GetPulseDataDir is empty; no preferences are written.
        settings.IntegrationAction(1);
        ok &= Report("saving failure is visible and retryable",
            settings.IntegrationState() == 3 && settings.IntegrationCanRetry());
        prefs.persist = false;
        settings.IntegrationAction(1);
        ok &= Report("successful save clears prior save failure while master remains off",
            settings.IntegrationState() == 0 && !settings.IntegrationCanRetry());
        return ok ? 0 : 1;
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--this-pc") return TestThisPc() ? 0 : 1;
    if (argc == 2 && std::wstring(argv[1]) == L"--context-menu-prefs") {
        pulse::app::AppPrefs prefs;
        prefs.persist = false;
        pulse::app::ContextMenuPrefs context;
        context.persist = false;
        context.ResetToDefaults();
        const auto category = pulse::ipc::CtxMenuCategory::Share;
        const std::wstring send_key = pulse::ipc::CatalogKey(L"Send to", true);
        const std::wstring other_key = pulse::ipc::CatalogKey(L"Other sharing action", false);
        context.RecordSeen(send_key, L"Send to", true, category, true);
        context.SetItemEnabled(other_key, false);
        pulse::index::IndexClient index;
        pulse::index::NetworkAgentClient network;
        pulse::app::SettingsController settings;
        settings.BindUi(prefs, context, index, network, {});
        settings.ToggleUi(100);
        bool passed = Report("enabling Send to enables Share without enabling disabled siblings",
            context.share && context.ItemEnabled(send_key, category, true) &&
            !context.ItemEnabled(other_key, category, true));
        pulse::app::ContextMenuPrefs loaded;
        loaded.persist = false;
        loaded.FromJson(context.ToJson());
        passed &= Report("linked context menu settings survive serialization",
            loaded.share && loaded.ItemEnabled(send_key, category, true) &&
            !loaded.ItemEnabled(other_key, category, true));
        settings.ToggleUi(100);
        passed &= Report("disabling Send to preserves the Share group",
            context.share && !context.ItemEnabled(send_key, category, true));
        context.RecordSeen(L"test-system", L"System action", false,
            pulse::ipc::CtxMenuCategory::Rotate, true);
        settings.ToggleUi(101);
        passed &= Report("enabling a system item also enables its parent group",
            context.GroupEnabled(pulse::ipc::CtxMenuGroup::System));
        return passed ? 0 : 1;
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--layout-search-prefs") {
        pulse::app::AppPrefs prefs;
        prefs.persist = false;
        prefs.sidebar_width = 360;
        prefs.address_search_current = true;
        prefs.address_search_content = true;
        pulse::app::AppPrefs loaded;
        loaded.persist = false;
        loaded.FromJson(prefs.ToJson());
        bool ok = Report("sidebar and search preferences round trip", loaded.sidebar_width == 360 &&
            loaded.address_search_current && loaded.address_search_content);
        loaded.FromJson(L"{\"sidebar_width\":9999}");
        ok &= Report("invalid width restores default", loaded.sidebar_width == 224);
        loaded.FromJson(L"{}");
        ok &= Report("old preferences load defaults", loaded.sidebar_width == 224 &&
            !loaded.address_search_current && !loaded.address_search_content);
        loaded.ResetToDefaults();
        ok &= Report("reset restores sidebar width", loaded.sidebar_width == 224);
        {
            // The sidebar must leave room for both the file list and the shared toolbar.
            using namespace pulse::ui;
            ok &= Report("panel limit: without details the shared toolbar retains 360 DIP",
                std::abs(MaxSidebarWidthDip(1600.0f, 340.0f, false, 8.0f) - 1240.0f) < 0.5f);
            ok &= Report("panel limit: an open details panel is reserved by the sidebar",
                std::abs(MaxSidebarWidthDip(1600.0f, 340.0f, true, 8.0f) - 1140.0f) < 0.5f);
            ok &= Report("panel limit: the details panel keeps the sidebar and the list",
                std::abs(MaxDetailsWidthDip(1600.0f, 224.0f, false, 8.0f) - 1256.0f) < 0.5f);
            ok &= Report("panel limit: a collapsed sidebar leaves its rail",
                std::abs(MaxDetailsWidthDip(880.0f, 224.0f, true, 8.0f) - 712.0f) < 0.5f);
            ok &= Report("panel limit: narrow windows stop at the pane minimums",
                MaxSidebarWidthDip(400.0f, 340.0f, true, 8.0f) == kSidebarMinWidthDip &&
                MaxDetailsWidthDip(400.0f, 224.0f, false, 8.0f) == kDetailsMinWidthDip);
        }
        return ok ? 0 : 1;
    }
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    if (argc == 2 && std::wstring(argv[1]) == L"--global-search") {
        pulse::app::AppPrefs prefs;
        prefs.persist = false;
        bool ok = Report("global search defaults to off with Alt Space", !prefs.global_search_enabled && prefs.global_search_modifiers == MOD_ALT && prefs.global_search_key == VK_SPACE);
        pulse::app::ContextMenuPrefs context;
        context.persist = false;
        pulse::index::IndexClient index;
        pulse::index::NetworkAgentClient network;
        pulse::app::SettingsController settings;
        pulse::app::SettingsEffect effect = pulse::app::SettingsEffect::None;
        pulse::app::SettingsController::UiCallbacks callbacks;
        callbacks.apply_effects = [&](pulse::app::SettingsEffect value) { effect = value; };
        settings.BindUi(prefs, context, index, network, std::move(callbacks));
        settings.ToggleUi(15);
        ok &= Report("global search toggle applies registration", prefs.global_search_enabled && pulse::app::HasEffect(effect, pulse::app::SettingsEffect::GlobalSearch));
        settings.BeginGlobalSearchHotkeyCapture();
        settings.CaptureGlobalSearchHotkey(VK_CONTROL, MOD_CONTROL);
        settings.CaptureGlobalSearchHotkey('A', 0);
        ok &= Report("invalid shortcut keeps capture and previous binding", settings.global_search_hotkey_capturing() && prefs.global_search_key == VK_SPACE);
        settings.CaptureGlobalSearchHotkey(VK_ESCAPE, 0);
        ok &= Report("Escape cancels shortcut capture", !settings.global_search_hotkey_capturing() && prefs.global_search_key == VK_SPACE);
        settings.BeginGlobalSearchHotkeyCapture();
        effect = pulse::app::SettingsEffect::None;
        settings.CaptureGlobalSearchHotkey('K', MOD_CONTROL | MOD_SHIFT);
        ok &= Report("shortcut capture saves and applies combination", !settings.global_search_hotkey_capturing() && prefs.global_search_key == 'K' && prefs.global_search_modifiers == (MOD_CONTROL | MOD_SHIFT) && pulse::app::HasEffect(effect, pulse::app::SettingsEffect::GlobalSearch));
        pulse::app::AppPrefs loaded;
        loaded.persist = false;
        loaded.FromJson(prefs.ToJson());
        ok &= Report("global search preferences round trip", loaded.global_search_enabled && loaded.global_search_key == 'K' && loaded.global_search_modifiers == (MOD_CONTROL | MOD_SHIFT));
        loaded.FromJson(L"{}");
        ok &= Report("older preferences retain safe defaults", !loaded.global_search_enabled && loaded.global_search_key == VK_SPACE && loaded.global_search_modifiers == MOD_ALT);
        prefs.persist = true; // This executable's data-directory stub is empty, so Save fails without touching disk.
        effect = pulse::app::SettingsEffect::None;
        settings.BeginGlobalSearchHotkeyCapture();
        settings.CaptureGlobalSearchHotkey('L', MOD_ALT);
        ok &= Report("failed shortcut save rolls back and leaves registration unchanged", prefs.global_search_key == 'K' &&
            prefs.global_search_modifiers == (MOD_CONTROL | MOD_SHIFT) && effect == pulse::app::SettingsEffect::None && settings.global_search_hotkey_capturing());
        settings.ToggleUi(15);
        ok &= Report("failed toggle save rolls back without registration", prefs.global_search_enabled && effect == pulse::app::SettingsEffect::None);
        prefs.persist = false;
        settings.ResetUi();
        ok &= Report("settings reset cancels capture", !settings.global_search_hotkey_capturing());
        return ok ? 0 : 1;
    }
    using pulse::app::HasEffect;
    using pulse::app::SettingsController;
    using pulse::app::SettingsEffect;
    using pulse::app::ContextMenuController;
    using pulse::app::SingleInstanceCoordinator;
    using pulse::app::TrayController;

    bool passed = true;

    {
        pulse::app::UncProbeScheduler probes;
        const uint64_t first = probes.Begin();
        const bool first_completed = probes.Finish(first);
        const uint64_t second = probes.Begin();
        const bool stale_completed = probes.Finish(first);
        const bool second_still_active = probes.IsActive(second);
        const bool second_completed = probes.Finish(second);
        passed &= Report("late UNC result cannot finish a newer same-path probe",
                         first_completed && first != second && !stale_completed &&
                         second_still_active && second_completed && probes.active_id == 0);
    }

    passed &= Report("display paths strip extended UNC prefixes without losing the server",
        pulse::path::StripExtendedPathPrefix(L"\\\\?\\UNC\\192.168.0.254\\资料\\项目") ==
            L"\\\\192.168.0.254\\资料\\项目");
    passed &= Report("display paths preserve normal and long local paths",
        pulse::path::StripExtendedPathPrefix(L"C:\\资料") == L"C:\\资料" &&
        pulse::path::StripExtendedPathPrefix(L"\\\\?\\C:\\" + std::wstring(300, L'a')) ==
            L"C:\\" + std::wstring(300, L'a'));
    {
        pulse::app::BlankPaneClickRelease click;
        click.pending = true;
        click.owns_capture = true;
        click.same_context = true;
        click.blank_list_hit = true;
        click.drag_width = 4;
        click.drag_height = 6;
        const auto accepts = pulse::app::IsBlankPaneBackClick;
        passed &= Report("blank pane click navigates back on release", accepts(click));
        auto changed = click;
        changed.delta_x = -3;
        changed.delta_y = 5;
        passed &= Report("blank pane click tolerates motion below drag threshold", accepts(changed));
        bool rejects_boundary = true;
        for (const int delta : {-4, 4}) {
            changed = click;
            changed.delta_x = delta;
            rejects_boundary &= !accepts(changed);
        }
        for (const int delta : {-6, 6}) {
            changed = click;
            changed.delta_y = delta;
            rejects_boundary &= !accepts(changed);
        }
        passed &= Report("blank pane release at either drag threshold never goes back", rejects_boundary);
        changed = click;
        changed.marquee_active = true;
        passed &= Report("marquee returning to its origin never goes back", !accepts(changed));
        changed = click;
        changed.modified = true;
        passed &= Report("modified blank pane release never goes back", !accepts(changed));
        changed = click;
        changed.owns_capture = false;
        passed &= Report("capture loss cancels blank pane back", !accepts(changed));
        changed = click;
        changed.same_context = false;
        passed &= Report("changed pane tab or view cancels blank pane back", !accepts(changed));
        changed = click;
        changed.blank_list_hit = false;
        passed &= Report("row control and outside-list release never go back", !accepts(changed));
        changed = click;
        changed.pending = false;
        passed &= Report("cancelled or second double-click release never goes back", !accepts(changed));
    }
    const std::wstring mutex_name = L"Local\\Pulse.ControllerTest." +
        std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
    SingleInstanceCoordinator primary;
    SingleInstanceCoordinator duplicate;
    passed &= Report("single-instance coordinator owns the primary mutex",
        primary.Acquire(mutex_name) == SingleInstanceCoordinator::AcquireResult::Primary);
    passed &= Report("single-instance coordinator detects a duplicate",
        duplicate.Acquire(mutex_name) == SingleInstanceCoordinator::AcquireResult::Existing);
    primary.Release();
    passed &= Report("single-instance mutex is released by its owner",
        duplicate.Acquire(mutex_name) == SingleInstanceCoordinator::AcquireResult::Primary);

    std::wstring decoded;
    std::wstring path = L"C:\\目录\\file.txt";
    COPYDATASTRUCT data{};
    data.dwData = SingleInstanceCoordinator::OpenPathMessageId();
    data.cbData = static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t));
    data.lpData = path.data();
    passed &= Report("single-instance IPC decodes a terminated UTF-16 path",
        SingleInstanceCoordinator::DecodeOpenPath(&data, decoded) && decoded == path);
    data.cbData -= sizeof(wchar_t);
    passed &= Report("single-instance IPC rejects a non-terminated path",
        !SingleInstanceCoordinator::DecodeOpenPath(&data, decoded));
    data.cbData = 3;
    passed &= Report("single-instance IPC rejects odd byte counts",
        !SingleInstanceCoordinator::DecodeOpenPath(&data, decoded));
    wchar_t embedded[] = {L'C', L':', L'\0', L'x', L'\0'};
    data.cbData = sizeof(embedded);
    data.lpData = embedded;
    passed &= Report("single-instance IPC rejects embedded NUL characters",
        !SingleInstanceCoordinator::DecodeOpenPath(&data, decoded));

    // Cross-window tab drag: the folder a dragged tab was showing travels as its
    // own message, which the receiver opens as a tab of its own even when it
    // already shows that folder. It must not be mistaken for the shell's "open
    // this folder" forward, where activating the existing tab is the intent.
    WNDCLASSW sink_class{};
    sink_class.lpfnWndProc = TabTransferSinkProc;
    sink_class.hInstance = GetModuleHandleW(nullptr);
    sink_class.lpszClassName = L"PulseTabTransferSink";
    RegisterClassW(&sink_class);
    HWND sink_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        sink_class.lpszClassName, L"", WS_POPUP, -32000, -32000, 100, 100,
        nullptr, nullptr, sink_class.hInstance, nullptr);
    TabTransferSink sink;
    SetWindowLongPtrW(sink_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&sink));
    const std::wstring transferred = L"D:\\工作\\资料";
    const bool delivered = sink_hwnd != nullptr &&
        SingleInstanceCoordinator::SendTabTransfer(sink_hwnd, transferred);
    passed &= Report("tab transfer reaches the window that takes the tab",
        delivered && sink.decoded && sink.path == transferred);

    // The same hand-off with what the user had selected in the tab it left: the
    // receiving window restores those rows (and previews the same file) instead of
    // parking on the first entry of the folder.
    const std::wstring handoff_focus = L"万事达冷链定制CAD插件问题反馈.pdf";
    SingleInstanceCoordinator::TabTransfer with_selection;
    with_selection.path = transferred;
    with_selection.focus_name = handoff_focus;
    with_selection.selected_names = { L"pipeline_article", handoff_focus };
    passed &= Report("tab transfer carries the selection the tab had",
        SingleInstanceCoordinator::SendTabTransfer(sink_hwnd, with_selection) &&
        sink.decoded && sink.path == transferred &&
        sink.transfer.focus_name == handoff_focus &&
        sink.transfer.selected_names.size() == 2 &&
        sink.transfer.selected_names[1] == handoff_focus);
    COPYDATASTRUCT forwarded{};
    forwarded.dwData = SingleInstanceCoordinator::OpenPathMessageId();
    forwarded.cbData = static_cast<DWORD>((transferred.size() + 1) * sizeof(wchar_t));
    forwarded.lpData = const_cast<wchar_t*>(transferred.data());
    std::wstring ignored;
    passed &= Report("tab transfer message is not the shell's open-path forward",
        sink.message_id != 0 && sink.message_id != forwarded.dwData &&
        !SingleInstanceCoordinator::DecodeTabTransfer(&forwarded, ignored));
    forwarded.dwData = sink.message_id;
    passed &= Report("shell open-path forward is not a tab transfer",
        !SingleInstanceCoordinator::DecodeOpenPath(&forwarded, ignored));
    if (sink_hwnd) DestroyWindow(sink_hwnd);

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC",
        L"Pulse tray controller test", WS_POPUP, -32000, -32000, 100, 100,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    TrayController tray;
    tray.Attach(hwnd, GetModuleHandleW(nullptr));
    int before_restore = 0;
    tray.SetBeforeRestore([&before_restore] { ++before_restore; });
    tray.RestoreWindow();
    passed &= Report("tray controller owns window restore behavior",
        hwnd && IsWindowVisible(hwnd));
    tray.RestoreWindow();
    passed &= Report("tray restore hook runs only while the window is hidden",
        before_restore == 1);
    ShowWindow(hwnd, SW_HIDE);
    tray.Detach();
    if (hwnd) DestroyWindow(hwnd);

    pulse::app::AppPrefs prefs;
    passed &= Report("pinned tab names are shown by default", prefs.show_pinned_tab_names);
    pulse::app::ContextMenuPrefs context;
    prefs.persist = false;
    context.persist = false;
    SettingsController settings;
    settings.SelectPage(2);
    settings.ScrollBy(-120.0f, 1.0f, 100.0f);
    passed &= Report("settings controller owns page and clamped scroll state",
        settings.page() == 2 && settings.scroll() == 48.0f);
    passed &= Report("settings controller maps stable page routes",
        SettingsController::PageFromName(L"index") == 1 &&
        std::wstring(SettingsController::PageName(2)) == L"context" &&
        SettingsController::PageFromName(L"about") == 3 &&
        std::wstring(SettingsController::PageName(3)) == L"about" &&
        SettingsController::PageFromName(L"duplicates") == 4 &&
        std::wstring(SettingsController::PageName(4)) == L"duplicates");

    pulse::app::SettingsTaskResult completed_task;
    completed_task.task.kind = pulse::app::SettingsTaskKind::InstallService;
    completed_task.ok = true;
    auto task_effect = settings.CompleteTask(completed_task, true);
    passed &= Report("settings completion owns service refresh state",
        task_effect.refresh_index && settings.service_installed());
    completed_task.task.kind = pulse::app::SettingsTaskKind::NetworkAdd;
    completed_task.task.path = L"\\\\server\\share";
    completed_task.task.pin = true;
    task_effect = settings.CompleteTask(completed_task, true);
    passed &= Report("settings completion returns network pin side effect",
        !task_effect.refresh_index && task_effect.pin_network == completed_task.task.path);
    completed_task.task.kind = pulse::app::SettingsTaskKind::DiagnosticsExport;
    completed_task.task.path = L"C:\\diagnostics-export";
    task_effect = settings.CompleteTask(completed_task, true);
    passed &= Report("diagnostics completion opens export without index refresh",
        !task_effect.refresh_index && task_effect.open_path == completed_task.task.path);
    completed_task.task.kind = pulse::app::SettingsTaskKind::NetworkRemove;
    completed_task.ok = false;
    task_effect = settings.CompleteTask(completed_task, true);
    passed &= Report("settings completion owns operation error text",
        !task_effect.refresh_index && settings.error() == L"无法移除服务器文件夹。");

    pulse::index::IndexClient index_client;
    pulse::index::NetworkAgentClient network_client;
    SettingsController& settings_ui = settings;
    int applied_effects = 0;
    SettingsEffect last_effect = SettingsEffect::None;
    bool picked_image = false;
    SettingsController::UiCallbacks ui_callbacks;
    ui_callbacks.pick_image = [&](std::wstring& selected) {
        picked_image = true;
        selected = L"C:\\wallpaper.png";
        return true;
    };
    ui_callbacks.apply_effects = [&](SettingsEffect effect) {
        last_effect = effect;
        if (effect != SettingsEffect::None) ++applied_effects;
    };
    settings_ui.BindUi(prefs, context, index_client, network_client,
                       std::move(ui_callbacks));
    settings_ui.WindowEffect(L"mica");
    passed &= Report("settings window effect returns material invalidation",
        prefs.window_effect == L"mica" &&
        HasEffect(last_effect, SettingsEffect::WindowMaterial));
    settings_ui.AccentChoice(false, 0x12AB34);
    passed &= Report("settings accent is normalized and reports its side effect",
        prefs.accent_rgb == L"12AB34" && HasEffect(last_effect, SettingsEffect::Accent));
    settings_ui.RowHeight(2);
    passed &= Report("settings density owns preference mutation",
        prefs.row_height == 40 && HasEffect(last_effect, SettingsEffect::RowHeight));
    settings_ui.TrayIconSize(0);
    passed &= Report("settings tray size owns preference mutation",
        prefs.tray_icon_size == 40 && HasEffect(last_effect, SettingsEffect::TrayDeckIcon));
    settings_ui.Language(L"en-US");
    passed &= Report("settings language uses a stable identifier",
        prefs.language == L"en-US" && HasEffect(last_effect, SettingsEffect::Language));
    settings_ui.ToggleUi(2);
    passed &= Report("settings close behavior requests tray synchronization",
        prefs.keep_running_on_close && HasEffect(last_effect, SettingsEffect::TrayVisibility));
    settings_ui.ToggleUi(4);
    passed &= Report("settings status performance toggle persists",
        prefs.show_status_performance &&
        HasEffect(last_effect, SettingsEffect::StatusBarPerformance));
    settings_ui.WindowEffect(L"mica-alt");
    settings_ui.RowHeight(0);
    settings_ui.AccentChoice(false, 0x2468AC);
    passed &= Report("settings UI controller routes preference commands",
        prefs.window_effect == L"mica-alt" && prefs.row_height == 28 &&
        prefs.accent_rgb == L"2468AC" && applied_effects == 10);
    settings_ui.NotifyIcon(2);
    passed &= Report("settings tray icon choice requests tray synchronization",
        prefs.notify_icon_mode == 2 && HasEffect(last_effect, SettingsEffect::TrayVisibility));
    settings_ui.NotifyIcon(5);
    passed &= Report("settings tray icon ignores out-of-range choices", prefs.notify_icon_mode == 2);
    settings_ui.NotifyIcon(0);
    settings_ui.ToggleUi(5);
    passed &= Report("settings hidden visibility toggle persists and refreshes panes",
        prefs.show_hidden_files && HasEffect(last_effect, SettingsEffect::FileVisibility));
    settings_ui.ToggleUi(5);
    passed &= Report("settings hidden visibility toggle is reversible", !prefs.show_hidden_files);
    pulse::app::AppPrefs protected_prefs;
    settings_ui.ToggleUi(16);
    passed &= Report("settings protected system files toggle persists and refreshes panes",
        prefs.show_protected_os_files && HasEffect(last_effect, SettingsEffect::FileVisibility) &&
        protected_prefs.FromJson(prefs.ToJson()) && protected_prefs.show_protected_os_files);
    settings_ui.ToggleUi(16);
    passed &= Report("settings protected system files toggle is reversible",
        !prefs.show_protected_os_files);
    settings_ui.ToggleUi(6);
    pulse::app::AppPrefs parsed_prefs;
    passed &= Report("pinned tab names toggle off and persist",
        !prefs.show_pinned_tab_names && parsed_prefs.FromJson(prefs.ToJson()) &&
        !parsed_prefs.show_pinned_tab_names);
    settings_ui.ToggleUi(6);
    passed &= Report("pinned tab names toggle on and persist",
        prefs.show_pinned_tab_names && parsed_prefs.FromJson(prefs.ToJson()) &&
        parsed_prefs.show_pinned_tab_names);
    {
        // Multi-window mode: off by default keeps every entry in the running
        // window; on lets tabs be torn off and windows be opened (jump_list.cpp,
        // tab_controller.cpp, app_input.cpp).
        pulse::app::AppPrefs jump_prefs;
        passed &= Report("multi-window mode is off by default",
            !jump_prefs.multi_instance_mode);
        settings_ui.ToggleUi(34);
        passed &= Report("multi-window mode turns on and persists",
            prefs.multi_instance_mode && parsed_prefs.FromJson(prefs.ToJson()) &&
            parsed_prefs.multi_instance_mode &&
            HasEffect(last_effect, SettingsEffect::MultiInstance));
        settings_ui.ToggleUi(34);
        passed &= Report("multi-window mode switches back and persists",
            !prefs.multi_instance_mode && parsed_prefs.FromJson(prefs.ToJson()) &&
            !parsed_prefs.multi_instance_mode);
    }
    {
        pulse::app::AppPrefs update_prefs;
        passed &= Report("automatic update checks default on, also for an older app.json",
            prefs.auto_check_updates && update_prefs.FromJson(L"{\"show_hints\":true}") &&
            update_prefs.auto_check_updates);
        settings_ui.ToggleUi(31);
        passed &= Report("automatic update checks toggle off and persist",
            !prefs.auto_check_updates && update_prefs.FromJson(prefs.ToJson()) &&
            !update_prefs.auto_check_updates);
        settings_ui.ToggleUi(31);
        passed &= Report("automatic update checks toggle back on and persist",
            prefs.auto_check_updates && update_prefs.FromJson(prefs.ToJson()) &&
            update_prefs.auto_check_updates);
    }
    settings_ui.Wallpaper(0);
    {
        using pulse::app::kBlankClickOff;
        using pulse::app::kBlankClickBack;
        using pulse::app::kBlankClickUp;
        passed &= Report("blank click navigation defaults off", prefs.blank_click_action == kBlankClickOff);
        settings_ui.BlankClick(1);
        passed &= Report("blank click navigation goes back and persists",
            prefs.blank_click_action == kBlankClickBack && parsed_prefs.FromJson(prefs.ToJson()) &&
            parsed_prefs.blank_click_action == kBlankClickBack);
        settings_ui.BlankClick(2);
        passed &= Report("blank click navigation goes up and persists",
            prefs.blank_click_action == kBlankClickUp && parsed_prefs.FromJson(prefs.ToJson()) &&
            parsed_prefs.blank_click_action == kBlankClickUp &&
            prefs.ToJson().find(L"\"blank_click_go_back\":true") != std::wstring::npos);
        settings_ui.BlankClick(7);
        passed &= Report("blank click navigation ignores an out-of-range choice",
            prefs.blank_click_action == kBlankClickUp);
        settings_ui.BlankClick(0);
        passed &= Report("blank click navigation disables and persists",
            prefs.blank_click_action == kBlankClickOff && parsed_prefs.FromJson(prefs.ToJson()) &&
            parsed_prefs.blank_click_action == kBlankClickOff &&
            prefs.ToJson().find(L"\"blank_click_go_back\":false") != std::wstring::npos);
        parsed_prefs.blank_click_action = kBlankClickUp;
        passed &= Report("legacy preferences leave blank click navigation off",
            parsed_prefs.FromJson(L"{}") && parsed_prefs.blank_click_action == kBlankClickOff);
        passed &= Report("legacy on/off flag migrates to back",
            parsed_prefs.FromJson(L"{\"blank_click_go_back\":true}") &&
            parsed_prefs.blank_click_action == kBlankClickBack);
        passed &= Report("blank click action wins over the legacy flag and is clamped",
            parsed_prefs.FromJson(L"{\"blank_click_action\":2,\"blank_click_go_back\":true}") &&
            parsed_prefs.blank_click_action == kBlankClickUp &&
            parsed_prefs.FromJson(L"{\"blank_click_action\":9}") && parsed_prefs.blank_click_action == kBlankClickOff);
        parsed_prefs.blank_click_action = kBlankClickUp;
        parsed_prefs.ResetToDefaults();
        passed &= Report("reset disables blank click navigation", parsed_prefs.blank_click_action == kBlankClickOff);
        passed &= Report("blank click: back uses history, up and missing history go to the parent",
            pulse::app::BlankClickGoesBack(kBlankClickBack, true) &&
            !pulse::app::BlankClickGoesBack(kBlankClickBack, false) &&
            !pulse::app::BlankClickGoesBack(kBlankClickUp, true) &&
            !pulse::app::BlankClickGoesBack(kBlankClickOff, true));
        passed &= Report("blank click setting has localized choices",
            pulse::l10n::Get(pulse::l10n::StringId::SettingsBlankClickOff) == L"\u4E0D\u64CD\u4F5C" &&
            !pulse::l10n::Get(pulse::l10n::StringId::SettingsBlankClickBack).empty());
    }
    passed &= Report("closing the last tab setting has localized text",
        pulse::l10n::Get(pulse::l10n::StringId::SettingsCloseLastTab) ==
            L"\u5173\u95ED\u6700\u540E\u4E00\u4E2A\u6807\u7B7E\u9875\u65F6\u5173\u95ED\u7A97\u53E3" &&
        !pulse::l10n::Get(pulse::l10n::StringId::SettingsCloseLastTabDesc).empty());
    passed &= Report("closing the last tab keeps the window by default",
        !prefs.close_window_with_last_tab);
    settings_ui.ToggleUi(26);
    passed &= Report("closing the last tab closes the window when enabled and persisted",
        prefs.close_window_with_last_tab && parsed_prefs.FromJson(prefs.ToJson()) &&
        parsed_prefs.close_window_with_last_tab);
    settings_ui.ToggleUi(26);
    passed &= Report("closing the last tab setting disables and persists",
        !prefs.close_window_with_last_tab && parsed_prefs.FromJson(prefs.ToJson()) &&
        !parsed_prefs.close_window_with_last_tab);
    parsed_prefs.close_window_with_last_tab = true;
    passed &= Report("legacy preferences keep the window on the last tab close",
        parsed_prefs.FromJson(L"{}") && !parsed_prefs.close_window_with_last_tab);
    parsed_prefs.close_window_with_last_tab = true;
    parsed_prefs.ResetToDefaults();
    passed &= Report("reset keeps the window on the last tab close",
        !parsed_prefs.close_window_with_last_tab);
    passed &= Report("confirm before deleting setting has localized text",
        !pulse::l10n::Get(pulse::l10n::StringId::SettingsConfirmDelete).empty() &&
        !pulse::l10n::Get(pulse::l10n::StringId::SettingsConfirmDeleteDesc).empty() &&
        !pulse::l10n::Get(pulse::l10n::StringId::RecycleConfirmOne).empty() &&
        !pulse::l10n::Get(pulse::l10n::StringId::RecycleConfirmManyFormat).empty());
    passed &= Report("deleting to the Recycle Bin does not ask by default",
        !prefs.confirm_recycle_delete);
    settings_ui.ToggleUi(32);
    passed &= Report("confirm before deleting enables and persists",
        prefs.confirm_recycle_delete && parsed_prefs.FromJson(prefs.ToJson()) &&
        parsed_prefs.confirm_recycle_delete);
    settings_ui.ToggleUi(32);
    passed &= Report("confirm before deleting disables and persists",
        !prefs.confirm_recycle_delete && parsed_prefs.FromJson(prefs.ToJson()) &&
        !parsed_prefs.confirm_recycle_delete);
    parsed_prefs.confirm_recycle_delete = true;
    passed &= Report("legacy preferences do not ask before deleting",
        parsed_prefs.FromJson(L"{}") && !parsed_prefs.confirm_recycle_delete);
    parsed_prefs.confirm_recycle_delete = true;
    parsed_prefs.ResetToDefaults();
    passed &= Report("reset does not ask before deleting",
        !parsed_prefs.confirm_recycle_delete);
    passed &= Report("outline selected items setting has localized text",
        !pulse::l10n::Get(pulse::l10n::StringId::ListSelectionOutline).empty() &&
        !pulse::l10n::Get(pulse::l10n::StringId::ListSelectionOutlineDesc).empty());
    passed &= Report("selected items have no outline by default", !prefs.list_selection_outline);
    settings_ui.ToggleUi(33);
    passed &= Report("outline selected items enables and persists",
        prefs.list_selection_outline && parsed_prefs.FromJson(prefs.ToJson()) &&
        parsed_prefs.list_selection_outline);
    settings_ui.ToggleUi(33);
    passed &= Report("outline selected items disables and persists",
        !prefs.list_selection_outline && parsed_prefs.FromJson(prefs.ToJson()) &&
        !parsed_prefs.list_selection_outline);
    parsed_prefs.list_selection_outline = true;
    passed &= Report("legacy preferences draw no selection outline",
        parsed_prefs.FromJson(L"{}") && !parsed_prefs.list_selection_outline);
    parsed_prefs.list_selection_outline = true;
    parsed_prefs.ResetToDefaults();
    passed &= Report("reset draws no selection outline", !parsed_prefs.list_selection_outline);
    passed &= Report("start in tray setting has localized text",
        pulse::l10n::Get(pulse::l10n::StringId::SettingsStartInTray) ==
            L"\u5F00\u673A\u81EA\u542F\u65F6\u9690\u85CF\u5230\u6258\u76D8" &&
        !pulse::l10n::Get(pulse::l10n::StringId::SettingsStartInTrayDesc).empty());
    passed &= Report("start in tray defaults off", !prefs.start_in_tray);
    settings_ui.ToggleUi(27);
    passed &= Report("start in tray enables and persists",
        prefs.start_in_tray && parsed_prefs.FromJson(prefs.ToJson()) && parsed_prefs.start_in_tray);
    settings_ui.ToggleUi(27);
    passed &= Report("start in tray disables and persists",
        !prefs.start_in_tray && parsed_prefs.FromJson(prefs.ToJson()) && !parsed_prefs.start_in_tray);
    parsed_prefs.start_in_tray = true;
    passed &= Report("legacy preferences leave start in tray off",
        parsed_prefs.FromJson(L"{}") && !parsed_prefs.start_in_tray);
    parsed_prefs.start_in_tray = true;
    parsed_prefs.ResetToDefaults();
    passed &= Report("reset turns start in tray off", !parsed_prefs.start_in_tray);
    {
        const std::wstring exe = L"C:\\Program Files\\Pulse\\pulse.exe";
        passed &= Report("startup Run command quotes the exe and adds --startup",
            pulse::app::StartupCommandLine(exe) == L"\"C:\\Program Files\\Pulse\\pulse.exe\" --startup" &&
            pulse::app::StartupCommandLine(L"").empty());
        passed &= Report("startup Run command: only the old flagless value of this exe is repaired",
            pulse::app::StartupCommandNeedsRepair(L"\"C:\\Program Files\\Pulse\\pulse.exe\"", exe) &&
            pulse::app::StartupCommandNeedsRepair(L"\"c:\\program files\\pulse\\PULSE.EXE\"", exe) &&
            !pulse::app::StartupCommandNeedsRepair(pulse::app::StartupCommandLine(exe), exe) &&
            !pulse::app::StartupCommandNeedsRepair(L"\"D:\\Portable\\pulse.exe\"", exe) &&
            !pulse::app::StartupCommandNeedsRepair(L"", exe) &&
            !pulse::app::StartupCommandNeedsRepair(L"\"C:\\Program Files\\Pulse\\pulse.exe\"", L""));
        const wchar_t* startup_argv[] = {L"pulse.exe", L"--startup"};
        const wchar_t* folder_argv[] = {L"pulse.exe", L"C:\\--startup"};
        passed &= Report("startup launch is recognised only by the exact flag",
            pulse::app::HasStartupArgument(2, startup_argv) &&
            !pulse::app::HasStartupArgument(2, folder_argv) &&
            !pulse::app::HasStartupArgument(1, startup_argv) &&
            !pulse::app::HasStartupArgument(0, nullptr));
        passed &= Report("start in tray needs both a sign-in launch and the setting",
            pulse::app::StartsHiddenInTray(true, true) && !pulse::app::StartsHiddenInTray(true, false) &&
            !pulse::app::StartsHiddenInTray(false, true) && !pulse::app::StartsHiddenInTray(false, false));
    }
    passed &= Report("last tab close: only the single unpinned tab closes the window",
        pulse::app::LastTabClosesWindow(1, false, true) && !pulse::app::LastTabClosesWindow(1, false, false) &&
        !pulse::app::LastTabClosesWindow(2, false, true) && !pulse::app::LastTabClosesWindow(1, true, true) &&
        !pulse::app::LastTabClosesWindow(0, false, true));
    passed &= Report("settings UI controller owns image selection flow",
        picked_image);
    settings_ui.ResetUi();
    passed &= Report("settings UI controller has explicit binding lifecycle",
        !settings_ui.ui_bound());

    pulse::app::LayoutTabSnapshot saved;
    saved.pinned = true;
    saved.marker_rgb = 0x0078D4;
    saved.group = 4;
    saved.layout = 1;
    saved.focused = 1;
    saved.panes.push_back({ L"C:\\first", pulse::ui::ViewMode::Tiles, { 0.2f, 0.5f, 0.8f }, {} });
    saved.panes.push_back({ L"D:\\loose", pulse::ui::ViewMode::Details, {}, {} });
    saved.panes.push_back({ L"", pulse::ui::ViewMode::Details, {}, {} });
    pulse::app::LayoutTab restored;
    std::vector<std::wstring> loaded_paths;
    pulse::app::RestoreLayoutTab(restored, saved,
        [&](pulse::app::Tab& tab, const std::wstring& loaded) {
            tab.current_path = loaded;
            loaded_paths.push_back(loaded);
        });
    // An empty saved path is This PC, a real location: the pane is kept, not skipped.
    passed &= Report("session layout restore keeps This PC (empty path) panes",
        restored.pinned && restored.tab_group == 0 &&
        restored.layout == pulse::app::LayoutPreset::TwoVertical &&
        restored.panes.size() == 3 && restored.focused_index == 1 &&
        loaded_paths.size() == 3 && loaded_paths[0] == L"C:\\first" && loaded_paths[2].empty());
    pulse::app::LayoutTabSnapshot single;
    single.layout = 1;
    single.panes.push_back({ L"", pulse::ui::ViewMode::Details, {}, {} });
    pulse::app::LayoutTab grown;
    std::vector<std::wstring> grown_paths;
    pulse::app::RestoreLayoutTab(grown, single,
        [&](pulse::app::Tab& tab, const std::wstring& loaded) {
            tab.current_path = loaded;
            grown_paths.push_back(loaded);
        });
    passed &= Report("session layout restore grows to the preset by cloning This PC",
        grown.panes.size() == 2 && grown_paths.size() == 2 &&
        grown_paths[0].empty() && grown_paths[1].empty());
    const auto captured = pulse::app::CaptureLayoutTab(restored);
    passed &= Report("session layout restore and capture preserve the tab marker",
        restored.marker_rgb == 0x0078D4 && captured.marker_rgb == 0x0078D4);
    passed &= Report("session layout capture preserves folder presentation state",
        captured.pinned && captured.layout == 1 && captured.panes.size() == 3 &&
        captured.panes[2].path.empty() &&
        captured.panes[0].view == pulse::ui::ViewMode::Tiles &&
        captured.panes[0].columns[1] == 0.5f);
    pulse::app::LayoutTab empty_restored;
    pulse::app::RestoreLayoutTab(empty_restored, {},
        [](pulse::app::Tab& tab, const std::wstring& loaded) {
            tab.current_path = loaded;
        });
    passed &= Report("session layout restore supplies a usable fallback folder",
        empty_restored.panes.size() == 1 &&
        empty_restored.panes[0]->view.current_path == L"C:\\");
    pulse::app::WindowTabs restored_tabs;
    std::vector<pulse::app::GroupSessionSnapshot> groups{
        { 4, L"work", 0x00112233, false },
        { 4, L"duplicate", 0x00445566, true },
        { -1, L"invalid", 0, false },
    };
    saved.pinned = false;
    saved.group = 4;
    pulse::app::RestoreWindowTabs(restored_tabs, { saved }, groups, 0,
        [&](pulse::app::Tab& tab, const std::wstring& loaded) {
            tab.current_path = loaded;
        });
    passed &= Report("session window tabs restore validates groups",
        restored_tabs.tab_groups.size() == 1 &&
        restored_tabs.next_tab_group_id == 5 && restored_tabs.items.size() == 1 &&
        restored_tabs.items[0]->tab_group == 4);

    // Dragging a tab across a group: an expanded run is crossed at the member
    // the dragged tab meets (its leading edge only has to pass that member's
    // centre), while a folded run is a chip crossed at the chip's own centre.
    {
        using pulse::app::RunCrossCenter;
        passed &= Report("group crossing: dragging right crosses at the first member centre",
            RunCrossCenter(false, 100.0f, 260.0f, 40.0f, 1) == 120.0f);
        passed &= Report("group crossing: dragging left crosses at the last member centre",
            RunCrossCenter(false, 100.0f, 260.0f, 40.0f, -1) == 240.0f);
        passed &= Report("group crossing: a folded group crosses at the block centre",
            RunCrossCenter(true, 100.0f, 260.0f, 40.0f, 1) == 180.0f &&
            RunCrossCenter(true, 100.0f, 260.0f, 40.0f, -1) == 180.0f);
    }

    ContextMenuController context_menu;
    uint32_t queried_token = 0;
    uint32_t invoked_command = 0;
    std::wstring invoked_verb;
    std::wstring invoked_text;
    int closed_sessions = 0;
    int folder_refreshes = 0;
    context_menu.SetShellOperations({
        [&](std::vector<std::wstring> paths, HWND, bool background, bool extended,
            std::vector<std::wstring>) {
            queried_token = paths == std::vector<std::wstring>{L"C:\\one.txt"} &&
                !background && !extended ? 17u : 0u;
            return queried_token;
        },
        [&](uint32_t) { ++closed_sessions; },
        [&](uint32_t, uint32_t command, std::wstring verb, std::wstring text) {
            invoked_command = command;
            invoked_verb = std::move(verb);
            invoked_text = std::move(text);
        },
        {}, {},
    });
    passed &= Report("context menu deduplicates static prefetch requests",
        context_menu.RequestStaticPrefetch(L".txt") &&
        !context_menu.RequestStaticPrefetch(L".txt"));
    context_menu.CompleteStaticVerbs(
        L".txt", { { L"edit", L"Edit text", L"" } }, context_menu.cache_generation());
    {
        // The registry changes while .doc is being read: the late result of the
        // old read must not refill the invalidated cache or cancel the new read.
        ContextMenuController stale;
        const bool requested = stale.RequestStaticPrefetch(L".doc");
        const uint32_t before = stale.cache_generation();
        stale.InvalidateCaches();
        const bool re_requested = stale.RequestStaticPrefetch(L".doc");
        stale.CompleteStaticVerbs(L".doc", { { L"old", L"Old verb", L"" } }, before);
        const bool old_dropped = !stale.HasCachedStaticVerbs(L".doc") &&
                                 !stale.RequestStaticPrefetch(L".doc");  // new read still pending
        stale.CompleteStaticVerbs(L".doc", { { L"new", L"New verb", L"" } }, stale.cache_generation());
        passed &= Report("context menu drops static verbs read before a cache invalidation",
            requested && re_requested && old_dropped && stale.HasCachedStaticVerbs(L".doc"));
    }
    context_menu.StartQuery(context, nullptr, { L"C:\\one.txt" }, false, L".txt",
        false, [](const std::wstring& path) { return path; }, {});
    passed &= Report("context menu begins a typed shell session",
        queried_token == 17);
    std::vector<pulse::ops::ShellMenuItem> com_items;
    pulse::ops::ShellMenuItem com_item;
    com_item.id = 23;
    com_item.verb = L"customverb";
    com_item.text = L"Custom action";
    com_items.push_back(com_item);
    const auto partial = context_menu.CompleteComQuery(
        17, com_items, GetTickCount64(), true);
    passed &= Report("context menu partial COM snapshot keeps the session open",
        partial.accepted && partial.partial);
    passed &= Report("context menu accepts only the active COM response",
        !context_menu.CompleteComQuery(99, {}, 1200).accepted);
    const auto completion = context_menu.CompleteComQuery(
        17, com_items, GetTickCount64());
    passed &= Report("context menu final COM snapshot finishes the session",
        completion.accepted && !completion.partial && completion.cache_key == L".txt");
    pulse::ui::FluentMenuItem base_item;
    base_item.command = 1;
    base_item.text = L"Base";
    bool prefs_changed = false;
    const auto display = context_menu.BuildDisplay(context, { base_item }, prefs_changed);
    passed &= Report("context menu composes cached static and live COM rows",
        display.size() >= 4 && prefs_changed);
    context_menu.InvalidateCaches();
    context_menu.OpenMenu({ std::move(base_item) });
    passed &= Report("context menu owns popup baseline state",
        context_menu.menu_open() && !context_menu.base_items().empty());
    pulse::ops::ShellMenuItem live_item = com_item;
    live_item.id = 42;
    passed &= Report("context menu remaps snapshot ids onto the live session",
        context_menu.CompleteComQuery(17, { live_item }, GetTickCount64()).accepted);
    context_menu.CloseMenu();
    context_menu.ScheduleFolderRefresh(2000);
    passed &= Report("context menu owns two-stage refresh deadlines",
        context_menu.ConsumeDueRefreshes(2399) == 0 &&
        context_menu.ConsumeDueRefreshes(2400) == 1 &&
        context_menu.ConsumeDueRefreshes(3600) == 1);
    // The shell host answers a right-click twice: a fast partial list (whatever
    // handlers finished inside its budget) and then the complete list. Only the
    // complete one may become the cache a later right-click paints from - that
    // is what made packaged verbs (WinRAR and friends) appear on one
    // right-click and vanish on the next.
    {
        pulse::app::ContextMenuController cache_menu;
        cache_menu.SetShellOperations({
            [](std::vector<std::wstring>, HWND, bool, bool, std::vector<std::wstring>) {
                return 31u;
            },
            [](uint32_t) {}, [](uint32_t, uint32_t, std::wstring, std::wstring) {}, {}, {},
        });
        pulse::app::ContextMenuPrefs cache_prefs;

        // First right-click: complete answer, so this is what must be cached.
        cache_menu.CompleteStaticVerbs(L".zip", {}, cache_menu.cache_generation());
        cache_menu.StartQuery(cache_prefs, nullptr, { L"C:\\one.zip" }, false, L".zip",
                              false, [](const std::wstring& path) { return path; }, {});
        std::vector<pulse::ops::ShellMenuItem> complete;
        pulse::ops::ShellMenuItem packaged;
        packaged.id = 5;
        packaged.verb = L"{b41db860-64e4-11d2-9906-e49fadc173ca}";
        packaged.text = L"WinRAR";
        complete.push_back(packaged);
        pulse::ops::ShellMenuItem classic;
        classic.id = 6;
        classic.text = L"Classic only";
        complete.push_back(classic);
        cache_menu.CompleteComQuery(31, complete, GetTickCount64());

        // A slow handler can make the host's next right-click answer with a
        // partial list that is missing the packaged verb; it must not overwrite
        // the complete one. Starting the session clears com_items_, exactly as
        // a real right-click does.
        cache_menu.StartQuery(cache_prefs, nullptr, { L"C:\\two.zip" }, false, L".zip",
                              false, [](const std::wstring& path) { return path; }, {});
        std::vector<pulse::ops::ShellMenuItem> early;
        pulse::ops::ShellMenuItem early_classic;
        early_classic.id = 9;
        early_classic.text = L"Classic only";
        early.push_back(early_classic);
        cache_menu.CompleteComQuery(31, early, GetTickCount64(), true);

        // What a later right-click paints before its own answer arrives.
        cache_menu.StartQuery(cache_prefs, nullptr, { L"C:\\three.zip" }, false, L".zip",
                              false, [](const std::wstring& path) { return path; }, {});
        cache_menu.SeedComItemsFromCache();
        bool cache_changed = false;
        const auto cached_display = cache_menu.BuildDisplay(
            cache_prefs, { pulse::ui::FluentMenuItem{} }, cache_changed);
        bool packaged_survived = false;
        for (const auto& row : cached_display)
            if (row.text == L"WinRAR") packaged_survived = true;
        passed &= Report("context menu never caches a partial COM snapshot",
            packaged_survived);
    }

    const bool handled_shell = context_menu.ExecuteShellCommand(
        pulse::app::CmdShellComBase + 23, {}, [&] { ++folder_refreshes; });
    passed &= Report("context menu invokes by verb when live ids differ",
        handled_shell && invoked_command == 42 &&
        invoked_verb == L"customverb" && invoked_text == L"Custom action" &&
        closed_sessions == 0 && folder_refreshes == 1);

    std::mutex task_mutex;
    std::condition_variable task_cv;
    bool task_done = false;
    bool task_started = false;
    bool release_task = false;
    pulse::app::SettingsTaskResult task_result;
    pulse::app::SettingsTask volume_task;
    volume_task.kind = pulse::app::SettingsTaskKind::Volume;
    volume_task.key = L"volume-async";
    volume_task.enabled = true;
    passed &= Report("settings async task admits one volume request",
        pulse::app::SettingsControllerTestPeer::Start(settings, volume_task,
            [&](const pulse::app::SettingsTask&, std::wstring&) {
                std::unique_lock<std::mutex> lock(task_mutex);
                task_started = true;
                task_cv.notify_one();
                task_cv.wait(lock, [&] { return release_task; });
                return true;
            }, [&](pulse::app::SettingsTaskResult result) {
                std::lock_guard<std::mutex> lock(task_mutex);
                task_result = std::move(result);
                task_done = true;
                task_cv.notify_one();
            }));
    {
        std::unique_lock<std::mutex> lock(task_mutex);
        task_cv.wait_for(lock, std::chrono::seconds(2), [&] { return task_started; });
    }
    passed &= Report("settings async task coalesces duplicate volume request",
        !pulse::app::SettingsControllerTestPeer::Start(settings, volume_task,
            [](const pulse::app::SettingsTask&, std::wstring&) { return true; },
            [](pulse::app::SettingsTaskResult) {}));
    pulse::app::SettingsTask exclude_task;
    exclude_task.kind = pulse::app::SettingsTaskKind::Exclude;
    exclude_task.path = L"C:\\excluded";
    passed &= Report("settings serializes all local index configuration",
        !pulse::app::SettingsControllerTestPeer::Start(settings, std::move(exclude_task),
            [](const pulse::app::SettingsTask&, std::wstring&) { return true; },
            [](pulse::app::SettingsTaskResult) {}));
    {
        std::lock_guard<std::mutex> lock(task_mutex);
        release_task = true;
        task_cv.notify_all();
    }
    {
        std::unique_lock<std::mutex> lock(task_mutex);
        const bool completed = task_cv.wait_for(lock, std::chrono::seconds(2),
                                                [&] { return task_done; });
        passed &= Report("settings async task invokes completion",
                         completed && task_result.ok && task_result.task.key == L"volume-async");
    }
    passed &= Report("settings async task releases volume pending state",
        !settings.VolumePending(L"volume-async"));

    task_done = false;
    pulse::app::SettingsTask failed_task;
    failed_task.kind = pulse::app::SettingsTaskKind::NetworkAdd;
    failed_task.path = L"\\\\server\\missing";
    passed &= Report("settings async task propagates operation failure",
        pulse::app::SettingsControllerTestPeer::Start(settings, std::move(failed_task),
            [](const pulse::app::SettingsTask&, std::wstring& error) {
                error = L"offline";
                return false;
            }, [&](pulse::app::SettingsTaskResult result) {
                std::lock_guard<std::mutex> lock(task_mutex);
                task_result = std::move(result);
                task_done = true;
                task_cv.notify_one();
            }));
    {
        std::unique_lock<std::mutex> lock(task_mutex);
        const bool completed = task_cv.wait_for(lock, std::chrono::seconds(2),
                                                [&] { return task_done; });
        passed &= Report("settings async task preserves failure detail",
                         completed && !task_result.ok && task_result.error == L"offline");
    }
    passed &= Report("settings async task releases network pending state",
        !settings.network_pending());

    {
        SettingsController migration_ui;
        pulse::app::SettingsTask task;
        task.kind = pulse::app::SettingsTaskKind::ConfigureIndexPath;
        task.path = L"D:\\Index";
        bool pending_seen = false;
        pulse::app::SettingsTaskResult result;
        pulse::app::SettingsControllerTestPeer::Start(migration_ui, task,
            [&](const pulse::app::SettingsTask&, std::wstring& error) {
                pending_seen = migration_ui.migration_pending();
                error = L"cleanup warning";
                return false;
            }, [&](pulse::app::SettingsTaskResult completed) { result = std::move(completed); });
        migration_ui.Stop();
        passed &= Report("migration displays pending state and clears it on failure",
            pending_seen && !migration_ui.migration_pending());
        const auto effect = migration_ui.CompleteTask(result, true);
        passed &= Report("migration failure preserves message and refreshes actual location",
            effect.refresh_index && migration_ui.error() == L"cleanup warning");
    }

    bool lifecycle_completed = false;
    {
        SettingsController lifecycle;
        pulse::app::SettingsTask task;
        task.kind = pulse::app::SettingsTaskKind::NetworkRebuild;
        pulse::app::SettingsControllerTestPeer::Start(lifecycle, std::move(task),
            [](const pulse::app::SettingsTask&, std::wstring&) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                return true;
            }, [&](pulse::app::SettingsTaskResult) { lifecycle_completed = true; });
    }
    passed &= Report("settings controller joins tasks during destruction",
        lifecycle_completed);

    passed &= TestAddressBarCommands();
    passed &= TestAddressShortcuts();
    passed &= TestSelectionTokens();
    passed &= TestShellRegistryDebounce();
    passed &= TestDefaultFileManager();
    passed &= TestShellWindowPlan();
    passed &= TestThisPc();
    passed &= Report("menu row height follows list density (28/34/40 -> 30/36/40, clamped)",
        pulse::app::MenuRowHeightDip(28) == 30 && pulse::app::MenuRowHeightDip(34) == 36 &&
        pulse::app::MenuRowHeightDip(40) == 40 && pulse::app::MenuRowHeightDip(24) == 28 &&
        pulse::app::MenuRowHeightDip(48) == 40);

    std::printf("\n== app controller tests: %s ==\n", passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
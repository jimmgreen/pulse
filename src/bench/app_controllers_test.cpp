#include "../app/settings_controller.h"
#include "../app/address_bar_command.h"
#include "../app/session.h"
#include "../app/layout_pane_selection.h"
#include "../app/network_sidebar.h"
#include "../app/context_menu_controller.h"
#include "../app/single_instance_coordinator.h"
#include "../app/tray_controller.h"
#include "../app/blank_pane_click.h"
#include "../app/shell_registry_debounce.h"
#include "../app/default_file_manager.h"
#include "../app/shell_window_plan.h"
#include "../app/explorer_shortcuts.h"
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
#include <string_view>
#include <chrono>
#include <algorithm>
#include <vector>
#include <thread>

namespace pulse::app {

std::wstring test_prefs_directory;
std::wstring GetPulseDataDir() {
    return test_prefs_directory;
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
        passed &= Report("default file manager: Recycle Bin launch arguments are recognized",
            IsRecycleBinArgument(L"::{645ff040-5081-101b-9f08-00aa002f954e}") &&
            IsRecycleBinArgument(L"\"::{645FF040-5081-101B-9F08-00AA002F954E}\"") &&
            IsRecycleBinArgument(L"shell:::{645FF040-5081-101B-9F08-00AA002F954E}") &&
            IsRecycleBinArgument(L"shell:RecycleBinFolder") &&
            !IsRecycleBinArgument(L"") && !IsRecycleBinArgument(L"C:\\$Recycle.Bin") &&
            !IsRecycleBinArgument(L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}") &&
            !IsRecycleBinArgument(L"RecycleBin") && !IsRecycleBinArgument(L"shell:"));
        AppPrefs flags;
        flags.persist = false;
        const bool off = DefaultFileManagerState(flags) == DefaultManagerState::Off;
        flags.take_over_win_e = true;
        const bool partial = DefaultFileManagerState(flags) == DefaultManagerState::Partial;
        flags.open_folders_in_pulse = flags.take_over_this_pc = true;
        const bool three_partial = DefaultFileManagerState(flags) == DefaultManagerState::Partial;
        flags.take_over_recycle_bin = true;
        passed &= Report("default file manager: state is off / partial / full from the four takeovers",
            off && partial && three_partial && DefaultFileManagerState(flags) == DefaultManagerState::Full);
        flags.take_over_this_pc = false;
        flags.open_folders_in_pulse = false;
        flags.take_over_recycle_bin = false;
        const std::wstring summary = DefaultFileManagerSummary(flags);
        passed &= Report("default file manager: a partial takeover names what Explorer still opens",
            summary.find(pulse::l10n::Get(pulse::l10n::StringId::SettingsTakeoverFolders)) != std::wstring::npos &&
            summary.find(pulse::l10n::Get(pulse::l10n::StringId::ThisPc)) != std::wstring::npos &&
            summary.find(pulse::l10n::Get(pulse::l10n::StringId::SettingsRecycleBin)) != std::wstring::npos &&
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
    {
        // Reproduce the remote pre-snapshot uninstall residue, without using
        // the current writer to construct an artificially healthy fixture.
        RegDeleteTreeW(HKEY_CURRENT_USER, L"Software");
        for (const auto* group : {L"Directory", L"Drive"}) {
            const auto shell = std::wstring(L"Software\\Classes\\") + group + L"\\shell";
            Write(shell.c_str(), nullptr, L"open");
            Write((shell + L"\\open").c_str(), L"DelegateExecute", L"");
        }
        Write(kWinECommand, L"DelegateExecute", L"");
        Write(kThisPcShell, nullptr, L"open");
        Write(kThisPcCommand, L"DelegateExecute", L"");
        wchar_t temp_path[MAX_PATH]{};
        const DWORD temp_length = GetTempPathW(MAX_PATH, temp_path);
        const auto prefs_directory = std::wstring(temp_path) + L"PulseLegacyPrefs-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        const bool prefs_directory_created = temp_length > 0 && temp_length < MAX_PATH &&
            CreateDirectoryW(prefs_directory.c_str(), nullptr);
        passed &= Report("legacy repair preferences use an isolated temporary directory", prefs_directory_created);
        if (prefs_directory_created) pulse::app::test_prefs_directory = prefs_directory;
        AppPrefs prefs;
        prefs.persist = false;
        prefs.Load();
        pulse::app::ContextMenuPrefs context;
        pulse::index::IndexClient index;
        pulse::index::NetworkAgentClient network;
        pulse::app::SettingsController settings;
        settings.BindUi(prefs, context, index, network, {});
        passed &= Report("legacy orphan is shown as incomplete and offers restoration",
            prefs.integration_residual && prefs.integration_incomplete && settings.IntegrationState() == 2 &&
            settings.IntegrationCanRestore());
        prefs.persist = true;
        settings.IntegrationAction(6);
        passed &= Report("explicit settings restore clears legacy orphan and refreshes status",
            !prefs.integration_residual && !prefs.integration_incomplete && !KeyExists(kThisPcClsid) &&
            settings.IntegrationState() == 0);
        for (const auto* group : {L"Directory", L"Drive"}) {
            const auto shell = std::wstring(L"Software\\Classes\\") + group + L"\\shell";
            Write(shell.c_str(), nullptr, L"open");
            Write((shell + L"\\open").c_str(), L"DelegateExecute", L"");
        }
        Write(kWinECommand, L"DelegateExecute", L"");
        Write(kThisPcShell, nullptr, L"open");
        Write(kThisPcCommand, L"DelegateExecute", L"");
        AppPrefs migrated;
        migrated.Load();
        passed &= Report("startup automatically backs up and repairs the confirmed remote legacy signature",
            !migrated.integration_residual && !migrated.integration_incomplete && !KeyExists(kThisPcClsid) &&
            SnapshotExists(L"LegacyOrphanRepair"));
        migrated.Load();
        passed &= Report("repeated startup leaves repaired associations and backup intact",
            !migrated.integration_residual && !KeyExists(kThisPcClsid) && SnapshotExists(L"LegacyOrphanRepair"));
        pulse::app::test_prefs_directory.clear();
        if (prefs_directory_created) {
            const bool removed_file = DeleteFileW((prefs_directory + L"\\app.json").c_str()) != FALSE;
            const bool removed_directory = RemoveDirectoryW(prefs_directory.c_str()) != FALSE;
            passed &= Report("isolated legacy repair preferences are removed", removed_file && removed_directory);
        }
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
    auto step = [](unsigned age, bool ready, bool supported, size_t selected, unsigned view_age = 0) {
        ExplorerWindowProbe probe;
        probe.age_ms = age;
        probe.view_age_ms = view_age;
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
    passed &= Report("explorer takeover: a selection is taken at once, a plain folder soon after its view",
        step(50, true, true, 1) == S::Take && step(5000, true, true, 0, 0) == S::Wait &&
        step(50, true, true, 0, kExplorerSelectionGraceMs - 1) == S::Wait &&
        step(50, true, true, 0, kExplorerSelectionGraceMs) == S::Take);

    using C = SourceCloseStep;
    auto close = [](bool identity, bool single_tab, bool hidden, bool changed, bool sent, unsigned view_age) {
        SourceCloseProbe probe;
        probe.identity_kept = identity;
        probe.single_tab = single_tab;
        probe.hidden = hidden;
        probe.selection_changed = changed;
        probe.sent_selection = sent;
        probe.view_age_ms = view_age;
        return DecideSourceClose(probe);
    };
    passed &= Report("explorer takeover: a source with another tab or a changed identity is never closed",
        close(true, false, true, false, true, 5000) == C::Abort &&
        close(true, false, false, false, true, 5000) == C::Abort &&
        close(false, true, true, false, true, 5000) == C::Abort);
    passed &= Report("explorer takeover: a hidden source waits for a late /select, then closes",
        close(true, true, true, false, false, 0) == C::Wait &&
        close(true, true, true, false, false, kExplorerLateSelectionMs - 1) == C::Wait &&
        close(true, true, true, false, false, kExplorerLateSelectionMs) == C::Close &&
        close(true, true, true, false, true, 0) == C::Close);
    passed &= Report("explorer takeover: a late selection follows a hidden source; a visible one is left",
        close(true, true, true, true, false, 300) == C::CloseAndSelect &&
        close(true, true, false, true, false, kExplorerProgrammaticSelectionMs) == C::Abort);
    passed &= Report("explorer takeover: a visible source's late /select still follows (WeChat)",
        close(true, true, false, true, false, 300) == C::CloseAndSelect &&
        close(true, true, false, true, false, kExplorerProgrammaticSelectionMs - 1) == C::CloseAndSelect);
    passed &= Report("explorer takeover: a visible single-tab source closes without waiting (Windows 11 too)",
        close(true, true, false, false, false, 0) == C::Close);
    passed &= Report("explorer takeover: setting has a label and description",
        !pulse::l10n::Get(pulse::l10n::StringId::SettingsExplorerWindows).empty() &&
        !pulse::l10n::Get(pulse::l10n::StringId::SettingsExplorerWindowsDesc).empty());
    return passed;
}

bool TestExplorerShortcuts() {
    using pulse::app::ExplorerShortcut;
    using pulse::app::NthVisibleTab;
    using A = pulse::app::ExplorerKeyAction;
    auto is = [](UINT key, bool ctrl, bool shift, bool alt, A action, int index = 0) {
        const auto command = ExplorerShortcut(key, ctrl, shift, alt);
        return command.action == action && command.index == index;
    };
    bool passed = true;
    passed &= Report("explorer keys: Ctrl+D deletes, Ctrl+Shift+D permanently, Ctrl+Alt+D marks the target",
        is(L'D', true, false, false, A::Delete) && !ExplorerShortcut(L'D', true, false, false).shift &&
        ExplorerShortcut(L'D', true, true, false).shift && is(L'D', true, false, true, A::MarkTarget));
    passed &= Report("explorer keys: Ctrl+Shift+N new folder, Ctrl+E / F3 search, Ctrl+N untouched",
        is(L'N', true, true, false, A::NewFolder) && is(L'E', true, false, false, A::Search) &&
        is(VK_F3, false, false, false, A::Search) && is(L'N', true, false, false, A::None));
    passed &= Report("explorer keys: Ctrl+1..9 tabs, Ctrl+Alt+1..4 layouts, Ctrl+Alt+5 nothing",
        is(L'1', true, false, false, A::TabNumber, 0) && is(L'9', true, false, false, A::TabNumber, 8) &&
        is(L'1', true, false, true, A::Layout, 0) && is(L'4', true, false, true, A::Layout, 3) &&
        is(L'5', true, false, true, A::None));
    passed &= Report("explorer keys: Ctrl+Shift+1..8 views in Explorer order, 9 nothing",
        is(L'1', true, true, false, A::ViewMode, 0) && is(L'6', true, true, false, A::ViewMode, 5) &&
        is(L'8', true, true, false, A::ViewMode, 7) && is(L'9', true, true, false, A::None) &&
        pulse::ui::ViewModeFromIndex(5) == pulse::ui::ViewMode::Details &&
        pulse::ui::ViewModeFromIndex(1) == pulse::ui::ViewMode::LargeIcons);
    passed &= Report("explorer keys: Ctrl+Alt+Shift+1..7 tags",
        is(L'1', true, true, true, A::Tag, 0) && is(L'7', true, true, true, A::Tag, 6) &&
        is(L'8', true, true, true, A::None));
    passed &= Report("explorer keys: Backspace back, Shift+F10 / Apps menu, F11 maximize; plain keys untouched",
        is(VK_BACK, false, false, false, A::Back) && is(VK_F10, false, true, false, A::ContextMenu) &&
        is(VK_APPS, false, false, false, A::ContextMenu) && is(VK_F11, false, false, false, A::Maximize) &&
        is(VK_F10, false, false, false, A::None) && is(L'D', false, false, false, A::None) &&
        is(VK_UP, false, false, true, A::None) && is(VK_TAB, true, false, false, A::None));
    const std::vector<size_t> visible{0, 2, 3};
    passed &= Report("explorer keys: Ctrl+N picks the Nth visible tab, Ctrl+9 the last",
        NthVisibleTab(visible, 0) == size_t{0} && NthVisibleTab(visible, 1) == size_t{2} &&
        !NthVisibleTab(visible, 5) && NthVisibleTab(visible, 8) == size_t{3} && !NthVisibleTab({}, 8));
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

static bool TestLayoutActivePane() {
    using namespace pulse::app;
    LayoutTab tab;
    for (const auto* path : {L"C:\\first", L"C:\\second", L"C:\\third"}) {
        auto pane = std::make_unique<Pane>();
        pane->view.current_path = path;
        tab.panes.push_back(std::move(pane));
    }
    auto* first = tab.panes[0].get();
    auto* second = tab.panes[1].get();
    auto* third = tab.panes[2].get();
    third->view.back_stack.push(L"C:\\previous");
    third->view.selected_index = 7;
    third->view.selected.insert(7);
    third->view.scroll_y = 125.0f;
    third->view.filter_text = L"report";
    third->view.view_mode = pulse::ui::ViewMode::Details;
    third->view.details_column_dividers[0] = 245.0f;
    bool ok = true;
    auto visible = SelectLayoutPanes(tab.panes, third, 2, {first, second, third});
    ok &= Report("layout: three to two retains the third active pane and order",
        visible == std::vector<Pane*>{first, third});
    visible = SelectLayoutPanes(tab.panes, first, 2, visible);
    ok &= Report("layout: changing orientation retains the visible pair after focus moves",
        visible == std::vector<Pane*>{first, third});
    visible = SelectLayoutPanes(tab.panes, third, 1, visible);
    ok &= Report("layout: shrinking to one retains the active pane",
        visible == std::vector<Pane*>{third});
    visible = SelectLayoutPanes(tab.panes, third, 2, visible);
    ok &= Report("layout: re-expanding restores the original pair order",
        visible == std::vector<Pane*>{first, third});
    visible = SelectLayoutPanes(tab.panes, third, 3, visible);
    ok &= Report("layout: full expansion restores every original pane",
        visible == std::vector<Pane*>{first, second, third} &&
        tab.panes[0].get() == first && tab.panes[1].get() == second &&
        tab.panes[2].get() == third);
    ok &= Report("layout: existing folder view state survives contraction and expansion",
        third->view.current_path == L"C:\\third" &&
        third->view.back_stack.top() == L"C:\\previous" &&
        third->view.selected_index == 7 && third->view.selected.contains(7) &&
        third->view.scroll_y == 125.0f && third->view.filter_text == L"report" &&
        third->view.details_column_dividers[0] == 245.0f);

    tab.focused_index = 2;
    tab.target_index = 1;
    tab.layout = LayoutPreset::TwoVertical;
    tab.root = std::make_unique<SplitContainer>();
    tab.root->is_leaf = false;
    tab.root->first = std::make_unique<SplitContainer>();
    tab.root->first->pane = first;
    tab.root->second = std::make_unique<SplitContainer>();
    tab.root->second->pane = third;
    auto saved = CaptureLayoutTab(tab);
    ok &= Report("layout: snapshot remaps active pane and drops hidden target",
        saved.panes.size() == 2 && saved.panes[1].path == L"C:\\third" &&
        saved.focused == 1 && saved.target == -1);
    tab.target_index = 2;
    saved = CaptureLayoutTab(tab);
    ok &= Report("layout: snapshot remaps visible target", saved.target == 1);
    LayoutTab restored;
    RestoreLayoutTab(restored, saved, [](Tab& view, const std::wstring& path) {
        view.current_path = path;
    });
    ok &= Report("layout: session restores the active view and target",
        restored.focused_index == 1 && restored.target_index == 1 &&
        restored.panes[1]->focused && restored.panes[1]->target &&
        restored.panes[1]->view.current_path == L"C:\\third");

    tab.layout = LayoutPreset::Single;
    tab.root = std::make_unique<SplitContainer>();
    tab.root->pane = third;
    saved = CaptureLayoutTab(tab);
    ok &= Report("layout: single-pane snapshot uses valid focus and target indices",
        saved.panes.size() == 1 && saved.panes[0].path == L"C:\\third" &&
        saved.focused == 0 && saved.target == 0);
    tab.root.reset();
    tab.panes.pop_back();
    visible = SelectLayoutPanes(tab.panes, second, 1, {first, second});
    visible = SelectLayoutPanes(tab.panes, second, 2, visible);
    ok &= Report("layout: two to one to two retains both pane objects and their order",
        visible == std::vector<Pane*>{first, second});
    ok &= Report("layout: absent focus and empty ownership have valid fallbacks",
        SelectLayoutPanes(tab.panes, nullptr, 1) == std::vector<Pane*>{first} &&
        SelectLayoutPanes({}, nullptr, 1).empty());
    return ok;
}

bool TestNetworkLocationsView() {
    using namespace pulse::app;
    SidebarModel sidebar;
    SidebarEntry duplicate;
    duplicate.label = L"System duplicate";
    duplicate.path = L"\\\\SERVER\\share\\";
    sidebar.system_networks.push_back(duplicate);
    SidebarEntry imported;
    imported.label = L"System location";
    imported.path = L"\\\\offline.invalid\\other";
    sidebar.system_networks.push_back(imported);
    pulse::ui::SidebarGroup group;
    pulse::ui::SidebarItem pin;
    pin.label = L"My saved name";
    pin.path = L"\\\\server\\share";
    group.items.push_back(pin);
    AppendSystemNetworkLocations(group, sidebar.system_networks);
    bool ok = Report("network view: section title opens its own view",
        group.navigable && group.navigation_path == L"pulse:networks");
    ok &= Report("network view: pinned name wins over imported duplicate",
        group.items.size() == 2 && group.items[0].label == L"My saved name");
    ok &= Report("network view: imported offline location is displayed without probing",
        group.items.back().path == imported.path && !group.items.back().status_dot);
    AppendSystemNetworkLocations(group, sidebar.system_networks);
    ok &= Report("network view: repeated merges do not duplicate locations", group.items.size() == 2);
    return ok;
}

bool TestStaticMenuIdentity() {
    using namespace pulse::app;
    bool ok = true;
    for (int scenario = 0; scenario != 5; ++scenario) {
        ContextMenuController controller;
        ContextMenuPrefs prefs;
        prefs.persist = false;
        std::wstring invoked;
        ContextMenuController::ShellOperations operations;
        operations.query = [](auto, HWND, bool, bool, auto) { return 17u; };
        operations.execute_command = [&](const auto& command, const auto&) { invoked = command; };
        controller.SetShellOperations(std::move(operations));
        StaticVerb a{L"a", L"A", L"", L"cmd-a", {}};
        StaticVerb b{L"b", L"B", L"", L"cmd-b", {}};
        StaticVerb parent{L"parent", L"Parent", L"", L"", {a,b}};
        const bool cascade = scenario == 1;
        controller.CompleteStaticVerbs(L".txt", cascade ? std::vector<StaticVerb>{parent} :
            std::vector<StaticVerb>{a,b}, controller.cache_generation());
        controller.StartQuery(prefs, nullptr, {L"C:\\one.txt"}, false, L".txt", false,
            [](const auto& path) { return path; }, {});
        controller.OpenMenu({});
        parent.children = {b,a};
        controller.CompleteStaticVerbs(L".txt", cascade ? std::vector<StaticVerb>{parent} :
            scenario == 2 ? std::vector<StaticVerb>{b} : std::vector<StaticVerb>{b,a},
            controller.cache_generation());
        if (scenario == 3) controller.NotePatchedDisplay();
        if (scenario == 4) controller.CloseMenu();
        controller.ExecuteShellCommand(CmdShellStaticBase + (cascade ? 1 : 0), {}, {});
        const auto expected = scenario == 2 ? L"" : scenario == 3 ? L"cmd-b" : L"cmd-a";
        ok &= Report("static menu displayed identity survives asynchronous refresh", invoked == expected);
    }
    return ok;
}

// Explorer parity (.dwg report): Explorer's own 打开方式 flyout replaces the
// static row once the host delivers it, and the 打开方式 switch governs it.
bool TestOpenWithFlyoutReplacesStatic() {
    using namespace pulse::app;
    bool ok = true;
    ContextMenuController controller;
    ContextMenuPrefs prefs;
    prefs.persist = false;
    prefs.ResetToDefaults();
    ContextMenuController::ShellOperations operations;
    operations.query = [](auto, HWND, bool, bool, auto) { return 17u; };
    controller.SetShellOperations(std::move(operations));
    StaticVerb edit{L"edit", L"Edit", L"", L"cmd-edit", {}};
    StaticVerb open_as{L"openas", L"Open with...", L"", L"", {}};
    controller.CompleteStaticVerbs(L".dwg", {edit, open_as}, controller.cache_generation());
    controller.StartQuery(prefs, nullptr, {L"C:\\a.dwg"}, false, L".dwg", false,
        [](const auto& path) { return path; }, {});
    auto find = [](const std::vector<pulse::ui::FluentMenuItem>& rows,
                   const std::wstring& text) -> const pulse::ui::FluentMenuItem* {
        for (const auto& row : rows)
            if (row.text == text) return &row;
        return nullptr;
    };
    bool changed = false;
    auto rows = controller.BuildDisplay(prefs, {}, changed);
    ok &= Report("static Open with row is the fallback before the COM flyout arrives",
        find(rows, L"Open with...") != nullptr && find(rows, L"Edit") != nullptr);

    const std::wstring open_with_clsid = L"{09799afb-ad67-11d1-abcd-00c04fc30936}";
    pulse::ops::ShellMenuItem header, app, pick;
    header.has_children = true;
    header.verb = L"openas";
    header.text = L"Open with(H)";
    header.clsid = open_with_clsid;
    app.id = 300;
    app.child = true;
    app.text = L"CAD Viewer";
    app.clsid = open_with_clsid;
    pick.id = 301;
    pick.child = true;
    pick.verb = L"openas";
    pick.text = L"Choose another app(C)";
    pick.clsid = open_with_clsid;
    controller.CompleteComQuery(17, {header, app, pick}, GetTickCount64());
    rows = controller.BuildDisplay(prefs, {}, changed);
    const auto* flyout = find(rows, L"Open with(H)");
    ok &= Report("Explorer's Open with flyout replaces the static row, apps and picker kept",
        find(rows, L"Open with...") == nullptr && flyout && flyout->children.size() == 2 &&
        flyout->children[0].text == L"CAD Viewer" && find(rows, L"Edit") != nullptr);
    bool seen_local = false;
    for (const auto& item : prefs.seen)
        if (item.key == pulse::ipc::HandlerCatalogKey(open_with_clsid))
            seen_local = !item.from_com && item.category == pulse::ipc::CtxMenuCategory::OpenWith;
    ok &= Report("the Open with flyout is catalogued under the Open with switch", seen_local);
    prefs.open_with = false;
    rows = controller.BuildDisplay(prefs, {}, changed);
    ok &= Report("turning Open with off hides the COM flyout too",
        find(rows, L"Open with(H)") == nullptr && find(rows, L"Open with...") == nullptr);
    return ok;
}

// Prefs v1 -> v2: verdicts of the old "everyone still running after 1 s"
// rule are dropped, the user's explicit switches stay, per-type timings go.
bool TestHungPrefsMigration() {
    using namespace pulse::app;
    bool ok = true;
    const std::wstring healthy = L"{bbbbbbbb-0000-0000-0000-000000000002}";
    const std::wstring user_off = L"{aaaaaaaa-0000-0000-0000-000000000001}";
    const std::wstring open_with_key = L"h:{09799afb-ad67-11d1-abcd-00c04fc30936}";
    const std::wstring v1 =
        L"{\"version\":1,\"explorer_cap\":32,\"open_with_mru\":2,"
        L"\"categories\":{\"software\":true,\"open_with\":true,\"open_with_com\":false},"
        L"\"items\":{\"h:" + user_off + L"\":{\"enabled\":false}},"
        L"\"seen\":[{\"key\":\"" + open_with_key + L"\",\"text\":\"Open with\",\"kind\":\"flyout\","
        L"\"category\":\"open_with\",\"source\":\"com\"}],"
        L"\"slow_ext\":{\"h:" + healthy + L"\":{\"ms\":1000,\"slow\":0,\"timeout\":3,"
        L"\"deferred\":true,\"disabled\":true},"
        L"\".dwg\":{\"ms\":5047,\"slow\":24,\"timeout\":15,\"deferred\":true,\"disabled\":true}}}";
    ContextMenuPrefs prefs;
    prefs.persist = false;
    ok &= Report("v1 context menu prefs load and are flagged for migration",
        prefs.FromJson(v1) && prefs.migrated);
    ok &= Report("old auto-disable verdict is cleared for a healthy handler",
        prefs.HandlerEnabled(healthy) && !prefs.ComDisabled(L"h:" + healthy));
    ok &= Report("explicit user switch survives the migration", !prefs.HandlerEnabled(user_off));
    ok &= Report("unused per-type timings are dropped", prefs.slow_ext.find(L".dwg") == prefs.slow_ext.end());
    bool open_with_row = false;
    for (const auto& item : prefs.seen)
        if (item.key == open_with_key)
            open_with_row = prefs.RowEnabled(item.key, item.category, item.from_com);
    ok &= Report("catalogued Open with flyout shows as on in settings", open_with_row);
    ContextMenuPrefs reloaded;
    reloaded.persist = false;
    const std::wstring v2 = prefs.ToJson();
    ok &= Report("migrated prefs are written as version 2 and reload without migrating",
        v2.find(L"\"version\":2") != std::wstring::npos && reloaded.FromJson(v2) &&
        !reloaded.migrated && reloaded.HandlerEnabled(healthy) && !reloaded.HandlerEnabled(user_off));
    const std::wstring hung = L"h:{cccccccc-0000-0000-0000-000000000003}";
    reloaded.RecordComTiming(hung, 5000);
    reloaded.RecordComTiming(hung, 5000);
    const bool two = reloaded.ComDisabled(hung);
    reloaded.RecordComTiming(hung, 5000);
    ok &= Report("a handler the host reports hung three times is disabled",
        !two && reloaded.ComDisabled(hung));
    return ok;
}

bool TestComMenuIdentity() {
    using namespace pulse::app;
    bool ok = true;
    for (int scenario = 0; scenario < 3; ++scenario) {
        ContextMenuController controller;
        ContextMenuPrefs prefs;
        prefs.persist = false;
        uint32_t invoked = 0;
        std::wstring invoked_text;
        ContextMenuController::ShellOperations operations;
        operations.query = [](auto, HWND, bool, bool, auto) { return 17u; };
        operations.invoke = [&](uint32_t, uint32_t id, auto, auto text) {
            invoked = id;
            invoked_text = text;
        };
        controller.SetShellOperations(std::move(operations));
        controller.StartQuery(prefs, nullptr, {L"C:\\fake.txt"}, false, L".txt", false, {}, {});
        pulse::ops::ShellMenuItem a, b;
        a.id = 10; a.text = L"A"; a.verb = L"a"; a.child = true;
        b.id = 11; b.text = L"B"; b.verb = L"b"; b.child = true;
        controller.CompleteComQuery(17, {a,b}, GetTickCount64(), true);
        controller.OpenMenu({});
        a.id = scenario == 0 ? 11 : 20;
        b.id = 10;
        controller.CompleteComQuery(17, scenario == 2 ? std::vector<pulse::ops::ShellMenuItem>{b} :
            std::vector<pulse::ops::ShellMenuItem>{a,b}, GetTickCount64());
        controller.ExecuteShellCommand(CmdShellComBase + 10, {}, {});
        ok &= Report("COM displayed A survives partial-to-full ID swap/shift or safely rejects removal",
            scenario == 2 ? invoked == 0 : invoked == a.id && invoked_text == L"A");
    }
    return ok;
}

bool TestMenuQueryIdentity() {
    using namespace pulse::app;
    bool ok = true;
    for (bool complete : {false, true}) {
        ContextMenuController controller;
        ContextMenuPrefs prefs;
        prefs.persist = false;
        uint32_t token = 0, closed = 0;
        std::vector<bool> flags;
        ContextMenuController::ShellOperations operations;
        operations.query = [&](auto, HWND, bool, bool extended, auto) {
            flags.push_back(extended); return ++token;
        };
        operations.close = [&](uint32_t old) { closed = old; };
        controller.SetShellOperations(std::move(operations));
        auto start = [&](bool extended) {
            controller.StartQuery(prefs, nullptr, {L"C:\\fake.txt"}, false, L".txt", extended, {}, {});
        };
        start(false);
        if (complete) controller.CompleteComQuery(token, {}, GetTickCount64());
        start(true);
        ok &= Report("Shift open replaces pending/completed normal prefetch and rejects stale reply",
            token == 2 && closed == 1 && !controller.CompleteComQuery(1, {}, GetTickCount64()).accepted);
        if (complete) controller.CompleteComQuery(token, {}, GetTickCount64());
        start(false);
        start(false);
        ok &= Report("normal open replaces extended prefetch; matching reopen reuses session",
            token == 3 && closed == 2 && flags == std::vector<bool>{false,true,false});
        prefs.SetItemEnabled(pulse::ipc::HandlerCatalogKey(L"{test-handler}"), false);
        start(false);
        ok &= Report("changed handler configuration replaces prefetch", token == 4 && closed == 3);
    }
    return ok;
}

bool TestViewHitBounds() {
    using namespace pulse::ui;
    bool ok = true;
    ViewLayout xl(ViewMode::ExtraLargeIcons, {0,0,900,900}, 20, 0, 0, 1);
    ViewLayout list(ViewMode::List, {0,0,800,250}, 30, 0, 0, 1);
    ok &= Report("XL right gutter and List bottom gutter reject neighboring items",
        xl.HitTest(850,20) == -1 && list.HitTest(30,245) == -1);
    bool contained = true;
    for (int mode = 0; mode < 8; ++mode) {
        for (float scale : {1.0f,1.5f,2.0f}) {
            for (size_t count : {size_t{0},size_t{1},size_t{20},size_t{37}}) {
                for (float scroll : {0.0f,123.0f}) {
                    ViewLayout layout(ViewModeFromIndex(mode), {13,17,914,368}, count, scroll, scroll, scale);
                    for (float y = 17; y < 368; y += 7) {
                        for (float x = 13; x < 914; x += 11) {
                            const int hit = layout.HitTest(x,y);
                            if (hit < 0) continue;
                            const auto rect = layout.ItemRect(hit);
                            contained &= x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
                        }
                    }
                }
            }
        }
    }
    return Report("eight views, DPI, partial rows, empty lists and scrolling hit only containing rectangles", contained) && ok;
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring_view(argv[1]) == L"--context-query-key") {
        pulse::app::ContextMenuController controller;
        pulse::app::ContextMenuPrefs prefs;
        prefs.persist = false;
        uint32_t issued = 0;
        std::vector<uint32_t> closed;
        std::vector<bool> modes;
        std::vector<std::wstring> disabled;
        controller.SetShellOperations({
            [&](std::vector<std::wstring>, HWND, bool, bool extended, std::vector<std::wstring> handlers) {
                modes.push_back(extended); disabled = std::move(handlers); return ++issued;
            },
            [&](uint32_t token) { closed.push_back(token); }, {}, {}, {}
        });
        auto start = [&](bool extended, const wchar_t* path = L"C:\\private-no-access.txt") {
            controller.StartQuery(prefs, nullptr, {path}, false, L".txt", extended, {}, {});
        };
        bool ok = true;
        start(false); start(false);
        ok &= Report("same hover query reuses its pending session", issued == 1 && closed.empty());
        start(true);
        ok &= Report("Shift open replaces non-extended hover session", issued == 2 && modes.back() && closed == std::vector<uint32_t>{1});
        ok &= Report("old hover response cannot populate extended session",
            !controller.CompleteComQuery(1, {}, GetTickCount64()).accepted);
        ok &= Report("current extended response is accepted",
            controller.CompleteComQuery(2, {}, GetTickCount64()).accepted);
        start(true);
        ok &= Report("completed query with unchanged mode remains reusable", issued == 2);
        start(false);
        ok &= Report("ordinary open replaces completed extended query", issued == 3 && !modes.back() && closed.back() == 2);
        const auto handler = pulse::ipc::HandlerCatalogKey(L"{00000000-0000-0000-0000-000000000001}");
        prefs.SetItemEnabled(handler, false);
        start(false);
        ok &= Report("disabled-handler configuration is sent through a fresh session",
            issued == 4 && disabled.size() == 1 && closed.back() == 3);
        ok &= Report("old configuration completion is rejected",
            !controller.CompleteComQuery(3, {}, GetTickCount64()).accepted);
        start(false);
        ok &= Report("unchanged handler set reuses its session", issued == 4);
        prefs.SetItemEnabled(handler, true);
        start(false);
        ok &= Report("enabling the handler opens a new session", issued == 5 && disabled.empty());
        start(false, L"C:\\different-private-path.txt");
        ok &= Report("changed selection still opens a new session", issued == 6);
        controller.Close(); start(false);
        ok &= Report("closed session is never reused on reopen", issued == 7);
        controller.Close();
        return ok ? 0 : 1;
    }

    if (argc == 2 && std::wstring(argv[1]) == L"--view-hit-bounds") return TestViewHitBounds() ? 0 : 1;
    if (argc == 2 && std::wstring(argv[1]) == L"--com-menu-identity") return TestComMenuIdentity() ? 0 : 1;
    if (argc == 2 && std::wstring(argv[1]) == L"--explorer-parity") {
        const bool open_with = TestOpenWithFlyoutReplacesStatic();
        const bool migration = TestHungPrefsMigration();
        return open_with && migration ? 0 : 1;
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--menu-query-identity") return TestMenuQueryIdentity() ? 0 : 1;
    if (argc == 2 && std::wstring(argv[1]) == L"--network-locations-view")
        return TestNetworkLocationsView() ? 0 : 1;
    if (argc == 2 && std::wstring(argv[1]) == L"--layout-active-pane")
        return TestLayoutActivePane() ? 0 : 1;
    if (argc == 2 && std::wstring(argv[1]) == L"--audit-identity") return TestStaticMenuIdentity() ? 0 : 1;
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
            prefs.take_over_recycle_bin && settings.IntegrationState() == 1);
        settings.IntegrationAction(7);
        ok &= Report("Recycle Bin scope toggles on its own",
            !prefs.integration_recycle_bin && !prefs.take_over_recycle_bin && prefs.take_over_this_pc &&
            settings.IntegrationState() == 1);
        settings.IntegrationAction(7);
        ok &= Report("Recycle Bin scope turns back on",
            prefs.integration_recycle_bin && prefs.take_over_recycle_bin && settings.IntegrationState() == 1);
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
            !loaded.integration_this_pc && !loaded.integration_recycle_bin);
        AppPrefs before_recycle;
        before_recycle.FromJson(L"{\"integration_enabled\":true,\"integration_folders\":true,"
            L"\"integration_win_e\":true,\"integration_this_pc\":true}");
        before_recycle.open_folders_in_pulse = before_recycle.take_over_win_e = before_recycle.take_over_this_pc = true;
        before_recycle.MigrateIntegration();
        SettingsController before_settings;
        before_settings.BindUi(before_recycle, context, index, network, {});
        ok &= Report("preferences saved before the Recycle Bin scope stay complete",
            !before_recycle.integration_recycle_bin && before_settings.IntegrationState() == 1);
        AppPrefs fresh;
        fresh.FromJson(L"{}");
        ok &= Report("new preferences select the Recycle Bin scope", fresh.integration_recycle_bin);
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
        {
            // Handler rows are keyed "h:{CLSID}" and texts are free-form: a
            // brace or quote inside them must not end "items" or "seen" early.
            pulse::app::ContextMenuPrefs handlers;
            handlers.persist = false;
            handlers.ResetToDefaults();
            const std::wstring first = pulse::ipc::HandlerCatalogKey(L"{11111111-2222-3333-4444-555555555555}");
            const std::wstring second = pulse::ipc::HandlerCatalogKey(L"{66666666-7777-8888-9999-AAAAAAAAAAAA}");
            handlers.RecordSeen(first, L"Ext } menu \"one\"", true, pulse::ipc::CtxMenuCategory::Software, true);
            handlers.RecordSeen(second, L"Ext ] two", false, pulse::ipc::CtxMenuCategory::Software, true);
            handlers.SetItemEnabled(first, false);
            handlers.slow_ext[L"{11111111-2222-3333-4444-555555555555}"].deferred = true;
            pulse::app::ContextMenuPrefs reloaded;
            reloaded.persist = false;
            reloaded.FromJson(handlers.ToJson());
            auto has_seen = [&](const std::wstring& key) {
                for (const auto& item : reloaded.seen)
                    if (item.key == key) return true;
                return false;
            };
            passed &= Report("handler catalog rows with brace keys survive reload",
                has_seen(first) && has_seen(second));
            const auto off = reloaded.item_enabled.find(first);
            passed &= Report("handler item switches survive reload",
                off != reloaded.item_enabled.end() && !off->second);
            const auto slow = reloaded.slow_ext.find(L"{11111111-2222-3333-4444-555555555555}");
            passed &= Report("slow extension state keyed by CLSID survives reload",
                slow != reloaded.slow_ext.end() && slow->second.deferred);
        }
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
    passed &= Report("double-click tab close setting has localized text",
        pulse::l10n::Get(pulse::l10n::StringId::SettingsCloseTabDoubleClick) ==
            L"\u53CC\u51FB\u6807\u7B7E\u9875\u5173\u95ED" &&
        !pulse::l10n::Get(pulse::l10n::StringId::SettingsCloseTabDoubleClickDesc).empty());
    passed &= Report("double-clicking a tab does nothing by default",
        !prefs.close_tab_on_double_click);
    settings_ui.ToggleUi(35);
    passed &= Report("double-click tab close enables and persists",
        prefs.close_tab_on_double_click && parsed_prefs.FromJson(prefs.ToJson()) &&
        parsed_prefs.close_tab_on_double_click);
    settings_ui.ToggleUi(35);
    passed &= Report("double-click tab close disables and persists",
        !prefs.close_tab_on_double_click && parsed_prefs.FromJson(prefs.ToJson()) &&
        !parsed_prefs.close_tab_on_double_click);
    parsed_prefs.close_tab_on_double_click = true;
    passed &= Report("legacy preferences keep double-click tab close off",
        parsed_prefs.FromJson(L"{}") && !parsed_prefs.close_tab_on_double_click);
    parsed_prefs.close_tab_on_double_click = true;
    parsed_prefs.ResetToDefaults();
    passed &= Report("reset turns double-click tab close off",
        !parsed_prefs.close_tab_on_double_click);
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
        !pulse::l10n::Get(pulse::l10n::StringId::ListSelectionOutlineDesc).empty() &&
        !pulse::l10n::Get(pulse::l10n::StringId::ListThumbnailBadges).empty() &&
        !pulse::l10n::Get(pulse::l10n::StringId::ListThumbnailBadgesDesc).empty());
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
    {
        int tab_a = 0, tab_b = 0;
        passed &= Report("double-click closes the tab both presses landed on",
            pulse::app::TabDoubleClickCloses(true, &tab_a, &tab_a, 200, 500) &&
            !pulse::app::TabDoubleClickCloses(false, &tab_a, &tab_a, 200, 500));
        passed &= Report("double-click after a close-button press keeps the next tab",
            !pulse::app::TabDoubleClickCloses(true, nullptr, &tab_b, 200, 500) &&
            !pulse::app::TabDoubleClickCloses(true, &tab_a, &tab_b, 200, 500));
        passed &= Report("double-click tab close honors the system double-click time",
            pulse::app::TabDoubleClickCloses(true, &tab_a, &tab_a, 500, 500) &&
            !pulse::app::TabDoubleClickCloses(true, &tab_a, &tab_a, 501, 500));
    }
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
    com_item.mnemonic = L'U';
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
    {
        bool com_key = false;
        for (const auto& row : display)
            com_key = com_key || (row.text == L"Custom action" && row.mnemonic == L'U');
        passed &= Report("context menu keeps the Explorer access key on merged COM rows", com_key);
        std::vector<pulse::ui::FluentMenuItem> rows;
        pulse::app::ShellMenuEntry header;
        header.text = L"Send to";
        header.mnemonic = L'N';
        pulse::app::ShellMenuEntry child;
        child.command = 8101;
        child.text = L"Desktop";
        child.mnemonic = L'D';
        header.children.push_back(child);
        pulse::app::ShellMenuEntry flat;
        flat.command = 8000;
        flat.text = L"Edit";
        flat.mnemonic = L'E';
        pulse::app::AppendShellSection(rows, { header, flat });
        passed &= Report("Explorer section rows and flyout children keep access keys",
            rows.size() == 2 && rows[0].mnemonic == L'N' && rows[0].children.size() == 1 &&
            rows[0].children[0].mnemonic == L'D' && rows[1].mnemonic == L'E');
    }
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

    passed &= TestExplorerShortcuts();
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

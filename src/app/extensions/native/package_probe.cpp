#include "package_probe.h"
#include "author_probe.h"
#include "preview_probe.h"
#include "task_probe.h"
#include "staging_probe.h"
#include "compression_probe.h"
#include "settings_pages_probe.h"
#include "session.h"
#include "ui_adapter.h"
#include "ui_probe.h"
#include "../runtime.h"
#include "package_commands.h"
#include "command_contributions.h"
#include "../../app_runtime.h"
#include "../../app_commands.h"
#include "../../../ui/ui_renderer.h"
#include <filesystem>
#include <fstream>
#include <functional>

namespace pulse::extensions::native {
namespace {
bool Wait(const std::function<bool()>& ready) {
    const auto deadline = GetTickCount64() + 10000;
    do {
        MSG message{};
        for (unsigned i = 0; i < 32 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++i) {
            if (message.message == WM_QUIT) { PostQuitMessage(static_cast<int>(message.wParam)); return false; }
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        if (ready()) return true;
        MsgWaitForMultipleObjectsEx(0, nullptr, 5, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    } while (GetTickCount64() < deadline);
    return false;
}
std::wstring Text(HWND window) {
    if (!window) return {};
    std::wstring value(static_cast<size_t>(GetWindowTextLengthW(window)) + 1, wchar_t{});
    value.resize(static_cast<size_t>(GetWindowTextW(window, value.data(), static_cast<int>(value.size()))));
    return value;
}
}
bool RunPackageProbe(AppState& owner, const std::wstring& directory) {
    wchar_t profile[32768]{};
    const auto length = GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", profile, 32768);
    if (!owner.isolatedTest || !PackageStoreMode() || !length || length >= 32768) return false;
    const std::filesystem::path output(directory);
    const auto relative = output.lexically_normal().lexically_relative(std::filesystem::path(profile).lexically_normal());
    if (relative.empty() || relative.is_absolute() || *relative.begin() == L".." || relative == L".") return false;
    std::filesystem::create_directories(output);
    if (PackageCount() == 1 && PackageManifest(0)->id == "independent.number-lens")
        return RunAuthorProbe(owner, directory);
    if (PackageCount() == 1 && PackageManifest(0)->id == "sample.native-archive") {
        std::ofstream report(output / "report.txt");
        const auto entries = EntriesForPackage(0, L"");
        bool passed = entries.size() >= 2 && HandlePackageCommand(owner, entries.front().command) && PackageActive(owner.hwnd, 0);
        if (!passed) report << "Native initialization status: " << PackageLastStatus(owner.hwnd, 0) << '\n';
        auto* panel = PackageUi(owner.hwnd, 0);
        const auto status = panel ? Text(panel->NodeWindow("engine-status")) : std::wstring{};
        passed = passed && status.starts_with(L"7-Zip engine loaded. Supported formats: ") && !status.ends_with(L": 0");
        report << (passed ? "[PASS]" : "[FAIL]") << " installed archive plugin calls private 7z.dll\n";
        if (passed) passed = panel->Snapshot((output / "archive-engine.png").wstring(), false, 1);
        if (passed) {
            HandlePackageCommand(owner, EntriesForPackage(0, L"").back().command);
            passed = !PackageEnabled(0) && !PackageActive(owner.hwnd, 0);
            HandlePackageCommand(owner, EntriesForPackage(0, L"").back().command);
            const auto reopened = EntriesForPackage(0, L"");
            passed = passed && reopened.size() >= 2 && HandlePackageCommand(owner, reopened.front().command) && PackageActive(owner.hwnd, 0);
        }
        report << (passed ? "[PASS]" : "[FAIL]") << " archive package disables and reenables\n";
        wchar_t settings_probe[4]{};
        if (GetEnvironmentVariableW(L"PULSE_TEST_NATIVE_SETTINGS", settings_probe, 4)) {
            passed = RunSettingsPagesProbe(owner, directory, report) && passed;
            report << "Result: " << (passed ? "PASS" : "FAIL") << '\n';
            return passed;
        }
        auto* tab = ActiveTab(owner);
        passed = passed && Wait([&] { tab = ActiveTab(owner); return tab && !tab->loading && tab->snapshot; });
        if (GetEnvironmentVariableW(L"PULSE_TEST_NATIVE_STAGING", nullptr, 0)) {
            passed = passed && RunArchiveStagingProbe(owner, directory, report);
            StopOwner(owner.hwnd);
            report << "Result: " << (passed ? "PASS" : "FAIL") << '\n';
            return passed;
        }
        wchar_t compression_probe[4]{};
        if (GetEnvironmentVariableW(L"PULSE_TEST_NATIVE_COMPRESSION", compression_probe, 4)) {
            passed = passed && RunArchiveCompressionProbe(owner, directory, report);
            StopOwner(owner.hwnd);
            report << "Result: " << (passed ? "PASS" : "FAIL") << '\n';
            return passed;
        }
        if (passed) {
            const auto select = [&](const wchar_t* name) {
                for (size_t i = 0; i < tab->EntryCount(); ++i) if (tab->EntryAt(i).name == name) {
                    tab->SelectOnly(static_cast<int>(i)); return true;
                }
                return false;
            };
            passed = select(L"sample.7z");
            if (passed) {
                auto rows = CommandRows(owner);
                bool commands_ok = rows.size() == 3 && CommandRows(owner, L"解压").size() == 2;
                const int token = rows.empty() ? 0 : rows.front().command;
                ui::FluentMenu menu;
                commands_ok = commands_ok && menu.Create(owner.hwnd, &owner.compositor, owner.scale) &&
                    menu.SaveDebugSnapshot((output / "archive-commands.png").c_str(), BuildToolbarMoreItems(owner));
                DispatchMenuCommand(owner, token + 0x10000000);
                commands_ok = commands_ok && IsCommandPinned(owner, token) && PinnedCommandRows(owner).size() == 1;
                const auto toolbar = ToolbarPinActions(owner);
                commands_ok = commands_ok && std::any_of(toolbar.begin(), toolbar.end(), [token](const auto& action) { return action.command == token && action.enabled; });
                commands_ok = commands_ok && select(L"notes.txt") && CommandRows(owner).size() == 1 && !PinnedCommandRows(owner).front().enabled;
                auto* current_panel = PackageUi(owner.hwnd, 0);
                const auto before = Text(current_panel->NodeWindow("engine-status"));
                DispatchMenuCommand(owner, token);
                commands_ok = commands_ok && Text(current_panel->NodeWindow("engine-status")) == before;
                commands_ok = commands_ok && select(L"sample.7z");
                RECT original{}; GetWindowRect(owner.hwnd, &original);
                SetWindowPos(owner.hwnd, nullptr, 0, 0, 640, 600, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                const auto overflow = BuildToolbarMoreItems(owner);
                const auto pinned = ToolbarPinActions(owner);
                const float width = static_cast<float>(owner.compositor.Width());
                const float left = owner.renderer.EffectiveSidebarWidth(width);
                const auto layout = owner.renderer.ToolbarLayoutAt(width, owner.renderer.NewButtonWidthPx(width - left < 600 * owner.scale),
                    owner.pane ? owner.pane->filter_expand : 0.0f);
                bool pin_accessible = std::any_of(overflow.begin(), overflow.end(), [token](const auto& row) { return row.command == token && row.enabled; });
                for (size_t i = 0; i < pinned.size() && i < layout.pins.size(); ++i)
                    if (pinned[i].command == token && pinned[i].enabled && layout.pins[i].right > layout.pins[i].left) pin_accessible = true;
                commands_ok = commands_ok && pin_accessible;
                SetWindowPos(owner.hwnd, nullptr, 0, 0, original.right - original.left, original.bottom - original.top,
                    SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                commands_ok = RunArchivePreviewProbe(owner, directory, report) && commands_ok;
                commands_ok = RunArchiveTaskProbe(owner, directory, report) && commands_ok;
                commands_ok = RunArchiveStagingProbe(owner, directory, report) && commands_ok;
                HandlePackageCommand(owner, EntriesForPackage(0, L"").back().command);
                commands_ok = commands_ok && CommandRows(owner).empty() && !PinnedCommandRows(owner).front().enabled;
                DispatchMenuCommand(owner, token);
                commands_ok = commands_ok && !PackageActive(owner.hwnd, 0);
                report << (commands_ok ? "[PASS]" : "[FAIL]") << " native commands filter, pin, invoke current selection and reject retired menu tokens\n";
                passed = passed && commands_ok;
            }
        }
        StopOwner(owner.hwnd);
        report << "Result: " << (passed ? "PASS" : "FAIL") << '\n';
        return passed;
    }
    if (PackageCount() == 1 && (PackageManifest(0)->id == "sample.native-rename" ||
        PackageManifest(0)->id == "sample.native-live-data" || PackageManifest(0)->id == "sample.native-directory-browser" ||
        PackageManifest(0)->id == "sample.native-file-operations" || PackageManifest(0)->id == "sample.native-directory-transfer" ||
        PackageManifest(0)->id == "sample.native-directory-workbench")) {
        std::ofstream startup(output / "package-startup.txt");
        const bool lazy = !PackageActive(owner.hwnd, 0);
        const auto entries = EntriesForPackage(0, L"");
        const bool opened = entries.size() >= 2 && HandlePackageCommand(owner, entries.front().command) &&
            PackageActive(owner.hwnd, 0);
        startup << (lazy ? "[PASS]" : "[FAIL]") << " installed revision remains lazy until normal open\n"
            << (opened ? "[PASS]" : "[FAIL]") << " normal palette opens installed revision\n";
        startup.flush();
        return lazy && opened && RunUiProbe(owner, directory);
    }
    std::ofstream report(output / "report.txt");
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        report << (ok ? "[PASS] " : "[FAIL] ") << label << '\n'; report.flush();
        if (!ok) ++failures;
        return ok;
    };
    auto finish = [&] { StopOwner(owner.hwnd); report << "Result: " << (failures ? "FAIL" : "PASS") << '\n'; return failures == 0; };
    size_t inspector = PackageCount(), reactive = PackageCount(), bad = PackageCount();
    for (size_t i = 0; i < PackageCount(); ++i) {
        const auto* item = PackageManifest(i);
        if (item->id == "sample.native-inspector") inspector = i;
        if (item->id == "sample.native-reactive") reactive = i;
        if (item->id == "sample.native-bad-entry") bad = i;
    }
    if (!check(PackageCount() == 3 && inspector < 3 && reactive < 3 && bad < 3,
        "default installed store discovers three separately identified native packages")) return finish();
    check(!PackageActive(owner.hwnd, inspector) && !PackageActive(owner.hwnd, reactive),
        "discovery does not execute DLLs or open plugin windows");
    const auto rows = PackageEntries(L"");
    check(rows.size() == 6 && rows[0].text != rows[2].text && !rows[0].shortcut.empty() &&
        !rows[1].shortcut.empty() && rows[0].shortcut != rows[1].shortcut,
        "same-name packages retain distinct identities and visible open or disable actions");
    ui::FluentMenu menu;
    check(menu.Create(owner.hwnd, &owner.compositor, owner.scale) &&
        menu.SaveDebugSnapshot((output / "installed-palette.png").c_str(), rows), "capture ordinary native package entry points");
    const auto open = [&](size_t slot) {
        const auto entries = EntriesForPackage(slot, L"");
        return entries.size() >= 2 && HandlePackageCommand(owner, entries.front().command) && PackageActive(owner.hwnd, slot);
    };
    if (!check(open(inspector), "normal palette opens installed inspector without a developer DLL argument")) return finish();
    if (!check(open(reactive), "second installed native package opens independently")) return finish();
    auto* first = PackageUi(owner.hwnd, inspector);
    auto* second = PackageUi(owner.hwnd, reactive);
    check(first && second && first != second && first->Window() != second->Window(), "native packages own distinct real panels");
    if (!first || !second) return finish();
    SetWindowTextW(second->NodeWindow("query-input"), L"installed plugin");
    check(Wait([&] { return Text(second->NodeWindow("result-text")) == L"INSTALLED PLUGIN"; }),
        "installed plugin receives native input and dispatches its own worker result");
    check(!StartPackage(owner, bad) && PackageLastStatus(owner.hwnd, bad) == PULSE_ERROR_ENTRY &&
        PackageActive(owner.hwnd, inspector) && PackageActive(owner.hwnd, reactive),
        "bad entry is diagnosed without stopping either working package");
    check(Wait([&] { const auto* tab = ActiveTab(owner); return tab && !tab->loading && tab->snapshot; }), "fixture directory is ready");
    SendMessageW(first->NodeWindow("analyze-button"), BM_CLICK, 0, 0);
    check(Wait([&] { return Text(first->NodeWindow("message-text")) == L"Analysis complete: 3 files, 2 extensions."; }),
        "installed inspector calls actual directory service under its own identity");
    check(first->Snapshot((output / "inspector-installed.png").wstring(), false, 1), "capture installed inspector");
    check(second->Snapshot((output / "reactive-installed.png").wstring(), true, 1.5f), "capture second installed package at 150 percent DPI");
    const auto image_path = PackageNativePath(inspector);
    HANDLE write = CreateFileW(image_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    check(write == INVALID_HANDLE_VALUE, "loaded verified DLL cannot be overwritten");
    if (write != INVALID_HANDLE_VALUE) CloseHandle(write);
    const auto image_directory = std::filesystem::path(image_path).parent_path();
    const auto moved_directory = image_directory.parent_path() / "probe-moved-binaries";
    const bool renamed = MoveFileW(image_directory.c_str(), moved_directory.c_str()) != FALSE;
    check(!renamed, "loaded image keeps ancestor directory pinned against path replacement");
    if (renamed) MoveFileW(moved_directory.c_str(), image_directory.c_str());
    SendMessageW(first->Window(), WM_CLOSE, 0, 0);
    check(open(inspector) && PackageUi(owner.hwnd, inspector)->NodeWindow("analyze-button"), "closed native panel reopens with a fresh module instance");
    const auto toggle = EntriesForPackage(inspector, L"").back().command;
    HandlePackageCommand(owner, toggle);
    check(!PackageEnabled(inspector) && !PackageActive(owner.hwnd, inspector) && !PackageUi(owner.hwnd, inspector) &&
        PackageActive(owner.hwnd, reactive), "disabling one package retires its UI and preserves the other");
    SetWindowTextW(second->NodeWindow("query-input"), L"still active");
    check(Wait([&] { return Text(second->NodeWindow("result-text")) == L"STILL ACTIVE"; }), "remaining package continues after peer unload");
    HandlePackageCommand(owner, EntriesForPackage(inspector, L"").back().command);
    if (!check(PackageEnabled(inspector) && open(inspector), "normal enable and open creates a new native session")) return finish();
    auto* reopened = PackageUi(owner.hwnd, inspector);
    check(reopened && reopened->Snapshot((output / "inspector-reenabled.png").wstring(), true, 1), "capture reenabled native panel");
    StopOwner(owner.hwnd);
    check(!PackageActive(owner.hwnd, inspector) && !PackageActive(owner.hwnd, reactive), "owner shutdown drains all native sessions");
    return finish();
}
}

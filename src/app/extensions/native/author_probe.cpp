#include "author_probe.h"
#include "session.h"
#include "ui_adapter.h"
#include "package_commands.h"
#include "../runtime.h"
#include "../../app_state.h"
#include <filesystem>
#include <fstream>

namespace pulse::extensions::native {
bool RunAuthorProbe(AppState& owner, const std::wstring& directory) {
    if (!owner.isolatedTest || PackageCount() != 1 || PackageManifest(0)->id != "independent.number-lens") return false;
    const std::filesystem::path output(directory);
    std::ofstream report(output / L"report.txt");
    unsigned failures = 0;
    auto check = [&](bool ok, const char* label) {
        report << (ok ? "[PASS] " : "[FAIL] ") << label << '\n'; report.flush();
        if (!ok) ++failures;
        return ok;
    };
    auto finish = [&] { StopOwner(owner.hwnd); report << "Result: " << (failures ? "FAIL" : "PASS") << '\n'; return failures == 0; };
    auto open = [&] {
        const auto rows = EntriesForPackage(0, L"");
        return rows.size() >= 2 && HandlePackageCommand(owner, rows.front().command) && PackageActive(owner.hwnd, 0);
    };
    check(!PackageActive(owner.hwnd, 0), "installed author DLL stays inactive before normal open");
    if (!check(open(), "normal package command activates independently authored DLL")) return finish();
    auto* ui = PackageUi(owner.hwnd, 0);
    if (!check(ui && IsWindow(ui->NodeWindow("numbers-input")), "author model creates real native input")) return finish();
    auto result = [&] {
        const HWND window = ui->NodeWindow("result-text");
        std::wstring text(static_cast<size_t>(GetWindowTextLengthW(window)) + 1, L'\0');
        text.resize(static_cast<size_t>(GetWindowTextW(window, text.data(), static_cast<int>(text.size()))));
        return text;
    };
    auto click = [&](const char* id) {
        const HWND button = ui->NodeWindow(id);
        if (!IsWindow(button) || !IsWindowEnabled(button)) return false;
        SendMessageW(GetParent(button), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(button), BN_CLICKED), reinterpret_cast<LPARAM>(button));
        return true;
    };
    auto input = [&](const wchar_t* value) {
        const HWND edit = ui->NodeWindow("numbers-input");
        SetWindowTextW(edit, value);
        SendMessageW(GetParent(edit), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(edit), EN_CHANGE), reinterpret_cast<LPARAM>(edit));
        return click("analyze-button");
    };
    check(result() == L"Summary | Count: 8 | Mean: 5 | Median: 4.5", "DLL computes initial summary");
    check(ui->Snapshot((output / L"author-light.png").wstring(), false, 1), "application captures real author controls in light theme");
    check(click("toggle-button") && result() == L"Spread | Min: 2 | Max: 9 | Population SD: 2", "native button invokes independent statistics logic");
    check(ui->Snapshot((output / L"author-dark.png").wstring(), true, 1.5f), "application captures spread result at dark 150 percent");
    check(click("toggle-button") && input(L"1,2,3") && result() == L"Summary | Count: 3 | Mean: 2 | Median: 2", "native edit and analyze recompute changed input");
    check(input(L"") && result() == L"No numbers yet. Enter a number list.", "empty input clears stale statistics");
    check(input(L"1,x") && result() == L"Invalid number. Use spaces or commas between numbers.", "invalid input reports a concrete error");
    check(ui->Snapshot((output / L"author-error.png").wstring(), false, 1), "application captures error state");
    const HWND old_panel = ui->Window();
    auto rows = EntriesForPackage(0, L"");
    check(!rows.empty() && HandlePackageCommand(owner, rows.back().command) && !PackageActive(owner.hwnd, 0) &&
        !PackageUi(owner.hwnd, 0) && !IsWindow(old_panel), "normal disable retires panel and subscriptions");
    rows = EntriesForPackage(0, L"");
    if (!check(!rows.empty() && HandlePackageCommand(owner, rows.back().command) && open(), "normal enable and reopen create a fresh instance")) return finish();
    ui = PackageUi(owner.hwnd, 0);
    check(ui && result() == L"Summary | Count: 8 | Mean: 5 | Median: 4.5", "reopened DLL has fresh state");
    StopOwner(owner.hwnd);
    check(!PackageActive(owner.hwnd, 0) && IsWindow(owner.hwnd), "host survives author plugin shutdown");
    return finish();
}
}

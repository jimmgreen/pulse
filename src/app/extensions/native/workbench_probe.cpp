#include "workbench_probe.h"
#include "session.h"
#include "ui_adapter.h"
#include "../runtime.h"
#include "../../app_state.h"
#include <filesystem>
#include <fstream>
#include <functional>

namespace pulse::extensions::native {
namespace {
bool Wait(const std::function<bool()>& ready, DWORD milliseconds = 15000) {
    const auto deadline = GetTickCount64() + milliseconds;
    do {
        MSG message{};
        for (unsigned n = 0; n < 64 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++n) {
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
    std::wstring value(static_cast<size_t>(GetWindowTextLengthW(window)) + 1, L'\0');
    value.resize(static_cast<size_t>(GetWindowTextW(window, value.data(), static_cast<int>(value.size()))));
    return value;
}
}

bool RunWorkbenchProbe(AppState& owner, const std::wstring& directory) {
    wchar_t profile[32768]{};
    const auto length = GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", profile, 32768);
    if (!owner.isolatedTest || !length || length >= 32768 ||
        !GetEnvironmentVariableW(L"PULSE_TEST_NATIVE_CASE", nullptr, 0)) return false;
    const std::filesystem::path output(directory), fixture = std::filesystem::path(profile) / L"fixture";
    const auto relative = output.lexically_normal().lexically_relative(std::filesystem::path(profile).lexically_normal());
    if (relative.empty() || relative.is_absolute() || *relative.begin() == L".." || relative == L".") return false;
    auto* ui = OwnerUi(owner.hwnd);
    if (!ui || !OwnerActive(owner.hwnd)) return false;
    std::filesystem::create_directories(output);
    std::ofstream report(output / L"report.txt");
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        report << (ok ? "[PASS] " : "[FAIL] ") << label << '\n'; report.flush();
        if (!ok) ++failures;
        return ok;
    };
    auto finish = [&] { StopOwner(owner.hwnd); report << "Result: " << (failures ? "FAIL" : "PASS") << '\n'; return failures == 0; };
    auto label = [&](const char* id) { return Text(ui->NodeWindow(id)); };
    auto click = [&](const char* id) {
        const HWND button = ui->NodeWindow(id);
        if (!IsWindow(button) || !IsWindowEnabled(button)) return false;
        SendMessageW(GetParent(button), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(button), BN_CLICKED), reinterpret_cast<LPARAM>(button));
        return true;
    };
    auto rows = [&] { const auto node = ui->DataNode("items"); return node ? node->rows : std::vector<UiDataRow>{}; };
    auto visible_data = [&] {
        const HWND table = ui->NodeWindow("items");
        RECT bounds{}, viewport{}, visible{};
        if (!IsWindowVisible(table) || !GetWindowRect(table, &bounds) || !GetClientRect(ui->Window(), &viewport)) return false;
        MapWindowPoints(ui->Window(), nullptr, reinterpret_cast<POINT*>(&viewport), 2);
        if (!IntersectRect(&visible, &bounds, &viewport)) return false;
        const auto dpi = ui->MountInfo().dpi;
        return visible.right - visible.left >= MulDiv(160, dpi, 96) &&
            visible.bottom - visible.top >= MulDiv(128, dpi, 96);
    };
    auto* initial = ui->Window();
    const HWND path_input = ui->NodeWindow("path-input");
    if (!check(initial && IsWindow(path_input) && Wait([&] { return label("status") == L"ready" && !rows().empty(); }),
        "independent workbench opens the actual Pulse context directory")) return finish();
    check(!rows().empty() && IsWindow(ui->NodeWindow("card-0")) && ui->DataNode("items").has_value(),
        "real native Grid cards and provider-bound DataTable are populated");
    check(ui->SnapshotCurrent((output / L"workbench-window-cards.png").wstring()),
        "capture actual mounted window cards at host size");
    check(click("view-table") && Wait([&] { return label("view") == L"table"; }) && ui->SnapshotCurrent((output / L"workbench-window-table.png").wstring()),
        "switch the same native panel to its real details table");
    const HWND filter = ui->NodeWindow("filter-input");
    const auto before_filter = rows().size();
    SetWindowTextW(filter, L"summary.txt");
    SendMessageW(GetParent(filter), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(filter), EN_CHANGE), reinterpret_cast<LPARAM>(filter));
    check(Wait([&] { const auto current = rows(); return current.size() == 1 && current.front().text == "summary.txt"; }) &&
        before_filter > 1, "native filter reduces the real table to its single matching entry");
    SetWindowTextW(filter, L"");
    SendMessageW(GetParent(filter), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(filter), EN_CHANGE), reinterpret_cast<LPARAM>(filter));
    check(Wait([&] { return !rows().empty(); }), "clearing the filter restores current directory entries");
    check(click("mount-sidebar") && Wait([&] { return ui->IsSidebar(); }) &&
        (GetWindowLongPtrW(ui->Window(), GWL_STYLE) & WS_CHILD) != 0 && ui->Window() == initial &&
        ui->NodeWindow("path-input") == path_input && ui->SnapshotCurrent((output / L"workbench-sidebar.png").wstring()),
        "sidebar mounts the same live HWND and control state inside Pulse");
    check(visible_data(), "sidebar initially exposes table header and multiple rows without scrolling past controls");
    RECT first_mount{}, last_mount{};
    check(GetWindowRect(ui->NodeWindow("mount-window"), &first_mount) &&
        GetWindowRect(ui->NodeWindow("mount-workspace"), &last_mount) && first_mount.top == last_mount.top,
        "all mount controls remain in one compact row at sidebar width");
    check(click("mount-workspace") && Wait([&] { return ui->IsWorkspace(); }) && ui->Window() == initial &&
        ui->NodeWindow("path-input") == path_input && ui->SnapshotCurrent((output / L"workbench-workspace.png").wstring()),
        "workspace mounts the same live HWND and control state inside Pulse");
    check(visible_data(), "workspace initially exposes table header and multiple rows");
    check(click("mount-window") && Wait([&] { return !ui->IsSidebar() && !ui->IsWorkspace() && IsWindow(ui->Window()); }) &&
        ui->Window() == initial && ui->NodeWindow("path-input") == path_input,
        "returning to Window preserves the panel HWND and control tree");
    const HWND advanced = ui->NodeWindow("advanced");
    SendMessageW(advanced, BM_CLICK, 0, 0);
    const bool advanced_open = Wait([&] { return IsWindowVisible(path_input) != FALSE; }, 1000);
    SendMessageW(advanced, BM_CLICK, 0, 0);
    check(advanced_open && Wait([&] { return IsWindowVisible(path_input) == FALSE; }, 1000),
        "secondary directory and settings controls expand and collapse through the native section header");
    const auto watch_file = fixture / L"workbench-probe-watch.txt";
    const auto old_count = rows().size();
    { std::ofstream created(watch_file, std::ios::binary); created << "isolated watch fixture\n"; }
    check(Wait([&] { const auto current = rows(); return current.size() == old_count + 1; }),
        "host directory watcher refreshes workbench after a real fixture file appears");
    check(click("view-table") && Wait([&] { return label("view") == L"table" && label("settings").find(L"saved") != std::wstring::npos; }),
        "workbench persists a user view change through package-owned settings CAS");
    StopOwner(owner.hwnd);
    check(!OwnerActive(owner.hwnd) && !OwnerUi(owner.hwnd), "deactivation releases mounted UI and host occupancy");
    if (StartPackage(owner, 0)) {
        ui = OwnerUi(owner.hwnd);
        check(ui && Wait([&] { return label("view") == L"table" && label("status") == L"ready"; }),
            "new session restores the saved view while following current Pulse context");
        if (ui) check(ui->SnapshotCurrent((output / L"workbench-restored-table.png").wstring()),
            "capture the restored native workbench session");
    } else check(false, "enabled package starts a fresh session after deactivation");
    return finish();
}
}

#include "data_probe.h"
#include "session.h"
#include "ui_adapter.h"
#include "../../app_runtime.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
namespace pulse::extensions::native {
namespace {
bool Wait(const std::function<bool()> &ready) {
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
    std::wstring text(static_cast<size_t>(GetWindowTextLengthW(window)) + 1, wchar_t{});
    text.resize(static_cast<size_t>(GetWindowTextW(window, text.data(), static_cast<int>(text.size()))));
    return text;
}
}
bool RunDataProbe(AppState &owner, const std::wstring &directory) {
    if (!owner.isolatedTest || !GetEnvironmentVariableW(L"PULSE_TEST_NATIVE_CASE", nullptr, 0)) return false;
    auto *ui = OwnerUi(owner.hwnd);
    if (!ui || !OwnerActive(owner.hwnd)) return false;
    const std::filesystem::path output(directory);
    if (!output.is_absolute()) return false;
    std::filesystem::create_directories(output);
    std::ofstream report(output / L"report.txt");
    int failures = 0;
    auto check = [&](bool ok, const char *label) {
        report << (ok ? "[PASS] " : "[FAIL] ") << label << '\n'; report.flush();
        if (!ok) ++failures;
        return ok;
    };
    auto finish = [&] { report << "Result: " << (failures ? "FAIL" : "PASS") << '\n'; return !failures; };
    auto rows = [&] { auto node = ui->DataNode("items"); return node ? node->rows : std::vector<UiDataRow>{}; };
    auto status = [&] { return Text(ui->NodeWindow("status-label")); };
    auto error = [&] { return Text(ui->NodeWindow("error-label")); };
    auto click = [&](const char *id) {
        const auto button = ui->NodeWindow(id);
        if (!IsWindow(button) || !IsWindowEnabled(button)) return false;
        SendMessageW(button, BM_CLICK, 0, 0); return true;
    };
    if (!check(Wait([&] { return status() == L"ready" && rows().size() == 128; }),
        "independently compiled DLL publishes 128 real generated rows through native provider")) return finish();
    check(rows().front().text == "Item 00001.txt" && rows().front().cells.size() == 3,
        "actual shared DataTable displays author columns and stable identities");
    check(ui->Snapshot((output/L"initial-light.png").wstring(), false, 1.0f), "capture initial live table with application renderer");
    bool paged = true;
    const auto paging_start = GetTickCount64();
    for (size_t page = 2; page <= 32 && paged; ++page)
        paged = click("next") && Wait([&] { return status() == L"ready" && rows().size() == page * 128; });
    const auto all = rows(); std::set<std::string> ids;
    for (const auto &row : all) ids.insert(row.id);
    check(paged && all.size() == 4096 && ids.size() == 4096 && all.back().text == "Item 04096.txt",
        "all 4096 author items page into actual control without duplicates or missing rows");
    report << "[INFO] 4096 paging observed wallMs=" << GetTickCount64()-paging_start << " (includes 80ms author delay/page)\n";
    SetWindowTextW(ui->NodeWindow("filter-input"), L"0001");
    if (!check(Wait([&] { return status() == L"ready" && rows().size() == 11; }),
        "real edit event filters author-owned data on worker and replaces previous snapshot")) return finish();
    check(click("descending") && Wait([&] { const auto r=rows(); return status()==L"ready" && r.size()==11 && r.front().text=="Item 00019.txt"; }),
        "sort state automatically restarts request and author controls sort algorithm");
    check(ui->Snapshot((output/L"filtered-dark.png").wstring(), true, 1.5f, "items"), "capture filtered table in dark theme at 150 percent DPI");
    for (int i = 0; i < 24; ++i) {
        const auto text = L"rapid-"+std::to_wstring(i);
        SetWindowTextW(ui->NodeWindow("filter-input"), text.c_str());
    }
    SetWindowTextW(ui->NodeWindow("filter-input"), L"04096");
    check(Wait([&] { return status()==L"ready" && rows().size()==1 && rows().front().text=="Item 04096.txt"; }),
        "rapid filters converge to latest generation rather than an obsolete worker result");
    SetWindowTextW(ui->NodeWindow("filter-input"), L""); click("ascending"); click("refresh");
    if (!check(Wait([&] { return status()==L"ready" && rows().size()==128 && rows().front().id=="item-1"; }),
        "refresh restores first sorted page and live stream")) return finish();
    const auto old_revision = rows().front().cells[2];
    const auto removed_id = rows().back().id;
    check(click("mutate") && Wait([&] {
        const auto current=rows();
        return status()==L"ready" && current.size()==128 &&
            std::none_of(current.begin(),current.end(),[&](const auto &row) { return row.id==removed_id; }) &&
            std::any_of(current.begin(),current.end(),[](const auto &row) { return row.text.ends_with(" (added)"); }) &&
            std::any_of(current.begin(),current.end(),[&](const auto &row) { return row.cells[2]!=old_revision; });
    }), "live delta removes inserts and updates author rows in actual native table");
    check(ui->Snapshot((output/L"delta-light.png").wstring(), false, 1.0f, "items"), "capture incremental result through application renderer");
    check(click("empty") && Wait([&] { return status()==L"ready" && rows().empty(); }), "empty author snapshot clears actual table");
    check(ui->Snapshot((output/L"empty-light.png").wstring(), false, 1.0f, "items"), "capture empty data state");
    check(click("error") && Wait([&] { return status()==L"error" && error().starts_with(L"Deliberate provider error"); }),
        "author error becomes explicit observable UI state");
    check(ui->Snapshot((output/L"error-dark.png").wstring(), true, 1.0f), "capture provider error state");
    click("refresh");
    check(Wait([&] { return status()==L"loading"; }) && click("cancel") && status()==L"cancelled",
        "cancel interrupts visible loading state without waiting for worker");
    const auto until = GetTickCount64()+200;
    bool stable = true;
    while (GetTickCount64()<until) {
        Wait([] {return true;}); stable &= status()==L"cancelled";
        MsgWaitForMultipleObjectsEx(0, nullptr, 5, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    check(stable, "canceled worker result does not overwrite terminal cancellation");
    click("refresh");
    const auto stopped = GetTickCount64(); StopOwner(owner.hwnd);
    check(!OwnerActive(owner.hwnd) && !OwnerUi(owner.hwnd) && IsWindow(owner.hwnd),
        "stop cancels provider then joins independent DLL worker while host survives");
    report << "[INFO] observedStopMs=" << GetTickCount64()-stopped << '\n';
    return finish();
}
}

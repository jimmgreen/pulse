#include "directory_probe.h"
#include "session.h"
#include "ui_adapter.h"
#include "../../app_navigation.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
namespace pulse::extensions::native {
namespace {
bool Wait(const std::function<bool()> &ready, DWORD milliseconds = 12000) {
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
    std::wstring value(static_cast<size_t>(GetWindowTextLengthW(window)) + 1, wchar_t{});
    value.resize(static_cast<size_t>(GetWindowTextW(window, value.data(), static_cast<int>(value.size()))));
    return value;
}
}
bool RunDirectoryProbe(AppState &owner, const std::wstring &directory) {
    wchar_t profile[32768]{};
    const auto length = GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", profile, 32768);
    if (!owner.isolatedTest || !length || length >= 32768 ||
        !GetEnvironmentVariableW(L"PULSE_TEST_NATIVE_CASE", nullptr, 0)) return false;
    const std::filesystem::path output(directory), root = std::filesystem::path(profile) / L"fixture";
    const auto relative = output.lexically_normal().lexically_relative(std::filesystem::path(profile).lexically_normal());
    if (relative.empty() || relative.is_absolute() || *relative.begin() == L".." || relative == L".") return false;
    auto *ui = OwnerUi(owner.hwnd);
    if (!ui || !OwnerActive(owner.hwnd)) return false;
    std::filesystem::create_directories(output);
    std::ofstream report(output / L"report.txt");
    int failures = 0;
    auto check = [&](bool ok, const char *label) {
        report << (ok ? "[PASS] " : "[FAIL] ") << label << '\n'; report.flush();
        if (!ok) ++failures;
        return ok;
    };
    auto finish = [&] { StopOwner(owner.hwnd); report << "Result: " << (failures ? "FAIL" : "PASS") << '\n'; return !failures; };
    auto rows = [&] { auto value = ui->DataNode("items"); return value ? value->rows : std::vector<UiDataRow>{}; };
    auto label = [&](const char *id) { return Text(ui->NodeWindow(id)); };
    auto ready = [&] { return label("status-label") == L"ready"; };
    auto click = [&](const char *id) {
        const auto button = ui->NodeWindow(id);
        if (!IsWindow(button) || !IsWindowEnabled(button)) return false;
        SendMessageW(button, BM_CLICK, 0, 0); return true;
    };
    auto open = [&](const std::filesystem::path &path) {
        return SetWindowTextW(ui->NodeWindow("directory-input"), path.c_str()) && click("open-directory");
    };
    auto contains = [&](const char *name) {
        const auto current = rows();
        return std::any_of(current.begin(), current.end(), [&](const auto &row) { return row.text == name; });
    };
    if (!check(Wait([&] { return !label("context-label").empty() && ready(); }),
        "installed independent DLL observes actual owner context and opens initial directory")) return finish();
    if (!check(open(root/L"A") && Wait([&] { return ready() && rows().size() == 256; }),
        "arbitrary directory snapshot supplies 256 rows independently of current owner directory")) return finish();
    check(ui->Snapshot((output/L"initial-light.png").wstring(), false, 1.0f, "items"),
        "capture actual directory data through application renderer");
    check(click("next") && Wait([&] { return ready() && rows().size() == 300; }) && contains("seed000.txt") && contains("seed299.txt"),
        "snapshot-bound continuation displays all 300 real files without a host preset algorithm");
    check(ui->Snapshot((output/L"paged-dark.png").wstring(), true, 1.5f, "items"),
        "capture real directory table in dark theme at 150 percent DPI");
    const auto old_id = rows().empty() ? std::string{} : rows().front().id;
    // Only mutate dedicated harness files, never an arbitrary selected directory.
    std::ofstream(root/L"A"/L"000-new.txt", std::ios::binary) << "new-by-fixture\n";
    if (!check(Wait([&] { return ready() && rows().size() == 256 && !rows().empty() && rows().front().id != old_id; }),
        "existing filesystem watcher invalidates and refreshes the displayed snapshot after create")) return finish();
    check(click("next") && Wait([&] { return ready() && rows().size() == 301; }) && contains("000-new.txt"),
        "refreshed immutable pagination includes newly created file");
    const auto before_rename = rows().front().id;
    std::filesystem::rename(root/L"A"/L"seed000.txt", root/L"A"/L"renamed.txt");
    std::filesystem::remove(root/L"A"/L"seed001.txt");
    std::ofstream(root/L"A"/L"seed002.txt", std::ios::binary | std::ios::trunc) << "changed-by-fixture\n";
    if (!check(Wait([&] { return ready() && rows().size() == 256 && !rows().empty() && rows().front().id != before_rename; }),
        "rename delete and content writes coalesce into real directory refresh")) return finish();
    check(click("next") && Wait([&] { return ready() && rows().size() == 300; }) &&
        contains("renamed.txt") && !contains("seed000.txt") && !contains("seed001.txt"),
        "new snapshot replaces renamed and deleted entries without carrying obsolete pages");
    const auto changed = rows();
    check(std::any_of(changed.begin(), changed.end(), [](const auto &row) {
        return row.text == "seed002.txt" && row.cells.size() == 3 && row.cells[1] == "19";
    }), "refreshed directory metadata includes actual changed byte length");
    check(ui->Snapshot((output/L"changes-light.png").wstring(), false, 1.0f, "items"),
        "capture actual watch-driven changes");
    if (!check(Wait([&] { const auto *tab = ActiveTab(owner); return tab && !tab->loading && tab->EntryCount() >= 3; }),
        "owner fixture is loaded for selection event acceptance")) return finish();
    auto *tab = ActiveTab(owner);
    tab->SelectOnly(0);
    const auto selected_name = tab->EntryAt(0).name;
    check(Wait([&] { return label("selection-label").find(selected_name) != std::wstring::npos; }),
        "actual owner single-selection change reaches independent native context subscription");
    tab->SelectIndices({0, 1});
    const auto previous_context = label("selection-label");
    check(Wait([&] { return label("selection-label") != previous_context; }), "multi-selection summary and revision update");
    const auto previous_multi = label("selection-label"); tab->SelectIndices({1, 2});
    check(Wait([&] { return label("selection-label") != previous_multi; }),
        "equal-count multi-selection change remains observable in actual host");
    if (!check(open(root/L"Empty") && Wait([&] { return ready() && rows().empty(); }),
        "empty arbitrary directory completes without leftover previous rows")) return finish();
    check(ui->Snapshot((output/L"empty-light.png").wstring(), false, 1.0f, "items"), "capture real empty directory");
    check(open(root/L"missing") && Wait([&] { return label("status-label") == L"error" && !label("error-label").empty(); }),
        "missing directory reports explicit service failure in real control");
    check(ui->Snapshot((output/L"error-dark.png").wstring(), true, 1.0f), "capture actual directory error");
    for (unsigned n = 0; n < 8; ++n) open(root/(n % 2 ? L"B" : L"A"));
    if (!check(open(root/L"B") && Wait([&] { return ready() && rows().size() == 4 && contains("folder-child"); }),
        "rapid directory switching converges to final handle and rejects stale snapshot deliveries")) return finish();
    NavigateTo(owner, (root/L"A").wstring());
    check(Wait([&] { const auto *current = ActiveTab(owner); return current && !current->loading &&
        current->current_path.find(L"\\A") != std::wstring::npos &&
        label("context-label").find(L"\\A") != std::wstring::npos; }) && rows().size() == 4 && contains("folder-child"),
        "owner navigation event updates context while plugin independently retains directory B");
    const auto current = rows();
    const auto folder = std::find_if(current.begin(), current.end(), [](const auto &row) { return row.text == "folder-child"; });
    bool opened = folder != current.end();
    if (opened) {
        const auto table = ui->NodeWindow("items");
        SetFocus(table); SendMessageW(table, WM_KEYDOWN, VK_HOME, 0);
        for (size_t n = 0; n < static_cast<size_t>(folder - current.begin()); ++n) SendMessageW(table, WM_KEYDOWN, VK_DOWN, 0);
        opened = click("open-item") && Wait([&] { const auto *value = ActiveTab(owner); return value && !value->loading &&
            value->current_path.find(L"folder-child") != std::wstring::npos; });
    }
    check(opened, "real table keyboard selection opens snapshot folder through normal Pulse navigation");
    check(click("close-directory") && Wait([&] { return rows().empty() && label("status-label") == L"cancelled"; }),
        "closing directory cancels requests clears rows and detaches watch");
    std::ofstream(root/L"B"/L"after-close.txt", std::ios::binary) << "closed-watch-fixture\n";
    const auto stopped_at = GetTickCount64(); bool stable = true;
    while (GetTickCount64() - stopped_at < 250) {
        Wait([] { return true; }); stable &= rows().empty() && label("status-label") == L"cancelled";
        MsgWaitForMultipleObjectsEx(0, nullptr, 5, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    check(stable, "closed directory never repopulates from pending watch callbacks");
    open(root/L"A"); StopOwner(owner.hwnd);
    check(!OwnerActive(owner.hwnd) && !OwnerUi(owner.hwnd) && IsWindow(owner.hwnd),
        "stop suppresses directory and context callbacks while pending filesystem work retires");
    return finish();
}
}

#include "fileops_probe.h"
#include "session.h"
#include "ui_adapter.h"
#include "package_commands.h"
#include "../../app_navigation.h"
#include "../../delete_preflight.h"
#include "../../../ui/confirm_dialog.h"
#include "../../../common/json_value.h"
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>

namespace pulse::extensions::native {
namespace {
std::function<void(HWND)> dialog_action;
void CALLBACK DialogTimer(HWND window, UINT, UINT_PTR timer, DWORD) {
    KillTimer(window, timer);
    if (dialog_action) dialog_action(window);
}
bool Wait(const std::function<bool()>& ready, DWORD timeout = 12000) {
    const auto deadline = GetTickCount64() + timeout;
    do {
        MSG message{};
        for (unsigned count = 0; count < 32 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++count) {
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
std::string Bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
}

bool RunFileOpsProbe(AppState& owner, const std::wstring& directory) {
    namespace fs = std::filesystem;
    wchar_t profile[32768]{};
    const auto length = GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", profile, 32768);
    const fs::path output(directory), root = fs::path(profile) / L"fixture";
    const auto relative = output.lexically_normal().lexically_relative(fs::path(profile).lexically_normal());
    if (!owner.isolatedTest || !length || length >= 32768 || relative.empty() || relative.is_absolute() ||
        *relative.begin() == L".." || relative == L"." || !GetEnvironmentVariableW(L"PULSE_TEST_NATIVE_CASE", nullptr, 0))
        return false;
    auto* ui = OwnerUi(owner.hwnd);
    if (!ui || !OwnerActive(owner.hwnd)) return false;
    fs::create_directories(output);
    std::ofstream report(output / L"report.txt");
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        report << (ok ? "[PASS] " : "[FAIL] ") << label << '\n'; report.flush();
        if (!ok) {
            ++failures;
            const auto status = Text(ui->NodeWindow("status-label"));
            report << "[DIAGNOSTIC] status=" << json::Utf8(status)
                << " preflight=" << static_cast<bool>(owner.delete_preflight) << '\n';
            const auto operation = owner.ops.Status();
            report << "[DIAGNOSTIC] task=" << operation.task_id << " active=" << operation.active
                << " phase=" << static_cast<int>(operation.phase) << " error=" << json::Utf8(operation.last_error) << '\n';
            report.flush();
        }
        return ok;
    };
    const auto finish = [&] {
        dialog_action = {};
        SetEnvironmentVariableW(L"PULSE_TEST_DELETE_PREFLIGHT_DELAY_MS", nullptr);
        StopOwner(owner.hwnd);
        report << "Result: " << (failures ? "FAIL" : "PASS") << '\n';
        return failures == 0;
    };
    const auto label = [&](const char* id) { return Text(ui->NodeWindow(id)); };
    const auto click = [&](const char* id) {
        const auto button = ui->NodeWindow(id);
        if (!IsWindow(button) || !IsWindowEnabled(button)) {
            report << "[DIAGNOSTIC] unavailable button=" << id << '\n'; report.flush();
            return false;
        }
        SendMessageW(GetParent(button), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(button), BN_CLICKED),
            reinterpret_cast<LPARAM>(button));
        return true;
    };
    const auto select = [&](const wchar_t* name) {
        if (!Wait([&] { const auto* tab = ActiveTab(owner); return tab && !tab->loading && tab->snapshot; })) return false;
        auto* tab = ActiveTab(owner);
        SelectNameInTab(owner, *tab, name);
        return tab->SelectedCount() == 1 && tab->selected_index >= 0 &&
            tab->EntryAt(static_cast<size_t>(tab->selected_index)).name == name;
    };
    const auto status = [&](const wchar_t* expected) {
        return Wait([&] { click("refresh-status"); return label("status-label") == expected; });
    };
    if (!check(Wait([&] { const auto* tab = ActiveTab(owner); return tab && !tab->loading && tab->snapshot; }),
        "installed DLL and isolated source listing are ready")) return finish();
    for (const wchar_t* name : {L"keep.txt", L"cancel.txt", L"cancel-confirm.txt", L"recycle.txt", L"disabled.txt"}) {
        if (!check(fs::is_regular_file(root / name) && !Bytes(root / name).empty(), "known isolated file fixture exists")) return finish();
    }
    const auto original = Bytes(root / L"recycle.txt");
    ActiveTab(owner)->ClearSelection();
    check(click("recycle-selection") && label("status-label") == L"rejected" && !owner.delete_preflight,
        "empty selection is rejected through actual DLL API call");
    check(select(L"folder") && click("recycle-selection") && label("status-label") == L"rejected" && !owner.delete_preflight,
        "directory selection is rejected before deletion preparation");
    check(ui->Snapshot((output / L"rejected-light.png").wstring(), false, 1.0f), "capture actual DLL rejection UI");

    int opened = 0, closed = 0;
    const auto observe = [&] {
        if (!owner.delete_preflight) return false;
        owner.delete_preflight->confirmation_observer = [&](HWND window) {
            if (!window) { ++closed; return; }
            ++opened;
            if (!SetTimer(window, 0x464f5053, 250, DialogTimer)) PostMessageW(window, WM_CLOSE, 0, 0);
        };
        return true;
    };
    dialog_action = [&](HWND window) {
        check(click("refresh-status") && label("status-label") == L"waiting", "DLL observes native confirmation as waiting");
        check(ui::SaveConfirmDialogFrame(window, (output / L"confirm-cancel-light.png").wstring()), "capture native recycle confirmation inside app");
        PostMessageW(window, WM_KEYDOWN, VK_RETURN, 0);
    };
    check(select(L"cancel.txt") && click("recycle-selection") && observe(), "DLL submits asynchronous recycle preparation");
    check(status(L"cancelled") && fs::exists(root / L"cancel.txt"), "initial Enter cancels native confirmation without mutation");
    check(opened == 1 && closed == 1, "cancelled native confirmation closes exactly once");

    dialog_action = [&](HWND window) {
        check(ui::SaveConfirmDialogFrame(window, (output / L"confirm-recycle-dark.png").wstring()), "capture confirmed recycle surface inside app");
        PostMessageW(window, WM_KEYDOWN, VK_LEFT, 0);
        PostMessageW(window, WM_KEYDOWN, VK_RETURN, 0);
    };
    const bool previous_dark = owner.darkMode;
    owner.darkMode = true;
    check(select(L"recycle.txt") && click("recycle-selection") && observe(), "DLL submits one known source for recycle");
    check(select(L"keep.txt"), "owner selection changes after immutable source capture");
    if (!check(status(L"completed") && !fs::exists(root / L"recycle.txt") && fs::exists(root / L"keep.txt"),
        "confirmed task recycles only the captured source and reports actual completion")) return finish();
    owner.darkMode = previous_dark;
    check(ui->Snapshot((output / L"completed-dark.png").wstring(), true, 1.5f), "capture real completed DLL UI at 150 percent DPI");
    uint64_t task = 0;
    try { task = std::stoull(label("task-label")); } catch (...) { }
    check(task && owner.ops.CanUndoTask(task), "completed recycle task has actual host undo metadata");
    owner.ops.Undo();
    check(Wait([&] { return fs::exists(root / L"recycle.txt") && Bytes(root / L"recycle.txt") == original; }),
        "host undo restores recycled fixture with identical bytes");

    dialog_action = [&](HWND) {
        check(click("cancel-request"), "DLL cancellation is invoked while native confirmation is open");
    };
    check(select(L"cancel-confirm.txt") && click("recycle-selection") && observe(), "second confirmation cancellation fixture begins");
    check(status(L"cancelled") && fs::exists(root / L"cancel-confirm.txt"), "SDK cancellation closes confirmation without mutation");

    SetEnvironmentVariableW(L"PULSE_TEST_DELETE_PREFLIGHT_DELAY_MS", L"600");
    check(select(L"cancel.txt") && click("recycle-selection") && owner.delete_preflight && click("cancel-request"),
        "DLL cancels its exact pending preparation");
    check(status(L"cancelled") && fs::exists(root / L"cancel.txt") && !owner.delete_preflight,
        "cancelled preparation leaves source intact and releases host request");
    check(select(L"disabled.txt") && click("recycle-selection") && owner.delete_preflight,
        "pending request exists before package disable");
    check(ui->Snapshot((output / L"pending-light.png").wstring(), false, 1.0f), "capture real pending request UI");
    const auto entries = EntriesForPackage(0, L"");
    check(entries.size() >= 2 && HandlePackageCommand(owner, entries.back().command), "normal package menu disables native DLL");
    check(Wait([&] { return !OwnerActive(owner.hwnd) && !owner.delete_preflight; }) && fs::exists(root / L"disabled.txt"),
        "revoked package cancels and releases its pending delete without execution");
    check(opened == 3 && closed == 3, "all native confirmations close without a leaked dialog");
    return finish();
}
}

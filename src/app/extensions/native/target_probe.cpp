#include "target_probe.h"
#include "session.h"
#include "ui_adapter.h"
#include "package_commands.h"
#include "../transfer_ui.h"
#include "../../app_navigation.h"
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
bool Wait(const std::function<bool()>& ready, DWORD timeout = 15000) {
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
bool RunTargetProbe(AppState& owner, const std::wstring& directory) {
    namespace fs = std::filesystem;
    wchar_t profile[32768]{};
    const auto length = GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", profile, 32768);
    const fs::path output(directory), root = fs::path(profile) / L"fixture", target = fs::path(profile) / L"target";
    const auto relative = output.lexically_normal().lexically_relative(fs::path(profile).lexically_normal());
    if (!owner.isolatedTest || !length || length >= 32768 || relative.empty() || relative.is_absolute() ||
        *relative.begin() == L".." || relative == L"." || !GetEnvironmentVariableW(L"PULSE_TEST_NATIVE_CASE", nullptr, 0)) return false;
    auto* ui = OwnerUi(owner.hwnd);
    if (!ui || !OwnerActive(owner.hwnd)) return false;
    fs::create_directories(output);
    std::ofstream report(output / L"report.txt");
    int failures = 0, opened = 0, closed = 0;
    const auto label = [&](const char* id) { return Text(ui->NodeWindow(id)); };
    const auto check = [&](bool ok, const char* text) {
        report << (ok ? "[PASS] " : "[FAIL] ") << text << '\n';
        if (!ok) {
            ++failures;
            if (OwnerActive(owner.hwnd)) report << "[DIAGNOSTIC] status=" << json::Utf8(label("status-label"))
                << " task=" << json::Utf8(label("task-label")) << '\n';
        }
        report.flush(); return ok;
    };
    const auto click = [&](const char* id) {
        const HWND button = ui->NodeWindow(id);
        if (!IsWindow(button) || !IsWindowEnabled(button)) return false;
        SendMessageW(GetParent(button), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(button), BN_CLICKED),
            reinterpret_cast<LPARAM>(button));
        return true;
    };
    const auto status = [&](const wchar_t* expected) {
        return Wait([&] { click("refresh-status"); return label("status-label") == expected; });
    };
    const auto open_target = [&](const fs::path& path) {
        return SetWindowTextW(ui->NodeWindow("destination-input"), path.c_str()) && click("open-target") &&
            Wait([&] { return label("status-label") == L"target-ready"; });
    };
    const auto select = [&](const wchar_t* name) {
        if (!Wait([&] { const auto* tab = ActiveTab(owner); return tab && !tab->loading && tab->snapshot; })) return false;
        auto* tab = ActiveTab(owner); SelectNameInTab(owner, *tab, name);
        return tab->SelectedCount() == 1 && tab->selected_index >= 0 &&
            tab->EntryAt(static_cast<size_t>(tab->selected_index)).name == name;
    };
    const auto finish = [&] {
        dialog_action = {}; SetTransferUiProbeObserver({}); SetTransferPreparationProbeObserver({});
        StopOwner(owner.hwnd);
        report << "Result: " << (failures ? "FAIL" : "PASS") << '\n';
        return failures == 0;
    };
    const auto accept = [](HWND window) {
        PostMessageW(window, WM_KEYDOWN, VK_LEFT, 0);
        PostMessageW(window, WM_KEYDOWN, VK_RETURN, 0);
    };
    SetTransferUiProbeObserver([&](HWND window) {
        if (!window) { ++closed; return; }
        ++opened;
        if (!SetTimer(window, 0x54415247, 250, DialogTimer)) PostMessageW(window, WM_CLOSE, 0, 0);
    });
    if (!check(Wait([&] { const auto* tab = ActiveTab(owner); return tab && !tab->loading && tab->snapshot; }),
        "installed V2 author DLL and isolated source listing are ready")) return finish();
    const auto* layout = owner.window_tabs.Active();
    check(layout && layout->VisiblePanes().size() == 1, "explicit directory transfer works without a second visible pane");
    check(SetWindowTextW(ui->NodeWindow("destination-input"), (fs::path(profile)/L"missing").c_str()) && click("open-target") &&
        Wait([&] { return label("status-label") == L"target-error"; }), "missing destination returns asynchronous directory error");
    if (!check(open_target(target), "DLL opens explicit destination through public directory service")) return finish();
    check(ui->Snapshot((output/L"target-ready-light.png").wstring(), false, 1.0f), "capture actual native target selection UI");
    ActiveTab(owner)->ClearSelection();
    check(click("copy-selection") && label("status-label") == L"rejected" && !TransferUiPending(owner.hwnd), "empty selection is rejected");
    check(select(L"folder") && click("copy-selection") && label("status-label") == L"rejected", "directory source is rejected before transfer");

    dialog_action = [&](HWND window) {
        check(click("refresh-status") && label("status-label") == L"waiting", "V2 DLL observes host confirmation waiting");
        check(ui::SaveConfirmDialogFrame(window, (output/L"confirm-copy-light.png").wstring()), "capture native explicit-target confirmation");
        PostMessageW(window, WM_KEYDOWN, VK_RETURN, 0);
    };
    check(select(L"cancel.txt") && click("copy-selection"), "submit explicit-target copy preparation");
    if (!check(status(L"cancelled") && fs::exists(root/L"cancel.txt") && !fs::exists(target/L"cancel.txt"),
        "initial Enter cancels without creating destination")) return finish();

    dialog_action = accept;
    const auto copied_bytes = Bytes(root/L"copy.txt");
    check(select(L"copy.txt") && click("copy-selection"), "copy captures selected source and directory handle");
    check(select(L"keep.txt") && SetWindowTextW(ui->NodeWindow("destination-input"), root.c_str()),
        "selection and input text changes do not retarget accepted preparation");
    if (!check(status(L"completed") && Bytes(target/L"copy.txt") == copied_bytes && Bytes(root/L"copy.txt") == copied_bytes &&
        fs::exists(root/L"keep.txt") && !fs::exists(target/L"keep.txt"), "copy commits captured source bytes only to opened target")) return finish();
    check(ui->Snapshot((output/L"copy-completed-light.png").wstring(), false, 1.0f), "capture completed public V2 copy UI");

    const auto collision = Bytes(target/L"collision.txt"), source_collision = Bytes(root/L"collision.txt");
    check(select(L"collision.txt") && click("copy-selection"), "existing-name conflict goes through explicit target request");
    if (!check(status(L"failed") && Bytes(target/L"collision.txt") == collision && Bytes(root/L"collision.txt") == source_collision,
        "conflict fails without replacement or source loss")) return finish();
    check(ui->Snapshot((output/L"conflict-light.png").wstring(), false, 1.0f), "capture actual failed conflict state");

    const auto moved_bytes = Bytes(root/L"move.txt");
    const bool old_dark = owner.darkMode; owner.darkMode = true;
    dialog_action = [&](HWND window) {
        check(ui::SaveConfirmDialogFrame(window, (output/L"confirm-move-dark.png").wstring()), "capture native move confirmation in dark theme");
        accept(window);
    };
    check(select(L"move.txt") && click("move-selection"), "move uses same directory handle through V2");
    if (!check(Wait([&] { return LastTransferTask(owner.hwnd) != 0; }), "confirmed move receives actual controlled task")) return finish();
    check(click("close-target"), "closing directory after task acceptance releases only directory handle");
    if (!check(status(L"completed") && !fs::exists(root/L"move.txt") && Bytes(target/L"move.txt") == moved_bytes,
        "accepted move completes with bytes preserved after directory handle close")) return finish();
    owner.darkMode = old_dark;
    check(ui->Snapshot((output/L"move-completed-dark.png").wstring(), true, 1.5f), "capture completed move at 150 percent DPI");

    if (!check(open_target(target), "closed target can be reopened with a fresh owned handle")) return finish();
    check(select(L"close.txt") && click("copy-selection") && click("close-target"), "close directory immediately during preparation");
    if (!check(status(L"cancelled") && !fs::exists(target/L"close.txt"), "closed target cancels preparation before mutation")) return finish();
    if (!check(open_target(target), "target reopens for modal lifetime test")) return finish();
    dialog_action = [&](HWND window) {
        check(click("close-target"), "close destination while native confirmation is open");
        accept(window);
    };
    check(select(L"close-confirm.txt") && click("copy-selection"), "prepare close-during-confirmation case");
    if (!check(status(L"cancelled") && !fs::exists(target/L"close-confirm.txt"), "confirmation cannot consume a closed destination lease")) return finish();

    if (!check(open_target(target), "target reopens for explicit cancellation")) return finish();
    dialog_action = [&](HWND) { check(click("cancel-request"), "DLL cancels its own pending confirmation"); };
    check(select(L"cancel.txt") && click("copy-selection"), "prepare cancellable explicit destination request");
    if (!check(status(L"cancelled") && !fs::exists(target/L"cancel.txt"), "SDK cancellation closes confirmation without file effects")) return finish();

    dialog_action = [&](HWND window) {
        const fs::path old = fs::path(profile)/L"target-old";
        fs::rename(target, old); fs::create_directory(target);
        accept(window);
    };
    check(select(L"replace.txt") && click("copy-selection"), "prepare immutable target identity before external replacement");
    if (!check(status(L"failed") && !fs::exists(target/L"replace.txt") &&
        !fs::exists(fs::path(profile)/L"target-old/replace.txt"), "changed directory object is rejected by controlled engine")) return finish();
    fs::remove(target); fs::rename(fs::path(profile)/L"target-old", target);

    check(SetWindowTextW(ui->NodeWindow("source-input"), root.c_str()) && click("open-source") &&
        Wait([&] { return label("status-label") == L"source-ready"; }),
        "V3 author DLL opens an arbitrary source directory and reads its current snapshot");
    check(SetWindowTextW(ui->NodeWindow("entry-input"), L"v3-copy.txt") && click("refresh-source") &&
        Wait([&] { return label("status-label") == L"source-refreshed"; }) && click("copy-entry") &&
        label("status-label") == L"rejected", "V3 rejects entry IDs from a replaced source snapshot");
    check(click("open-source") && Wait([&] { return label("status-label") == L"source-ready"; }),
        "V3 can reopen and enumerate a fresh source snapshot");
    check(SetWindowTextW(ui->NodeWindow("entry-input"), L"v3-copy.txt"), "select a named file from the enumerated source snapshot");
    const auto v3_copy_bytes = Bytes(root/L"v3-copy.txt");
    dialog_action = [&](HWND window) {
        check(ui::SaveConfirmDialogFrame(window, (output/L"v3-confirm-copy-light.png").wstring()),
            "capture native V3 transfer confirmation");
        accept(window);
    };
    check(click("copy-entry"), "V3 submits a current-snapshot file entry to the opened target");
    if (!check(status(L"completed") && Bytes(target/L"v3-copy.txt") == v3_copy_bytes &&
        Bytes(root/L"v3-copy.txt") == v3_copy_bytes, "V3 copies exact file bytes without removing the source")) return finish();
    check(ui->Snapshot((output/L"v3-copy-completed-light.png").wstring(), false, 1.0f),
        "capture completed V3 transfer in the application UI");

    check(SetWindowTextW(ui->NodeWindow("entry-input"), L"v3-move.txt"), "choose a second current-snapshot file for V3 move");
    dialog_action = accept;
    const auto v3_move_bytes = Bytes(root/L"v3-move.txt");
    check(click("move-entry") && status(L"completed") && !fs::exists(root/L"v3-move.txt") &&
        Bytes(target/L"v3-move.txt") == v3_move_bytes, "V3 move commits destination bytes and removes the verified source");

    check(SetWindowTextW(ui->NodeWindow("entry-input"), L"v3-metadata.txt"), "choose a file for metadata race validation");
    const auto metadata_bytes = Bytes(root/L"v3-metadata.txt");
    SetTransferPreparationProbeObserver([&](const std::vector<std::wstring>&) {
        std::ofstream changed(root/L"v3-metadata.txt", std::ios::binary | std::ios::trunc);
        changed << "metadata changed after source snapshot";
    });
    check(click("copy-entry") && status(L"failed") && !fs::exists(target/L"v3-metadata.txt"),
        "V3 rejects a source whose size changed after snapshot capture");
    { std::ofstream restore(root/L"v3-metadata.txt", std::ios::binary | std::ios::trunc); restore.write(metadata_bytes.data(), static_cast<std::streamsize>(metadata_bytes.size())); }

    check(SetWindowTextW(ui->NodeWindow("entry-input"), L"v3-lease.txt") && click("copy-entry") &&
        click("close-source") && status(L"cancelled") && !fs::exists(target/L"v3-lease.txt"),
        "closing the source directory invalidates the preparation lease before mutation");

    check(SetWindowTextW(ui->NodeWindow("source-input"), root.c_str()) && click("open-source") &&
        Wait([&] { return label("status-label") == L"source-ready"; }),
        "V4 DLL reopens the source and receives a fresh directory snapshot");
    const auto tree_file = target/L"folder"/L"nested"/L"tile.txt";
    const auto source_tree_file = root/L"folder"/L"nested"/L"tile.txt";
    const auto tree_bytes = Bytes(source_tree_file);
    dialog_action = [&](HWND window) {
        check(ui::SaveConfirmDialogFrame(window, (output/L"v4-tree-confirm-light.png").wstring()),
            "capture native V4 directory-tree confirmation");
        accept(window);
    };
    check(SetWindowTextW(ui->NodeWindow("entry-input"), L"folder") && click("copy-directory-tree"),
        "V4 submits a current-snapshot ordinary directory entry");
    if (!check(status(L"completed") && Bytes(tree_file) == tree_bytes && Bytes(source_tree_file) == tree_bytes &&
        fs::is_empty(target/L"folder"/L"empty") && label("progress-label").find(L"nested\\tile.txt: completed") != std::wstring::npos,
        "V4 copies nested bytes and empty directories and reports each file result")) return finish();
    check(ui->Snapshot((output/L"v4-tree-completed-light.png").wstring(), false, 1.0f, "progress-label"),
        "capture completed V4 per-file result in the application UI");
    dialog_action = accept;
    check(click("copy-directory-tree") && status(L"failed") && Bytes(tree_file) == tree_bytes,
        "V4 rejects an existing tree root and preserves the first copy without overwrite");
    check(ui->Snapshot((output/L"v4-tree-conflict-light.png").wstring(), false, 1.0f, "progress-label"),
        "capture V4 existing-root conflict state");

    dialog_action = [&](HWND) {
        const auto entries = EntriesForPackage(0, L"");
        check(entries.size() >= 2 && HandlePackageCommand(owner, entries.back().command), "normal package menu disables V2 DLL during confirmation");
    };
    check(select(L"disable.txt") && click("copy-selection"), "prepare outstanding request before disable");
    check(Wait([&] { return !OwnerActive(owner.hwnd) && !TransferUiPending(owner.hwnd); }) &&
        !fs::exists(target/L"disable.txt"), "disable revokes and cancels exact explicit-target request");
    check(opened == closed && opened == 12, "all twelve real native confirmations close exactly once");
    return finish();
}
}

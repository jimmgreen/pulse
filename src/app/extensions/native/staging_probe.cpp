#include "staging_probe.h"
#include "staging_adapter_host.h"
#include "command_contributions.h"
#include "../../app_runtime.h"
#include "../../app_ops_ui.h"
#include "../../../ui/confirm_dialog.h"
#include <filesystem>
#include <fstream>
#include <functional>
#include <ostream>
#include <windowsx.h>

namespace pulse::ui { bool SaveFileConflictDialogFrame(HWND, const std::wstring&); }
namespace pulse::extensions::native {
namespace {
std::function<void(HWND)> conflict_action;
std::function<void(HWND)> confirm_action;
void CALLBACK ConfirmTimer(HWND window, UINT, UINT_PTR timer, DWORD) {
    KillTimer(window, timer);
    if (confirm_action) confirm_action(window);
}
void CALLBACK DialogTimer(HWND, UINT, UINT_PTR, DWORD) {
    EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM) -> BOOL {
        wchar_t name[64]{}; GetClassNameW(window, name, 64);
        if (!wcscmp(name, L"PulseFileConflictWindow") && IsWindowVisible(window) && conflict_action) conflict_action(window);
        return TRUE;
    }, 0);
}
bool Wait(const std::function<bool()>& ready) {
    const auto until = GetTickCount64() + 30000;
    do {
        if (ready()) return true;
        MSG msg{};
        for (unsigned n = 0; n < 32 && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE); ++n) {
            if (msg.message == WM_QUIT) { PostQuitMessage(static_cast<int>(msg.wParam)); return false; }
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        MsgWaitForMultipleObjectsEx(0, nullptr, 5, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    } while (GetTickCount64() < until);
    return false;
}
std::string Read(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary); return {(std::istreambuf_iterator<char>(in)), {}};
}
}
bool RunArchiveStagingProbe(AppState& owner, const std::wstring& directory, std::ostream& report) {
    const std::filesystem::path output(directory), fixture = output.parent_path() / "fixture";
    if (!std::filesystem::exists(fixture / "sample.rar")) return true;
    const auto hello = fixture / "hello.txt";
    bool passed = true;
    const auto check = [&](bool ok, const char* text) { report << (ok ? "[PASS] " : "[FAIL] ") << text << '\n'; report.flush(); passed &= ok; return ok; };
    const auto clean = [&] {
        for (const auto& entry : std::filesystem::directory_iterator(fixture))
            if (entry.path().filename().wstring().starts_with(L".pulse-stage-")) return false;
        return !owner.ops.HasPendingOperations();
    };
    const auto invoke = [&](const wchar_t* identity) {
        auto* tab = ActiveTab(owner); if (!tab) return false;
        bool selected = false;
        for (size_t i = 0; i < tab->EntryCount(); ++i) if (tab->EntryAt(i).name == L"sample.7z") {
            tab->SelectOnly(static_cast<int>(i)); selected = true; break;
        }
        if (!selected) return false;
        for (const auto& row : CommandRows(owner)) if (CommandIdentity(owner, row.command) == identity)
            return HandleContributedCommand(owner, row.command);
        return false;
    };
    const auto task = [&] {
        uint64_t id = 0;
        for (const auto& item : owner.ops.UndoHistory().entries)
            if (!item.undone && item.entry.generated_output) id = std::max(id, item.entry.task_id);
        return id;
    };
    const auto output_selected = [&] {
        auto* tab = ActiveTab(owner);
        if (!tab || tab->loading || tab->SelectedCount() != 1) return false;
        for (size_t i = 0; i < tab->EntryCount(); ++i)
            if (tab->EntryAt(i).name == L"hello.txt" && tab->IsSelected(static_cast<int>(i))) return true;
        return false;
    };
    int confirmations = 0, conflicts = 0;
    bool approve = false, picker_approve = false, picker_shown = false;
    const UINT_PTR timer = SetTimer(owner.hwnd, 0x53544745, 20, DialogTimer);
    struct Reset {
        HWND owner; UINT_PTR timer;
        ~Reset() { if (timer) KillTimer(owner, timer); conflict_action = {}; confirm_action = {}; SetStagingConfirmObserver({}); SetStagingPickerObserver({}); }
    } reset{owner.hwnd, timer};
    confirm_action = [&](HWND window) {
        ++confirmations;
        check(ui::SaveConfirmDialogFrame(window, (output / "staging-confirm.png").wstring()), "capture host publication confirmation");
        PostMessageW(window, approve ? WM_KEYDOWN : WM_CLOSE, approve ? VK_RETURN : 0, 0);
    };
    SetStagingConfirmObserver([](HWND window) {
        // Capture the live content after its normal entrance animation settles.
        SetTimer(window, 0x53474346, 700, ConfirmTimer);
    });
    SetStagingPickerObserver([&](HWND window) {
        if (picker_shown) return;
        picker_shown = true;
        if (!picker_approve) PostMessageW(window, WM_CLOSE, 0, 0);
        else {
            RECT client{}; GetClientRect(window, &client); const float scale = owner.scale;
            const auto point = MAKELPARAM(static_cast<int>(client.right - 60 * scale), static_cast<int>(client.bottom - 30 * scale));
            PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, point); PostMessageW(window, WM_LBUTTONUP, 0, point);
        }
    });
    conflict_action = [&](HWND window) {
        ++conflicts;
        check(ui::SaveFileConflictDialogFrame(window, (output / "staging-conflict.png").wstring()), "capture real host name conflict");
        PostMessageW(window, WM_KEYDOWN, 'R', 0);
    };
    if (!check(!std::filesystem::exists(hello), "publication fixture starts without output")) return false;
    check(invoke(L"native:sample.native-archive:extract-to") && picker_shown && clean(), "cancel destination picker starts no extraction");
    check(invoke(L"native:sample.native-archive:extract-here") && Wait([&] { return confirmations == 1 && clean(); }) &&
        !std::filesystem::exists(hello), "cancel host confirmation cleans same-volume staging without publication");
    approve = true;
    check(invoke(L"native:sample.native-archive:extract-here") && Wait([&] { return Read(hello) == "Hello archive" && clean(); }),
        "actual plugin publishes real archive output through host operations");
    const auto published = task();
    check(published && owner.ops.CanUndoTask(published), "published task has host undo history");
    check(Wait(output_selected), "extracted output is automatically selected without locate action");
    if (GetEnvironmentVariableW(L"PULSE_TEST_STAGING_SELECTION_ONLY", nullptr, 0)) {
        check(invoke(L"native:sample.native-archive:extract-here") &&
            Wait([&] { return conflicts > 0 && Read(hello) == "Hello archive" && clean(); }),
            "second real extraction replaces output after reselecting archive");
        check(Wait(output_selected), "second extraction automatically selects output without locate action");
        return passed;
    }
    HandleOperationHistoryAction(owner, published, 1);
    check(Wait([&] { auto* tab = ActiveTab(owner); if (!tab || tab->loading) return false;
        for (size_t i = 0; i < tab->EntryCount(); ++i) if (tab->EntryAt(i).name == L"hello.txt" && tab->IsSelected(static_cast<int>(i))) return true;
        return false; }), "normal locate action selects published file after refresh");
    check(owner.ops.UndoTask(published) && Wait([&] { return !std::filesystem::exists(hello) && clean(); }), "undo removes output after staging was cleaned");
    check(owner.ops.Redo() && Wait([&] { return Read(hello) == "Hello archive" && clean(); }), "redo restores output without the staging source");
    owner.ops.Undo();
    if (!check(Wait([&] { return !std::filesystem::exists(hello) && clean(); }), "fresh output is removed before replacement fixture")) return false;
    { std::ofstream file(hello, std::ios::binary); file << "original document"; }
    check(invoke(L"native:sample.native-archive:extract-here") && Wait([&] { return Read(hello) == "Hello archive" && clean(); }) && conflicts > 0,
        "real conflict Replace publishes extracted content");
    check(Wait(output_selected), "reselecting archive and extracting again automatically selects replaced output");
    const auto replaced = task();
    check(owner.ops.UndoTask(replaced) && Wait([&] { return Read(hello) == "original document" && clean(); }), "undo restores original replaced content");
    check(owner.ops.Redo() && Wait([&] { return Read(hello) == "Hello archive" && clean(); }), "redo publishes replacement after temporary cleanup");
    owner.ops.Undo();
    if (!check(Wait([&] { return Read(hello) == "original document" && clean(); }), "restore original before final chooser case")) return false;
    std::filesystem::remove(hello);
    picker_shown = false; picker_approve = true;
    check(invoke(L"native:sample.native-archive:extract-to") && picker_shown &&
        Wait([&] { return Read(hello) == "Hello archive" && clean(); }), "accepted native folder picker publishes to chosen directory");
    owner.ops.Undo();
    check(Wait([&] { return !std::filesystem::exists(hello) && clean(); }), "final output undo leaves no staging directories");
    if (owner.operationWindow) owner.operationWindow->Hide();
    return passed;
}
}

#include "compression_probe.h"
#include "staging_adapter_host.h"
#include "command_contributions.h"
#include "settings_pages_adapter.h"
#include "session.h"
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
bool RunArchiveCompressionProbe(AppState& owner, const std::wstring& directory, std::ostream& report) {
    const std::filesystem::path output(directory), fixture = output.parent_path() / "fixture";

    const auto hello = fixture / "notes.txt.7z";
    const auto original_input = Read(fixture / "notes.txt");
    bool passed = true;
    const auto check = [&](bool ok, const char* text) { report << (ok ? "[PASS] " : "[FAIL] ") << text << '\n'; report.flush(); passed &= ok; return ok; };
    const auto clean = [&] {
        for (const auto& entry : std::filesystem::directory_iterator(fixture))
            if (entry.path().filename().wstring().starts_with(L".pulse-stage-")) return false;
        return !owner.ops.HasPendingOperations();
    };
    bool picker_shown = false;
    const auto invoke = [&](const wchar_t* identity) {
        picker_shown = false;
        auto* tab = ActiveTab(owner); if (!tab) return false;
        bool selected = false;
        for (size_t i = 0; i < tab->EntryCount(); ++i) if (tab->EntryAt(i).name == L"notes.txt") {
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
    int confirmations = 0, conflicts = 0;
    bool approve = false, picker_approve = false;
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
        const auto snapshot = (output / "compression-destination.png").wstring();
        const ATOM path = GlobalAddAtomW(snapshot.c_str());
        if (path) SetPropW(window, L"Pulse.PickerTestSnapshot", reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(path)));
        InvalidateRect(window, nullptr, FALSE);
        UpdateWindow(window);
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
    auto* pages = PackageSettings(owner.hwnd, 0);
    if (!pages || !pages->Page()) return false;
    PulseUiModelV1 model{}; pages->Ui().QueryModel(1, &model, sizeof(model));
    if (!check(Wait([&] {
        char buffer[2048]{}; uint32_t needed = 0;
        return model.get_state(model.context, pages->Page()->id, {"status", 6}, buffer, sizeof(buffer), &needed) == PULSE_OK &&
            std::string(buffer, needed).starts_with("Loaded settings revision ");
    }), "saved compression preferences finish loading")) return false;
    check(invoke(L"native:sample.native-archive:compress") && picker_shown && clean(), "cancel destination picker starts no compression");
    picker_approve = true;
    if (!check(invoke(L"native:sample.native-archive:compress") && Wait([&] { return confirmations == 1 && clean(); }) &&
        !std::filesystem::exists(hello), "cancel host confirmation cleans same-volume staging without publication")) {
        const auto status = owner.ops.Status();
        report << "Task " << status.task_id << " outcome " << static_cast<int>(status.external_outcome) << '\n';
        const int length = WideCharToMultiByte(CP_UTF8, 0, status.last_error.data(), static_cast<int>(status.last_error.size()), nullptr, 0, nullptr, nullptr);
        std::string error(static_cast<size_t>(length), '\0');
        if (length) WideCharToMultiByte(CP_UTF8, 0, status.last_error.data(), static_cast<int>(status.last_error.size()), error.data(), length, nullptr, nullptr);
        report << "Error: " << error << '\n';
        return false;
    }
    approve = true;
    check(invoke(L"native:sample.native-archive:compress") && Wait([&] { return Read(hello).starts_with(std::string("7z\xbc\xaf\x27\x1c", 6)) && clean(); }),
        "actual plugin publishes real archive output through host operations");
    const auto published = task();
    check(published && owner.ops.CanUndoTask(published), "published task has host undo history");
    HandleOperationHistoryAction(owner, published, 1);
    check(Wait([&] { auto* tab = ActiveTab(owner); if (!tab || tab->loading) return false;
        for (size_t i = 0; i < tab->EntryCount(); ++i) if (tab->EntryAt(i).name == L"notes.txt.7z" && tab->IsSelected(static_cast<int>(i))) return true;
        return false; }), "normal locate action selects published file after refresh");
    check(owner.ops.UndoTask(published) && Wait([&] { return !std::filesystem::exists(hello) && clean(); }), "undo removes output after staging was cleaned");
    check(owner.ops.Redo() && Wait([&] { return Read(hello).starts_with(std::string("7z\xbc\xaf\x27\x1c", 6)) && clean(); }), "redo restores output without the staging source");
    owner.ops.Undo();
    if (!check(Wait([&] { return !std::filesystem::exists(hello) && clean(); }), "fresh output is removed before replacement fixture")) return false;
    { std::ofstream file(hello, std::ios::binary); file << "original document"; }
    check(invoke(L"native:sample.native-archive:compress") && Wait([&] { return Read(hello).starts_with(std::string("7z\xbc\xaf\x27\x1c", 6)) && clean(); }) && conflicts > 0,
        "real conflict Replace publishes compressed content");
    const auto replaced = task();
    check(owner.ops.UndoTask(replaced) && Wait([&] { return Read(hello) == "original document" && clean(); }), "undo restores original replaced content");
    check(owner.ops.Redo() && Wait([&] { return Read(hello).starts_with(std::string("7z\xbc\xaf\x27\x1c", 6)) && clean(); }), "redo publishes replacement after temporary cleanup");
    owner.ops.Undo();
    if (!check(Wait([&] { return Read(hello) == "original document" && clean(); }), "restore original before final chooser case")) return false;
    std::filesystem::remove(hello);
    picker_shown = false; picker_approve = true;
    check(invoke(L"native:sample.native-archive:compress") && picker_shown &&
        Wait([&] { return Read(hello).starts_with(std::string("7z\xbc\xaf\x27\x1c", 6)) && clean(); }), "accepted native folder picker publishes to chosen directory");
    owner.ops.Undo();
    check(Wait([&] { return !std::filesystem::exists(hello) && clean(); }), "final output undo leaves no staging directories");
    const PulseUiState preferences[]{{{"format", 6}, {"1", 1}}, {{"level", 5}, {"3", 1}}};
    check(model.set_states(model.context, pages->Page()->id, preferences, 2) == PULSE_OK,
        "settings page accepts ZIP and compression level draft");
    SendMessageW(pages->Ui().NodeWindow("save"), BM_CLICK, 0, 0);
    if (!check(Wait([&] {
        char buffer[2048]{}; uint32_t needed = 0;
        return model.get_state(model.context, pages->Page()->id, {"status", 6}, buffer, sizeof(buffer), &needed) == PULSE_OK &&
            std::string(buffer, needed).starts_with("Saved settings revision ");
    }), "existing settings Save persists ZIP preferences")) return false;
    const auto zip = fixture / "notes.txt.zip";
    check(invoke(L"native:sample.native-archive:compress") && picker_shown &&
        Wait([&] { return Read(zip).starts_with(std::string("PK\x03\x04", 4)) && clean(); }),
        "saved ZIP preference produces ZIP through normal publication");
    check(!std::filesystem::exists(hello), "ZIP preference does not publish a 7z output");
    check(Read(fixture / "notes.txt") == original_input, "compression and undo preserve source bytes");
    if (owner.operationWindow) owner.operationWindow->Hide();
    return passed;
}
}



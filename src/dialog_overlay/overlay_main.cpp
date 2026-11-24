#include "companion_bar.h"
#include "dialog_classifier.h"
#include "../common/dialog_bridge_state.h"

#include <windows.h>
#include <shlobj.h>
#include <string>
#include <vector>

namespace {

namespace overlay = pulse::dialog_overlay;
namespace state_ns = pulse::dialog_bridge_state;

overlay::CompanionBar g_bar;
state_ns::StateReader g_state_reader;
state_ns::ActiveDialogLedger g_ledger;
HWINEVENTHOOK g_show_hook = nullptr;
HWINEVENTHOOK g_move_hook = nullptr;

std::wstring KnownFolderPath(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw)) || !raw) return {};
    std::wstring out(raw);
    CoTaskMemFree(raw);
    return out;
}

std::vector<overlay::QuickJumpEntry> DefaultQuickJumps() {
    std::vector<overlay::QuickJumpEntry> list;
    auto add = [&](const wchar_t* label, REFKNOWNFOLDERID id) {
        auto p = KnownFolderPath(id);
        if (!p.empty()) list.push_back({label, std::move(p)});
    };
    add(L"Desktop", FOLDERID_Desktop);
    add(L"Downloads", FOLDERID_Downloads);
    add(L"Documents", FOLDERID_Documents);
    return list;
}

bool OverlayEnabledInState(bool& mode_b_out) noexcept {
    mode_b_out = false;
    state_ns::StateSnapshot snap;
    if (!g_state_reader.IsOpen()) {
        g_state_reader.Open();
    }
    if (!g_state_reader.Read(snap)) {
        // When Broker state page is absent (e.g., standalone run), default to Mode A enabled.
        return true;
    }
    mode_b_out = (snap.flags & state_ns::kFlagOverlayModeB) != 0;
    return (snap.flags & state_ns::kFlagOverlayEnabled) != 0;
}

void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG id_object, LONG id_child,
                         DWORD, DWORD) {
    if (id_object != OBJID_WINDOW || id_child != CHILDID_SELF || !hwnd) return;

    if (event == EVENT_OBJECT_HIDE || event == EVENT_OBJECT_DESTROY) {
        if (hwnd == g_bar.TargetHwnd()) {
            g_bar.Detach();
        }
        return;
    }
    if (event == EVENT_OBJECT_LOCATIONCHANGE) {
        if (hwnd == g_bar.TargetHwnd()) {
            g_bar.UpdatePosition();
        }
        return;
    }
    if (event != EVENT_OBJECT_SHOW) return;

    bool mode_b_enabled = false;
    if (!OverlayEnabledInState(mode_b_enabled)) return;

    const auto info = overlay::ClassifyNativeDialog(hwnd, &g_ledger);
    if (info.kind == overlay::NativeDialogKind::NotAFileDialog ||
        info.kind == overlay::NativeDialogKind::AlreadyTakenOverByPulse) {
        return;
    }

    // Default Mode A: non-focus companion jump bar docked above the native dialog.
    g_bar.AttachTo(info, DefaultQuickJumps());
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    // Prevent duplicate overlay instances per user session.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\PulseDialogOverlaySingleton");
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_state_reader.Open();
    g_ledger.OpenOrCreate();

    if (!g_bar.Create(instance)) {
        CoUninitialize();
        CloseHandle(mutex);
        return 1;
    }

    g_show_hook = SetWinEventHook(
        EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE, nullptr, &OnWinEvent,
        0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    g_move_hook = SetWinEventHook(
        EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, &OnWinEvent,
        0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_move_hook) UnhookWinEvent(g_move_hook);
    if (g_show_hook) UnhookWinEvent(g_show_hook);
    g_bar.Destroy();
    g_ledger.Close();
    g_state_reader.Close();
    CoUninitialize();
    CloseHandle(mutex);
    return 0;
}

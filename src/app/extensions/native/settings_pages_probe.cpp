#include "settings_pages_probe.h"
#include "settings_pages_adapter.h"
#include "session.h"
#include "resources_adapter.h"
#include "../runtime.h"
#include "../../app_runtime.h"
#include "../../app_commands.h"
#include "../../app_navigation.h"
#include "../../../ui/settings_page_ids.h"
#include <filesystem>
#include <functional>
#include <cstring>
#include <windowsx.h>
#include <ostream>

namespace pulse::extensions::native {
namespace {
PulseStringView View(const char* text) { return {text, static_cast<uint32_t>(std::strlen(text))}; }
PulseStringView View(const std::string& text) { return {text.data(), static_cast<uint32_t>(text.size())}; }
bool Wait(const std::function<bool()>& ready) {
    const auto end = GetTickCount64() + 15000;
    do {
        MSG msg{};
        for (unsigned n = 0; n < 32 && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE); ++n) {
            if (msg.message == WM_QUIT) { PostQuitMessage(static_cast<int>(msg.wParam)); return false; }
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        if (ready()) return true;
        MsgWaitForMultipleObjectsEx(0, nullptr, 5, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    } while (GetTickCount64() < end);
    return false;
}
struct Reply {
    bool done = false; PulseStatus status = PULSE_ERROR_STATE; uint64_t version = 0;
    static void PULSE_CALL Complete(void* user, const PulseResourcesReply* value) {
        auto& self = *static_cast<Reply*>(user); self.status = value->status;
        self.version = value->settings.version; self.done = true;
    }
};
}
bool RunSettingsPagesProbe(AppState& owner, const std::wstring& directory, std::ostream& report) {
    bool passed = true;
    auto check = [&](bool ok, const char* text) { report << (ok ? "[PASS] " : "[FAIL] ") << text << '\n'; report.flush(); passed &= ok; return ok; };
    const std::filesystem::path output(directory);
    auto* pages = PackageSettings(owner.hwnd, 0);
    if (!check(pages && pages->Page(), "installed DLL registers independent settings page")) return false;
    const auto id = pages->Page()->id;
    const HWND window = pages->Ui().Window();
    check(window && !IsWindowVisible(window) && GetParent(window) == owner.hwnd, "settings starts hidden inside owner without a popup");
    PulseUiModelV1 ui{};
    pages->Ui().QueryModel(1, &ui, sizeof(ui));
    auto read = [&](const char* key) {
        char buffer[2048]{}; uint32_t needed = 0;
        if (ui.get_state(ui.context, id, View(key), buffer, sizeof(buffer), &needed) != PULSE_OK) return std::string{};
        return std::string(buffer, needed);
    };
    if (!check(Wait([&] { return read("status").starts_with("Loaded settings revision "); }), "asynchronous settings load completes")) return false;
    wchar_t restart[4]{};
    if (GetEnvironmentVariableW(L"PULSE_TEST_SETTINGS_RESTART", restart, 4))
        check(read("format") == "1" && read("level") == "3" && !read("destination").empty(), "saved preferences survive process restart and package revision change");
    OpenSettingsTab(owner, 8); BuildVm(owner);
    const auto settled = GetTickCount64() + 700;
    Wait([&] { return GetTickCount64() >= settled; });
    const bool copy_before = owner.compositor.FrameCopyEnabled();
    owner.compositor.SetFrameCopyEnabled(true);
    RedrawWindow(owner.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    check(owner.compositor.SaveSnapshot((output / "settings-entry.png").c_str()), "capture ordinary enhancement-pack settings entry");
    owner.compositor.SetFrameCopyEnabled(copy_before);
    ui::HitTestResult hit{}; hit.region = ui::HitTestResult::SettingsPackAction; hit.index = static_cast<int>(ui::PackAction::NativeSettings);
    check(HandleSettingsControl(owner, hit), "ordinary enhancement-pack settings entry handles click");
    auto vm = BuildVm(owner);
    check(vm.settings_page == ui::kSettingsPageNativeExtensions && IsWindowVisible(window), "settings page is mounted below the host settings heading");
    SetFocus(pages->Ui().NodeWindow("destination-input"));
    const HWND focused = GetFocus();
    BuildVm(owner); BuildVm(owner);
    check(focused && GetFocus() == focused, "repeated host updates preserve editor focus");
    const std::string destination = (output.parent_path() / "fixture").string();
    const PulseUiState invalid{View("destination"), View("relative/path")};
    ui.set_states(ui.context, id, &invalid, 1);
    SendMessageW(pages->Ui().NodeWindow("save"), BM_CLICK, 0, 0);
    check(read("status").starts_with("Use an empty destination or a local absolute drive path.") && read("destination") == "relative/path",
        "invalid destination is rejected before storage and preserves the draft");
    const PulseUiState changes[]{{View("destination"), View(destination)}, {View("format"), View("1")}, {View("level"), View("3")}};
    check(ui.set_states(ui.context, id, changes, 3) == PULSE_OK, "native settings controls accept draft preferences");
    OpenSettingsTab(owner, 8); BuildVm(owner);
    check(!IsWindowVisible(window), "leaving settings hides embedded plugin controls");
    OpenSettingsTab(owner, ui::kSettingsPageNativeExtensions); BuildVm(owner);
    check(IsWindowVisible(window) && read("format") == "1" && read("destination") == destination, "returning preserves the unsaved draft");
    const auto binding = CaptureCallerBinding(0, reinterpret_cast<uintptr_t>(owner.hwnd));
    if (!binding) return false;
    ResourcesAdapter external(owner, *binding); PulseResourcesV1 resources{}; external.Query(1, &resources, sizeof(resources));
    Reply reply; PulseResourcesRequestId request = 0;
    resources.settings_get(resources.context, Reply::Complete, &reply, &request);
    if (!check(Wait([&] { return reply.done; }) && reply.status == PULSE_OK, "second settings client reads current revision")) return false;
    const auto version = reply.version; reply = {};
    resources.settings_set(resources.context, View("archive.preferences"), View("1\n0\n1\n"), 0, version, Reply::Complete, &reply, &request);
    if (!check(Wait([&] { return reply.done; }) && reply.status == PULSE_OK, "second client creates a genuine optimistic-version conflict")) return false;
    SendMessageW(pages->Ui().NodeWindow("save"), BM_CLICK, 0, 0);
    check(Wait([&] { return read("status").starts_with("New revision loaded. Draft retained."); }) && read("format") == "1" && read("destination") == destination,
        "conflicting save retains draft and requires explicit retry");
    SendMessageW(pages->Ui().NodeWindow("save"), BM_CLICK, 0, 0);
    check(Wait([&] { return read("status").starts_with("Saved settings revision "); }), "real Save button persists settings after conflict review");
    const auto scroll_top = [&] {
        const HWND tree = GetParent(pages->Ui().NodeWindow("save"));
        RECT area{}; GetWindowRect(tree, &area);
        const LPARAM point = MAKELPARAM((area.left + area.right) / 2, (area.top + area.bottom) / 2);
        for (int i = 0; i < 20; ++i) SendMessageW(tree, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), point);
    };
    scroll_top();
    check(pages->Ui().SnapshotCurrent((output / "settings-page.png").wstring()), "capture actual hosted native settings controls");
    RECT original{}; GetWindowRect(owner.hwnd, &original); const bool old_dark = owner.darkMode;
    owner.darkMode = true;
    SetWindowPos(owner.hwnd, nullptr, 0, 0, 760, 720, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE); BuildVm(owner);
    scroll_top();
    check(pages->Ui().SnapshotCurrent((output / "settings-page-dark-narrow.png").wstring()), "capture dark narrow hosted settings page");
    const HWND save = pages->Ui().NodeWindow("save");
    const HWND tree = GetParent(save);
    RECT viewport{}; GetWindowRect(tree, &viewport);
    const LPARAM point = MAKELPARAM((viewport.left + viewport.right) / 2, (viewport.top + viewport.bottom) / 2);
    for (int i = 0; i < 20; ++i) SendMessageW(tree, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), point);
    RECT button{}; GetWindowRect(save, &button);
    check(button.top >= viewport.top && button.bottom <= viewport.bottom && button.right > button.left,
        "narrow settings scroll reaches the real Save button");
    check(pages->Ui().SnapshotCurrent((output / "settings-page-dark-scrolled.png").wstring()), "capture reachable narrow settings actions");
    owner.darkMode = old_dark;
    SetWindowPos(owner.hwnd, nullptr, 0, 0, original.right-original.left, original.bottom-original.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE); BuildVm(owner);
    StopPackage(owner.hwnd, 0); BuildVm(owner);
    check(!IsWindow(window) && !PackageSettings(owner.hwnd, 0), "stopping plugin removes settings controls and callbacks");
    check(StartPackage(owner, 0), "plugin restarts without losing its settings store");
    pages = PackageSettings(owner.hwnd, 0);
    if (!check(pages && pages->Page() && pages->Page()->id != id, "reloaded settings page gets a fresh identity")) return false;
    PulseUiModelV1 again{}; pages->Ui().QueryModel(1, &again, sizeof(again)); const auto again_id = pages->Page()->id;
    check(Wait([&] { char buffer[2048]{}; uint32_t needed = 0;
        return again.get_state(again.context, again_id, View("destination"), buffer, sizeof(buffer), &needed) == PULSE_OK && std::string(buffer, needed) == destination;
    }), "plugin reload reads the persisted destination");
    OpenSettingsTab(owner, 8); BuildVm(owner);
    return passed;
}
}

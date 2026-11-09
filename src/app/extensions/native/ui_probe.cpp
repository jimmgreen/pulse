#include "ui_probe.h"
#include "inspector_probe.h"
#include "reactive_probe.h"
#include "data_probe.h"
#include "directory_probe.h"
#include "fileops_probe.h"
#include "target_probe.h"
#include "workbench_probe.h"
#include "session.h"
#include "ui_adapter.h"
#include "../rename_service.h"
#include "../../app_runtime.h"
#include "../../../ui/ui_renderer.h"
#include <filesystem>
#include <fstream>
#include <map>
void Render(pulse::AppState&);
namespace pulse::extensions::native {
namespace {
bool Wait(const std::function<bool()>& ready, DWORD timeout = 15000) {
    const auto deadline = GetTickCount64() + timeout;
    while (GetTickCount64() < deadline) {
        MSG message{};
        for (unsigned n = 0; n < 32 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++n) {
            if (message.message == WM_QUIT) { PostQuitMessage(static_cast<int>(message.wParam)); return false; }
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        if (ready()) return true;
        MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
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
bool RunUiProbe(AppState& owner, const std::wstring& directory) {
    if (const auto* manifest = CurrentManifest(); manifest && manifest->id == "sample.native-directory-workbench")
        return RunWorkbenchProbe(owner, directory);
    if (const auto* manifest = CurrentManifest(); manifest && manifest->id == "sample.native-directory-transfer")
        return RunTargetProbe(owner, directory);
    if (const auto* manifest = CurrentManifest(); manifest && manifest->id == "sample.native-file-operations")
        return RunFileOpsProbe(owner, directory);
    if (const auto* manifest = CurrentManifest(); manifest && manifest->id == "sample.native-directory-browser")
        return RunDirectoryProbe(owner, directory);
    if (const auto* manifest = CurrentManifest(); manifest && manifest->id == "sample.native-live-data")
        return RunDataProbe(owner, directory);
    if (const auto* manifest = CurrentManifest(); manifest && manifest->id == "sample.native-reactive")
        return RunReactiveProbe(owner, directory);
    if (const auto* manifest = CurrentManifest(); manifest && manifest->id == "sample.native-inspector")
        return RunInspectorProbe(owner, directory);
    if (!owner.isolatedTest || !GetEnvironmentVariableW(L"PULSE_TEST_NATIVE_CASE", nullptr, 0)) return false;
    auto* ui = OwnerUi(owner.hwnd);
    if (!ui || !OwnerActive(owner.hwnd)) return false;
    const std::filesystem::path output(directory);
    if (!output.is_absolute()) return false;
    std::filesystem::create_directories(output);
    std::ofstream report(output / L"report.txt");
    std::ofstream(output / L"activation-metrics.json") << json::Serialize(OwnerMetrics(owner.hwnd));
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        report << (ok ? "[PASS] " : "[FAIL] ") << label << '\n'; report.flush();
        if (!ok) ++failures;
        return ok;
    };
    auto finish = [&] { SetRenameReviewProbe({}); report << "Result: " << (failures ? "FAIL" : "PASS") << '\n'; return failures == 0; };
    if (!check(Wait([&] { const auto* tab = ActiveTab(owner); return tab && !tab->loading && !tab->pending_generation && tab->snapshot; }),
        "isolated native plugin host listing ready")) return finish();
    const auto folder = std::filesystem::path(ActiveTab(owner)->current_path);
    std::map<std::wstring, std::string> before;
    for (const auto& item : std::filesystem::directory_iterator(folder)) before.emplace(item.path().filename().wstring(), Bytes(item.path()));
    if (!check(before.size() == 2 && before.contains(L"image alpha.jpg") && before.contains(L"Notes \u03b2.txt"),
        "test touches only two known isolated fixtures")) return finish();
    auto message = [&] { return Text(ui->NodeWindow("message-text")); };
    auto diagnose = [&] { report << "[INFO] message=" << json::Utf8(message()) << '\n'; report.flush(); };
    auto click = [&](const char* id) {
        const HWND button = ui->NodeWindow(id);
        if (!IsWindow(button) || !IsWindowEnabled(button)) return false;
        SendMessageW(button, BM_CLICK, 0, 0); return true;
    };
    if (!check(IsWindow(ui->NodeWindow("prefix-input")), "DLL creates an actual native input")) return finish();
    SetWindowTextW(ui->NodeWindow("prefix-input"), L"demo-");
    auto prepare = [&] {
        if (!click("preview-button")) return false;
        const bool ok = Wait([&] { return message().find(L"Preview ready.") != std::wstring::npos; });
        diagnose(); return ok;
    };
    if (!check(prepare(), "native DLL computes names and receives a typed rename plan")) return finish();
    check(ui->Snapshot((output / L"prepared-panel.png").wstring(), owner.darkMode, owner.scale), "capture actual DLL-created native UI");
    bool refused = false;
    SetRenameReviewProbe([&](HWND dialog, RenameReviewSnapshot snapshot) {
        check(snapshot((output / L"native-refusal.png").wstring()), "capture native review before refusal");
        refused = true; PostMessageW(dialog, WM_KEYDOWN, VK_ESCAPE, 0);
    });
    check(click("commit-button") && Wait([&] { return refused && message().find(L"cancelled") != std::wstring::npos; }),
        "actual native review refusal returns to the DLL");
    bool unchanged = true;
    for (const auto& [name, bytes] : before) unchanged &= std::filesystem::exists(folder / name) && Bytes(folder / name) == bytes;
    check(unchanged, "refusing review leaves every input byte and name unchanged");
    if (!check(prepare(), "DLL creates a fresh plan after refusal")) return finish();
    bool confirmed = false;
    SetRenameReviewProbe([&](HWND dialog, RenameReviewSnapshot snapshot) {
        check(snapshot((output / L"native-confirm.png").wstring()), "capture native review before confirmation");
        confirmed = true; PostMessageW(dialog, WM_KEYDOWN, VK_RETURN, 0);
    });
    if (!check(click("commit-button") && Wait([&] { return confirmed && message().find(L"Task submitted.") != std::wstring::npos; }),
        "native confirmation submits DLL-generated names to real operation engine")) { diagnose(); return finish(); }
    ULONGLONG next = 0;
    check(Wait([&] {
        if (message() == L"Task completed.") return true;
        if (GetTickCount64() >= next) { next = GetTickCount64() + 150; click("status-button"); }
        return false;
    }), "DLL queries its real task until completed");
    diagnose();
    check(ui->Snapshot((output / L"completed-panel.png").wstring(), owner.darkMode, owner.scale), "capture completed native plugin result");
    bool old_gone = true;
    for (const auto& [name, bytes] : before) { (void)bytes; old_gone &= !std::filesystem::exists(folder / name); }
    check(old_gone, "operation changed actual fixture names");
    Wait([&] {
        const auto* tab = ActiveTab(owner);
        if (!tab || tab->loading || !tab->snapshot || tab->snapshot->size() != 2) return false;
        for (const auto& entry : *tab->snapshot) if (entry.name.find(L"demo-") != 0) return false;
        return true;
    });
    Render(owner); Render(owner);
    check(owner.compositor.SaveSnapshot((output / L"renamed-host.png").c_str()), "capture actual host after directory refresh");
    StopOwner(owner.hwnd);
    check(!OwnerActive(owner.hwnd) && !OwnerUi(owner.hwnd), "native plugin deactivates and releases its host UI");
    return finish();
}
}

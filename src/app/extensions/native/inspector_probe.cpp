#include "inspector_probe.h"
#include "session.h"
#include "ui_adapter.h"
#include "resources_adapter.h"
#include "../../app_runtime.h"
#include <filesystem>
#include <fstream>
#include <map>
namespace pulse::extensions::native {
namespace {
bool Wait(const std::function<bool()>& ready) {
    const auto deadline = GetTickCount64() + 15000;
    do {
        MSG message{};
        for (unsigned n = 0; n < 32 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++n) {
            if (message.message == WM_QUIT) { PostQuitMessage(static_cast<int>(message.wParam)); return false; }
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        if (ready()) return true;
        MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    } while (GetTickCount64() < deadline);
    return false;
}
std::wstring Text(HWND window) {
    if (!window) return {};
    std::wstring value(static_cast<size_t>(GetWindowTextLengthW(window)) + 1, wchar_t{});
    value.resize(static_cast<size_t>(GetWindowTextW(window, value.data(), static_cast<int>(value.size()))));
    return value;
}
std::string Bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
PulseStringView View(const char* value) { return {value, static_cast<uint32_t>(std::char_traits<char>::length(value))}; }
struct Capture {
    bool done = false;
    PulseStatus status = PULSE_ERROR_STATE;
    uint64_t version = 0;
    std::string option;
    static void PULSE_CALL Complete(void* pointer, const PulseResourcesReply* reply) {
        auto& self = *static_cast<Capture*>(pointer);
        self.status = reply->status; self.version = reply->settings.version; self.option.clear();
        for (uint32_t i = 0; i < reply->settings.count; ++i) {
            const auto& value = reply->settings.values[i];
            if (std::string_view(value.key.data, value.key.length) == "display.sort")
                self.option.assign(value.value.data, value.value.length);
        }
        self.done = true;
    }
};
}
bool RunInspectorProbe(AppState& owner, const std::wstring& directory) {
    if (!owner.isolatedTest || !GetEnvironmentVariableW(L"PULSE_TEST_NATIVE_CASE", nullptr, 0)) return false;
    auto* ui = OwnerUi(owner.hwnd);
    if (!ui || !OwnerActive(owner.hwnd)) return false;
    const std::filesystem::path output(directory);
    if (!output.is_absolute()) return false;
    std::filesystem::create_directories(output);
    std::ofstream report(output / L"report.txt");
    std::ofstream(output / L"activation-metrics.json") << json::Serialize(OwnerMetrics(owner.hwnd));
    int failures = 0;
    auto check = [&](bool ok, const char* label) { report << (ok ? "[PASS] " : "[FAIL] ") << label << '\n'; report.flush(); if (!ok) ++failures; return ok; };
    auto finish = [&] { report << "Result: " << (failures ? "FAIL" : "PASS") << '\n'; return failures == 0; };
    if (!check(Wait([&] { const auto* tab = ActiveTab(owner); return tab && !tab->loading && tab->snapshot && !tab->pending_generation; }),
        "isolated native inspector directory is ready")) return finish();
    const auto folder = std::filesystem::path(ActiveTab(owner)->current_path);
    std::map<std::wstring, std::string> before;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) before.emplace(entry.path().filename().wstring(), Bytes(entry.path()));
    if (!check(before.size() == 3 && before.contains(L"one.txt") && before.contains(L"TWO.TXT") && before.contains(L"three.bin"),
        "inspector test is confined to three known isolated files")) return finish();
    auto message = [&] { return Text(ui->NodeWindow("message-text")); };
    auto click = [&](const char* id) {
        const HWND button = ui->NodeWindow(id);
        if (!IsWindow(button) || !IsWindowEnabled(button)) return false;
        SendMessageW(button, BM_CLICK, 0, 0); return true;
    };
    auto result = [&](const char* id, const std::wstring& expected) {
        const bool ok = click(id) && Wait([&] { return message() == expected; });
        report << "[INFO] " << id << "=" << json::Utf8(message()) << '\n'; report.flush(); return ok;
    };
    if (!check(result("analyze-button", L"Analysis complete: 3 files, 2 extensions."), "DLL aggregates real file metadata into its own extension groups")) return finish();
    check(IsWindow(ui->NodeWindow("result-table")), "custom algorithm results use the actual native DataTable");
    check(ui->Snapshot((output / L"analysis-light.png").wstring(), false, 1.0f), "capture real model table in light theme");
    check(ui->Snapshot((output / L"analysis-dark.png").wstring(), true, 1.5f), "capture real model table in dark theme and 150 percent DPI");
    const auto bytes = ActiveTab(owner)->snapshot->front().size;
    check(result("stat-button", L"Stat complete: " + std::to_wstring(bytes) + L" bytes."), "DLL reads file stat through an opaque entry reference");
    if (!check(result("load-settings-button", L"Settings loaded: version 0."), "DLL reads its empty versioned settings namespace")) return finish();
    SetWindowTextW(ui->NodeWindow("option-input"), L"count");
    if (!check(result("save-settings-button", L"Settings saved: version 1."), "DLL persists its own setting with expected version")) return finish();
    const auto caller = CaptureCallerBinding(0, reinterpret_cast<uintptr_t>(owner.hwnd));
    if (!check(caller.has_value(), "second typed attachment uses the same actual package namespace")) return finish();
    Capture capture;
    ResourcesAdapter second(owner, *caller);
    const auto api = second.Table();
    PulseResourcesRequestId request = 0;
    bool accepted = api.settings_get(api.context, Capture::Complete, &capture, &request) == PULSE_OK;
    check(accepted && Wait([&] { return capture.done; }) && capture.status == PULSE_OK && capture.version == 1 && capture.option == "count",
        "persisted value is read back through a separate native attachment");
    capture.done = false;
    accepted = api.settings_set(api.context, View("display.sort"), View("bytes"), 0, 1, Capture::Complete, &capture, &request) == PULSE_OK;
    if (!check(accepted && Wait([&] { return capture.done; }) && capture.status == PULSE_OK && capture.version == 2,
        "competing actual writer advances the settings version")) return finish();
    check(result("save-settings-button", L"Settings conflict: reload before saving; nothing overwritten."),
        "stale DLL settings write reports a real version conflict");
    check(result("load-settings-button", L"Settings loaded: version 2.") && Text(ui->NodeWindow("option-input")) == L"bytes",
        "reload observes competing value instead of silently overwriting it");
    check(click("audit-button") && Wait([&] { const auto text = message(); return text.starts_with(L"Audit complete: ") && text != L"Audit complete: 0 own records."; }),
        "DLL queries nonempty real caller-owned audit records");
    check(ui->Snapshot((output / L"resources-complete.png").wstring(), owner.darkMode, owner.scale), "capture native resource results");
    bool unchanged = true;
    for (const auto& [name, content] : before) unchanged &= std::filesystem::exists(folder / name) && Bytes(folder / name) == content;
    check(unchanged, "analysis stat and settings do not change user fixture files");
    second.Close(); StopOwner(owner.hwnd);
    check(!OwnerActive(owner.hwnd), "native inspector detaches cleanly");
    return finish();
}
}

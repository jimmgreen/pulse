#include "reactive_probe.h"
#include "session.h"
#include "ui_adapter.h"
#include "../../app_runtime.h"
#include <filesystem>
#include <fstream>
namespace pulse::extensions::native {
namespace {
void Pump() {
    MSG message{};
    for (unsigned n = 0; n < 32 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++n) {
        if (message.message == WM_QUIT) { PostQuitMessage(static_cast<int>(message.wParam)); return; }
        TranslateMessage(&message); DispatchMessageW(&message);
    }
}
bool Wait(const std::function<bool()>& ready) {
    const auto deadline = GetTickCount64() + 15000;
    do { Pump(); if (ready()) return true; MsgWaitForMultipleObjectsEx(0, nullptr, 5, QS_ALLINPUT, MWMO_INPUTAVAILABLE); }
    while (GetTickCount64() < deadline);
    return false;
}
std::wstring Text(HWND window) {
    if (!window) return {};
    std::wstring text(static_cast<size_t>(GetWindowTextLengthW(window)) + 1, wchar_t{});
    text.resize(static_cast<size_t>(GetWindowTextW(window, text.data(), static_cast<int>(text.size()))));
    return text;
}
}
bool RunReactiveProbe(AppState& owner, const std::wstring& directory) {
    if (!owner.isolatedTest || !GetEnvironmentVariableW(L"PULSE_TEST_NATIVE_CASE", nullptr, 0)) return false;
    auto* ui = OwnerUi(owner.hwnd);
    if (!ui || !OwnerActive(owner.hwnd)) return false;
    const std::filesystem::path output(directory);
    if (!output.is_absolute()) return false;
    std::filesystem::create_directories(output);
    std::ofstream report(output / L"report.txt");
    auto metrics = OwnerMetrics(owner.hwnd);
    int failures = 0;
    auto check = [&](bool ok, const char* label) { report << (ok ? "[PASS] " : "[FAIL] ") << label << '\n'; report.flush(); if (!ok) ++failures; return ok; };
    auto finish = [&] { std::ofstream(output / L"metrics.json") << json::Serialize(metrics); report << "Result: " << (failures ? "FAIL" : "PASS") << '\n'; return failures == 0; };
    if (!check(IsWindow(ui->NodeWindow("query-input")), "DLL creates real input using only public C++ wrappers")) return finish();
    auto result = [&] { return Text(ui->NodeWindow("result-text")); };
    auto status = [&] { return Text(ui->NodeWindow("message-text")); };
    const auto started = GetTickCount64();
    SetWindowTextW(ui->NodeWindow("query-input"), L"alpha beta \u03b2");
    if (!check(Wait([&] { return result() == L"ALPHA BETA \u03b2" && status() == L"Complete: 3 tokens, 12 code points."; }),
        "actual edit event runs DLL worker and dispatches Unicode-preserving result to UI")) {
        report << "[INFO] status=" << json::Utf8(status()) << " result=" << json::Utf8(result()) << '\n'; return finish();
    }
    metrics.set("firstObservedInputCompletionMs", static_cast<uint64_t>(GetTickCount64() - started));
    check(ui->Snapshot((output / L"reactive-light.png").wstring(), false, 1.0f), "capture actual reactive native controls");
    for (unsigned i = 0; i < 32; ++i) {
        const auto value = L"rapid input " + std::to_wstring(i);
        SetWindowTextW(ui->NodeWindow("query-input"), value.c_str()); Pump();
    }
    SetWindowTextW(ui->NodeWindow("query-input"), L"final \u03b2 value");
    if (!check(Wait([&] { return result() == L"FINAL \u03b2 VALUE" && status() == L"Complete: 3 tokens, 13 code points."; }),
        "rapid edits converge to latest committed input rather than an old worker result")) return finish();
    const auto stable_until = GetTickCount64() + 150;
    bool stable = true;
    while (GetTickCount64() < stable_until) { Pump(); stable &= result() == L"FINAL \u03b2 VALUE"; MsgWaitForMultipleObjectsEx(0, nullptr, 5, QS_ALLINPUT, MWMO_INPUTAVAILABLE); }
    check(stable, "late queued work cannot overwrite the final result");
    check(ui->Snapshot((output / L"reactive-dark.png").wstring(), true, 1.5f), "capture reactive dark theme at 150 percent DPI");
    SetWindowTextW(ui->NodeWindow("query-input"), L"");
    check(Wait([&] { return result().empty() && status() == L"Complete: 0 tokens, 0 code points."; }), "empty input completes without stale output");
    const std::wstring pending(1000, L'x');
    SetWindowTextW(ui->NodeWindow("query-input"), pending.c_str());
    const auto close_started = GetTickCount64();
    StopOwner(owner.hwnd);
    check(!OwnerActive(owner.hwnd) && !OwnerUi(owner.hwnd), "stop retires subscriptions dispatch and private worker before unloading DLL");
    metrics.set("observedStopMs", static_cast<uint64_t>(GetTickCount64() - close_started));
    Pump();
    check(IsWindow(owner.hwnd), "host window remains alive after native worker plugin stops");
    return finish();
}
}

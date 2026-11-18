#include "ai_authorization_probe.h"
#include "../common/ai_action_request.h"
#include "../ui/confirm_dialog.h"
#include "ai_tools_actions.h"
#include "app_runtime.h"
#include <filesystem>
#include <fstream>
namespace pulse::app::ai {
namespace {
struct Probe {
    std::shared_ptr<McpActionsRun> run;
    std::wstring capture;
    bool saw_confirmation = false, captured = false;
    HWND observed_dialog = nullptr;
    ULONGLONG first_frame_at = 0;
};
Probe *active = nullptr;
void CALLBACK Observe(HWND, UINT, UINT_PTR, DWORD) {
    if (!active)
        return;
    HWND dialog = nullptr;
    {
        std::lock_guard lock(active->run->mu);
        dialog = active->run->dialog;
    }
    if (!dialog || !IsWindow(dialog) || active->saw_confirmation)
        return;
    if (active->observed_dialog != dialog) {
        active->observed_dialog = dialog;
        RedrawWindow(dialog, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        active->first_frame_at = GetTickCount64();
        return;
    }
    // The confirmation body and footer enter after the first composed frame.
    // Keep pumping the real dialog until its opening motion has settled.
    if (GetTickCount64() - active->first_frame_at < 1000)
        return;
    UpdateWindow(dialog);
    active->saw_confirmation = true;
    active->captured = ui::SaveConfirmDialogFrame(dialog, active->capture);
    PostMessageW(dialog, WM_CLOSE, 0, 0);
}
json::Value Request(std::string_view action) {
    auto step = json::Value::Object();
    step.set("action", std::string(action));
    auto steps = json::Value::Array();
    steps.push(std::move(step));
    auto request = json::Value::Object();
    request.set("steps", std::move(steps));
    return request;
}
bool WaitRun(const std::shared_ptr<McpActionsRun> &run) {
    const auto deadline = GetTickCount64() + 15000;
    do {
        {
            std::lock_guard lock(run->mu);
            if (run->done)
                return true;
        }
        MSG message{};
        for (unsigned count = 0;
             count < 32 && GetTickCount64() < deadline && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++count) {
            if (message.message == WM_QUIT) {
                PostQuitMessage(static_cast<int>(message.wParam));
                return false;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    } while (GetTickCount64() < deadline);
    run->cancelled = true;
    std::lock_guard lock(run->mu);
    if (run->dialog)
        PostMessageW(run->dialog, WM_CLOSE, 0, 0);
    return false;
}
} // namespace
bool RunAiAuthorizationProbe(AppState &state, const std::wstring &directory) {
    if (!state.isolatedTest || !GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", nullptr, 0) ||
        !GetEnvironmentVariableW(L"PULSE_TEST_EXTENSION_CAPTURE", nullptr, 0))
        return false;
    const std::filesystem::path output(directory);
    if (!output.is_absolute())
        return false;
    std::error_code error;
    std::filesystem::create_directories(output, error);
    if (error)
        return false;
    std::ofstream report(output / "report.txt");
    if (!report)
        return false;
    int failures = 0;
    auto check = [&](bool ok, const char *message) {
        report << (ok ? "[PASS] " : "[FAIL] ") << message << '\n';
        report.flush();
        if (!ok)
            ++failures;
    };
    std::atomic<bool> cancelled{false};
    Settings settings;
    for (const char *claim : {"principal", "owner", "ticket", "package"}) {
        auto args = Request("nav.up");
        args.set(claim, "sample.transfer-workflow");
        ToolCall call{L"sample.transfer-workflow", args, cancelled};
        std::shared_ptr<McpActionsRun> dispatched;
        auto result = RunActionsTool(state.hwnd, call, settings, {}, dispatched);
        check(!result.ok && result.error.code == "invalid_args" && !dispatched,
              "real host run_actions rejects forged package authority before UI dispatch");
    }
    auto workflow = Request("workflow.run");
    ToolCall call{L"sample.transfer-workflow", workflow, cancelled};
    std::shared_ptr<McpActionsRun> dispatched;
    auto rejected = RunActionsTool(state.hwnd, call, settings, {}, dispatched);
    check(!rejected.ok && !dispatched, "AI cannot invoke package workflow by reusing its capability name");
    wchar_t executable[32768]{};
    GetModuleFileNameW(nullptr, executable, 32768);
    auto run = std::make_shared<McpActionsRun>();
    run->client = L"sample.transfer-workflow";
    run->allowed.fill(true); // Isolated per-call switches; persisted user settings are never changed.
    auto step = json::Value::Object();
    step.set("action", "nav.open");
    auto args = json::Value::Object();
    args.set("path", json::Utf8(executable));
    step.set("args", std::move(args));
    run->steps.push_back(std::move(step));
    Probe probe{run, (output / "external-action-confirmation.png").wstring()};
    active = &probe;
    const auto timer = SetTimer(state.hwnd, 0x41495033, 50, Observe);
    if (!timer) {
        active = nullptr;
        return false;
    }
    StartMcpRunActions(state, run);
    const bool done = WaitRun(run);
    KillTimer(state.hwnd, timer);
    active = nullptr;
    check(probe.saw_confirmation && probe.captured,
          "actual sensitive AI action opens native confirmation despite package-like client name");
    {
        std::lock_guard lock(run->mu);
        check(done && !run->outcome.ok && run->outcome.error.code == "user_denied" && !run->outcome.changed_files &&
                  run->undo.empty(),
              "denying native confirmation prevents actual sensitive action execution");
    }
    wchar_t profile[32768]{};
    GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", profile, 32768);
    const auto guarded = std::filesystem::path(profile) / L"fixture" / L"guard.txt";
    if (!std::filesystem::is_regular_file(guarded)) {
        check(false, "isolated file-plan fixture exists");
        return false;
    }
    auto file_run = std::make_shared<McpActionsRun>();
    file_run->client = L"sample.transfer-workflow";
    file_run->allowed.fill(true);
    auto file_step = json::Value::Object();
    file_step.set("action", "file.recycle");
    auto file_args = json::Value::Object();
    auto items = json::Value::Array();
    items.push(json::Utf8(guarded.wstring()));
    file_args.set("items", std::move(items));
    file_step.set("args", std::move(file_args));
    file_run->steps.push_back(std::move(file_step));
    Probe file_probe{file_run, (output / "file-plan-confirmation.png").wstring()};
    active = &file_probe;
    const auto file_timer = SetTimer(state.hwnd, 0x41495033, 50, Observe);
    if (!file_timer) {
        active = nullptr;
        return false;
    }
    StartMcpRunActions(state, file_run);
    const bool file_done = WaitRun(file_run);
    KillTimer(state.hwnd, file_timer);
    active = nullptr;
    check(file_probe.saw_confirmation && file_probe.captured,
          "actual AI file operation still shows native plan confirmation");
    {
        std::lock_guard lock(file_run->mu);
        check(file_done && !file_run->outcome.ok && file_run->outcome.error.code == "user_denied" &&
                  !file_run->outcome.changed_files && std::filesystem::is_regular_file(guarded),
              "denying native file plan leaves isolated source untouched");
    }
    report << "Result: " << (failures ? "FAIL" : "PASS") << " (" << failures << " failures)\n";
    return failures == 0;
}
} // namespace pulse::app::ai

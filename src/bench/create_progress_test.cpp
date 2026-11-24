#define PULSE_ELEVATED_TEST_CLIENT 1
#include "../app/app_internal.h"
#include "../ui/ui_renderer.h"
#include <cstdio>
namespace pulse::ops {
struct OpsManagerReviewTestAccess {
    static void Status(OpsManager& manager, const OpStatus& status) {
        manager.SetStatus([&](OpStatus& value) { value = status; });
    }
};
}
int main() {
    using namespace pulse;
    OleInitialize(nullptr);
    int failures = 0;
    auto check = [&](bool ok, const char* name) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); failures += !ok; };
    {
        AppState state;
        state.isolatedTest = true;
        state.appPrefs.persist = false;
        state.places.persist = false;
        state.hwnd = CreateWindowExW(0, L"STATIC", L"Create progress test", WS_OVERLAPPEDWINDOW,
            100, 100, 800, 600, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ops::OpStatus status;
        status.task_id = 1;
        status.type = ops::OpType::CreateFolder;
        status.active = true;
        status.phase = ops::OpPhase::Running;
        status.authorization = ops::AuthorizationState::Requesting;
        auto apply = [&] { ops::OpsManagerReviewTestAccess::Status(state.ops, status); UpdateOperationWindow(state, false); };
        auto visible = [&] { return state.operationWindow && state.operationWindow->IsVisible(); };
        apply();
        check(!visible(), "folder authorization does not open transfer dialog");
        status.authorization = ops::AuthorizationState::None;
        apply();
        check(!visible(), "folder creation stays inline after authorization");
        EnsureOperationWindow(state);
        state.operationWindow->Show(false);
        check(visible(), "fixture opens an actual operation window");
        status.active = false;
        status.phase = ops::OpPhase::Completed;
        apply();
        check(!visible(), "successful creation immediately hides leftover progress window");
        state.operationPinnedByUser = true;
        state.operationWindow->Show(false);
        apply();
        check(visible(), "explicitly pinned task details stay visible");
        state.operationPinnedByUser = false;
        status.phase = ops::OpPhase::Failed;
        status.last_error = l10n::Get(l10n::StringId::Canceled);
        apply();
        check(!visible(), "cancelled creation leaves no progress window");
        auto pump_once = [] {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        };
        auto pump_ms = [&](DWORD ms) {
            const ULONGLONG stop = GetTickCount64() + ms;
            while (GetTickCount64() < stop) {
                pump_once();
                Sleep(10);
            }
            pump_once();
        };
        {
            ops::OpStatus ext;
            ext.external = true;
            ext.type = ops::OpType::External;
            ext.task_id = 10;
            ext.external_title = L"Extract sample.zip";
            ext.summary = ext.external_title;
            ext.active = true;
            ext.phase = ops::OpPhase::Running;
            ext.percent = 50.0f;
            ext.started_tick = GetTickCount64();
            status = ext;
            apply();
            check(!visible(), "new external task stays hidden before auto-show delay");
            state.operationAutoShowFrom = std::chrono::steady_clock::now() - std::chrono::milliseconds(2500);
            apply();
            check(visible(), "external task auto-shows after 2s delay");
            pump_ms(320);

            status.active = false;
            status.phase = ops::OpPhase::Completed;
            status.percent = 100.0f;
            status.external_outcome = ops::ExternalOutcome::Completed;
            status.ended_tick = GetTickCount64();
            apply();
            check(visible(), "completed auto-shown card stays visible initially for dismiss hold");
            const ULONGLONG complete_tick = GetTickCount64();
            bool auto_closed = false;
            while (GetTickCount64() - complete_tick < 2000) {
                pump_once();
                UpdateOperationWindow(state, false);
                if (!visible()) {
                    auto_closed = true;
                    break;
                }
                Sleep(15);
            }
            check(auto_closed, "completed card auto-closes under 15ms UpdateOperationWindow polling");
            pump_ms(400);
        }
        {
            ops::OpStatus linger;
            linger.external = true;
            linger.type = ops::OpType::External;
            linger.task_id = 20;
            linger.external_title = L"Extract prev.zip";
            linger.summary = linger.external_title;
            linger.active = true;
            linger.phase = ops::OpPhase::Running;
            linger.percent = 45.0f;
            linger.started_tick = GetTickCount64();
            status = linger;
            apply();
            state.operationAutoShowFrom = std::chrono::steady_clock::now() - std::chrono::milliseconds(2500);
            apply();
            pump_ms(320);
            status.active = false;
            status.phase = ops::OpPhase::Completed;
            status.percent = 100.0f;
            status.external_outcome = ops::ExternalOutcome::Completed;
            status.ended_tick = GetTickCount64();
            apply();
            check(visible(), "lingering task is visible before quick task arrives");

            ops::OpStatus fast;
            fast.external = true;
            fast.type = ops::OpType::External;
            fast.task_id = 21;
            fast.external_title = L"Extract fast.zip";
            fast.summary = fast.external_title;
            fast.active = true;
            fast.phase = ops::OpPhase::Running;
            fast.percent = 10.0f;
            fast.started_tick = GetTickCount64();
            status = fast;
            apply();
            check(!visible(), "starting a new task immediately hides lingering window from previous task");
            Sleep(50);
            status.active = false;
            status.phase = ops::OpPhase::Completed;
            status.percent = 100.0f;
            status.external_outcome = ops::ExternalOutcome::Completed;
            status.ended_tick = GetTickCount64();
            apply();
            check(!visible(), "fast 50ms task never opens operation window on completion");

            // Simulate prior pinned task followed by a 0s task completed before active polling.
            state.operationPinnedByUser = true;
            state.operationWindow->Show(false);
            pump_ms(320);
            ops::OpStatus instant;
            instant.external = true;
            instant.type = ops::OpType::External;
            instant.task_id = 22;
            instant.external_title = L"Extract instant.zip";
            instant.summary = instant.external_title;
            instant.active = false;
            instant.phase = ops::OpPhase::Completed;
            instant.percent = 100.0f;
            instant.external_outcome = ops::ExternalOutcome::Completed;
            instant.started_tick = GetTickCount64();
            instant.ended_tick = instant.started_tick;
            status = instant;
            apply();
            check(state.operationUiTaskId == 22 && !state.operationPinnedByUser && !visible(),
                "0s completed task clears prior pinned window state and never displays");
            pump_ms(400);
        }
        {
            ops::OpStatus deferred;
            deferred.external = true;
            deferred.type = ops::OpType::External;
            deferred.task_id = 23;
            deferred.external_title = L"Extract deferred.zip";
            deferred.summary = deferred.external_title;
            deferred.active = true;
            deferred.phase = ops::OpPhase::Running;
            deferred.percent = 40.0f;
            deferred.started_tick = GetTickCount64();
            status = deferred;
            apply();
            check(!visible(), "deferred task starts hidden");
            state.operationAutoShowFrom = std::chrono::steady_clock::now() - std::chrono::milliseconds(2500);
            DeferOperationAutoShow(state);
            apply();
            check(!visible() && !state.operationAutoShown,
                "DeferOperationAutoShow restarts auto-show timer across extension modal prompts");
        }
        state.operationWindow.reset();
        DestroyWindow(state.hwnd);
        state.hwnd = nullptr;
    }
    OleUninitialize();
    return failures ? 1 : 0;
}

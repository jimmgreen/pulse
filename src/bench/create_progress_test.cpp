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
        state.operationWindow.reset();
        DestroyWindow(state.hwnd);
        state.hwnd = nullptr;
    }
    OleUninitialize();
    return failures ? 1 : 0;
}

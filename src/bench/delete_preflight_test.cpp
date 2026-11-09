#include "../app/delete_preflight.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace pulse;
static bool Wait(const std::shared_ptr<DeletePreflight>& request) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!request->done.load() && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return request->done.load();
}
int main() {
    const HWND receiver = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!receiver) return 1;
    auto notified = std::make_shared<DeletePreflight>();
    notified->notify_window = receiver;
    StartDeletePreflight(notified);
    MSG completion{};
    bool received = false;
    const auto notification_deadline = GetTickCount64() + 5000;
    while (!received && GetTickCount64() < notification_deadline) {
        received = PeekMessageW(&completion, receiver, WM_TIMER, WM_TIMER, PM_REMOVE) != FALSE;
        if (!received) Sleep(1);
    }
    DestroyWindow(receiver);
    if (!received || completion.wParam != 0xD311 || !notified->done.load()) {
        std::puts("[FAIL] completion must wake the UI without a polling timer"); return 1;
    }
    std::puts("[PASS] completion wakes the UI without a polling timer");
    DeletePreflight display;
    display.started_at = 1000;
    if (ShouldShowDeletePreparation(display, 1199) || !ShouldShowDeletePreparation(display, 1200)) return 1;
    display.done.store(true);
    if (ShouldShowDeletePreparation(display, 1500)) return 1;
    display.done.store(false); display.cancel.store(true);
    if (ShouldShowDeletePreparation(display, 1500)) return 1;
    display.cancel.store(false); display.preparation_shown = true;
    if (ShouldShowDeletePreparation(display, 1500)) return 1;
    std::puts("[PASS] preparation window only for pending uncanceled work after grace period");
    auto request = std::make_shared<DeletePreflight>();
    std::atomic<bool> entered{false}, release{false};
    request->paths = {L"Z:\\preflight-read-only-test"};
    request->before_probe = [&] {
        entered.store(true);
        while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    };
    StartDeletePreflight(request);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!entered.load() && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool started = entered.load();
    request->cancel.store(true);
    release.store(true);
    if (!Wait(request) || !started || request->failed || !request->direct.empty()) {
        std::puts("[FAIL] cancellation before read-only probe"); return 1;
    }
    std::puts("[PASS] cancellation before read-only probe");
    auto old = std::make_shared<DeletePreflight>();
    old->cancel.store(true);
    StartDeletePreflight(old);
    auto current = std::make_shared<DeletePreflight>();
    StartDeletePreflight(current);
    if (!Wait(old) || !Wait(current) || current->cancel.load() || current->failed) {
        std::puts("[FAIL] independent request generations"); return 1;
    }
    std::puts("[PASS] independent request generations");
    unsigned submitted = 0;
    auto submit = [&](const std::shared_ptr<DeletePreflight>& owner,
                      const std::shared_ptr<DeletePreflight>& result) {
        if (CanSubmitDeletePreflight(owner, result)) ++submitted;
    };
    submit(request, request); // canceled
    submit(current, old); // replaced
    submit({}, current); // destroyed owner
    if (submitted != 0) { std::puts("[FAIL] invalid lifecycle submitted"); return 1; }
    submit(current, current);
    if (submitted != 1) { std::puts("[FAIL] current completion rejected"); return 1; }
    auto failed = std::make_shared<DeletePreflight>();
    failed->before_probe = [] { throw 1; };
    StartDeletePreflight(failed);
    if (!Wait(failed) || !failed->failed || CanSubmitDeletePreflight(failed, failed)) {
        std::puts("[FAIL] failed preparation submitted"); return 1;
    }
    std::puts("[PASS] canceled/replaced/destroyed/failed requests cannot submit");
    return 0;
}

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

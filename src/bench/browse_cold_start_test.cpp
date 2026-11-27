#include "../explorer_bridge/cold_start.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

namespace {
using namespace pulse;
namespace cold = explorer_bridge::cold_start;
struct Fake final : cold::Hooks {
    uint64_t tick = 0, ready_at = 50;
    unsigned locks = 0, releases = 0, starts = 0, submits = 0;
    DWORD lock_error = 0, start_error = 0;
    bool initially_ready = false, existing_startup = false, another_started = false, never_ready = false;
    cold::Outcome reply{DirectRevealResult::Accepted, ERROR_SUCCESS};
    bool wait_for_ready = false;
    uint64_t submitted_deadline = 0;
    browse::Request captured;
    uint64_t Now() noexcept override { return tick; }
    void Pause(DWORD ms) noexcept override { tick += ms; }
    cold::EndpointState Find() noexcept override {
        if (initially_ready || (another_started && locks) ||
            (!never_ready && (starts || existing_startup) && tick >= ready_at)) return cold::EndpointState::Ready;
        return existing_startup ? cold::EndpointState::Starting : cold::EndpointState::Absent;
    }
    DWORD Acquire(DWORD) noexcept override { ++locks; return lock_error; }
    void Release() noexcept override { ++releases; }
    DWORD Start() noexcept override { ++starts; return start_error; }
    cold::Outcome Submit(const browse::Request& request, bool wait, uint64_t deadline) noexcept override {
        ++submits; captured = request; wait_for_ready = wait; submitted_deadline = deadline; return reply;
    }
};
struct Shared {
    std::timed_mutex mutex;
    std::atomic<unsigned> starts{0}, submits{0};
    std::atomic<uint64_t> ready_at{0};
};
struct Parallel final : cold::Hooks {
    Shared& shared;
    explicit Parallel(Shared& value) : shared(value) {}
    uint64_t Now() noexcept override { return GetTickCount64(); }
    void Pause(DWORD ms) noexcept override { Sleep(ms); }
    cold::EndpointState Find() noexcept override {
        const auto ready = shared.ready_at.load();
        if (ready && Now() >= ready) return cold::EndpointState::Ready;
        return ready ? cold::EndpointState::Starting : cold::EndpointState::Absent;
    }
    DWORD Acquire(DWORD timeout) noexcept override {
        return shared.mutex.try_lock_for(std::chrono::milliseconds(timeout)) ? ERROR_SUCCESS : ERROR_TIMEOUT;
    }
    void Release() noexcept override { shared.mutex.unlock(); }
    DWORD Start() noexcept override {
        ++shared.starts; shared.ready_at = Now() + 30; return ERROR_SUCCESS;
    }
    cold::Outcome Submit(const browse::Request&, bool, uint64_t) noexcept override {
        ++shared.submits; return {DirectRevealResult::Accepted, ERROR_SUCCESS};
    }
};
}
int main() {
    unsigned checks = 0, failures = 0;
    const auto check = [&](bool ok, const char* label) {
        ++checks; if (!ok) ++failures;
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    };
    const pulse::browse::Request request{pulse::browse::Intent::Reveal, L"C:\\fixture\\item.txt"};
    {
        Fake h; h.initially_ready = true;
        const auto result = cold::Deliver(request, h);
        check(result.result == DirectRevealResult::Accepted && h.submits == 1 && !h.starts && !h.locks &&
              h.captured.intent == request.intent && h.captured.path == request.path &&
              !h.wait_for_ready && h.submitted_deadline == 1000,
              "verified warm endpoint submits unchanged intent once without startup coordination");
    }
    for (const auto reply : {DirectRevealResult::Rejected, DirectRevealResult::Indeterminate}) {
        Fake h; h.initially_ready = true;
        const DWORD error = reply == DirectRevealResult::Indeterminate ? ERROR_TIMEOUT : ERROR_NOT_READY;
        h.reply = {reply, error};
        const auto result = cold::Deliver(request, h);
        check(result.result == reply && result.error == h.reply.error && h.submits == 1 && !h.starts && !h.locks && !h.wait_for_ready,
              "warm rejection or uncertain ACK never starts a host or resubmits");
    }
    {
        Fake h;
        const auto result = cold::Deliver(request, h, 200);
        check(result.result == DirectRevealResult::Accepted && h.starts == 1 && h.submits == 1 &&
              h.locks == 1 && h.releases == 1 && h.tick == 50 && h.wait_for_ready && h.submitted_deadline == 200,
              "cold request waits for readiness then submits exactly once and releases startup mutex");
    }
    {
        Fake h; h.existing_startup = true;
        const auto result = cold::Deliver(request, h, 200);
        check(result.result == DirectRevealResult::Accepted && !h.starts && h.submits == 1 && h.releases == 1,
              "an already-starting Pulse is awaited without launching a peer");
    }
    {
        Fake h; h.another_started = true;
        const auto result = cold::Deliver(request, h, 200);
        check(result.result == DirectRevealResult::Accepted && !h.starts && h.submits == 1 && h.releases == 1,
              "mutex waiter rechecks endpoint and reuses the winning helper's startup");
    }
    {
        Fake h; h.never_ready = true;
        const auto result = cold::Deliver(request, h, 73);
        check(result.error == ERROR_TIMEOUT && h.starts == 1 && !h.submits && h.tick == 73 && h.releases == 1,
              "readiness timeout is bounded and neither resubmits nor kills the started host");
    }
    {
        Fake h; h.start_error = ERROR_ACCESS_DENIED;
        const auto result = cold::Deliver(request, h, 200);
        check(result.error == ERROR_ACCESS_DENIED && h.starts == 1 && !h.submits && h.releases == 1,
              "trusted launch rejection propagates without sending or retrying");
    }
    {
        Fake h; h.lock_error = ERROR_TIMEOUT;
        const auto result = cold::Deliver(request, h, 200);
        check(result.error == ERROR_TIMEOUT && !h.starts && !h.submits && !h.releases,
              "coordination timeout never releases an unowned mutex or launches a host");
    }
    {
        Fake h; const auto result = cold::Deliver(request, h, 0);
        check(result.error == ERROR_TIMEOUT && !h.starts && !h.submits && !h.locks,
              "zero cold-start budget has no side effects");
    }
    {
        Fake h; h.ready_at = 73;
        const auto result = cold::Deliver(request, h, 73);
        check(result.error == ERROR_TIMEOUT && h.tick == 73 && !h.submits && h.releases == 1,
              "endpoint publication at deadline does not obtain a fresh submission budget");
    }
    {
        Shared shared; Parallel first(shared), second(shared);
        cold::Outcome a, b;
        std::thread one([&] { a = cold::Deliver(request, first, 1000); });
        std::thread two([&] { b = cold::Deliver(request, second, 1000); });
        one.join(); two.join();
        check(a.result == DirectRevealResult::Accepted && b.result == DirectRevealResult::Accepted &&
              shared.starts == 1 && shared.submits == 2,
              "two concurrent requests coordinate one startup and independently submit once each");
    }
    std::printf("RESULT checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}

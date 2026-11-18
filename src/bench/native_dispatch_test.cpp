#include "../app/extensions/native/dispatch_adapter.h"
#include <atomic>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>
#include <windows.h>

namespace {
void Pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}
struct Counter {
    int calls = 0;
    DWORD thread = 0;
};
void PULSE_CALL Count(void *user) {
    auto &counter = *static_cast<Counter *>(user);
    ++counter.calls;
    counter.thread = GetCurrentThreadId();
}
PulseDispatchV1 Table(pulse::extensions::native::DispatchAdapter &adapter) {
    PulseDispatchV1 result{};
    if (adapter.Query(PULSE_DISPATCH_VERSION, &result, sizeof(result)) != PULSE_OK)
        return {};
    return result;
}
} // namespace
int main() {
    using pulse::extensions::native::DispatchAdapter;
    int passed = 0, failed = 0;
    auto check = [&](bool ok, const char *label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        ok ? ++passed : ++failed;
    };
    const DWORD ui_thread = GetCurrentThreadId();
    {
        int alive_checks = 0;
        bool alive_thread = true;
        DispatchAdapter adapter([&] {
            ++alive_checks;
            alive_thread &= GetCurrentThreadId() == ui_thread;
            return true;
        });
        auto api = Table(adapter);
        check(api.post != nullptr, "query dispatch v1");
        if (!api.post)
            return 1;
        Counter counter;
        uint64_t token = 0;
        PulseStatus status = PULSE_ERROR_STATE;
        std::thread producer([&] { status = api.post(api.context, Count, &counter, &token); });
        producer.join();
        check(status == PULSE_OK && token && counter.calls == 0 && alive_checks == 0,
              "worker post only queues work without owner access");
        Pump();
        check(counter.calls == 1 && counter.thread == ui_thread && alive_thread && alive_checks == 1,
              "real message window dispatches once on owner UI thread");
        check(api.cancel(api.context, token) == PULSE_ERROR_STATE, "completed token is not reported safely cancelled");
        auto cancelled = std::make_unique<Counter>();
        uint64_t cancelled_token = 0;
        api.post(api.context, Count, cancelled.get(), &cancelled_token);
        std::thread canceller([&] { status = api.cancel(api.context, cancelled_token); });
        canceller.join();
        check(status == PULSE_OK, "background cancellation wins before callback begins");
        cancelled.reset();
        Pump();
        check(api.cancel(api.context, cancelled_token) == PULSE_ERROR_STATE,
              "cancelled userdata can be freed before dispatch; token not reused");
        std::vector<uint64_t> tokens;
        for (uint32_t i = 0; i < PULSE_DISPATCH_MAX_PENDING; ++i) {
            uint64_t current = 0;
            if (api.post(api.context, Count, &counter, &current) == PULSE_OK)
                tokens.push_back(current);
        }
        uint64_t overflow = 99;
        check(tokens.size() == PULSE_DISPATCH_MAX_PENDING &&
                  api.post(api.context, Count, &counter, &overflow) == PULSE_ERROR_STATE && !overflow,
              "exact pending limit accepted; overflow rejected without callback");
        bool cancelled_all = true;
        for (uint64_t current : tokens)
            cancelled_all &= api.cancel(api.context, current) == PULSE_OK;
        uint64_t reused_slot = 0;
        check(cancelled_all && api.post(api.context, Count, &counter, &reused_slot) == PULSE_OK &&
                  reused_slot > tokens.back(),
              "cancel frees bounded slots while identities remain monotonic");
        Pump();
        check(counter.calls == 2, "cancelled queue produces no extra callbacks");
    }
    {
        DispatchAdapter adapter([] { return true; });
        auto api = Table(adapter);
        struct Running {
            DispatchAdapter *adapter;
            HANDLE started;
            HANDLE finished;
            bool active = false;
            bool worker_finished = false;
            static void PULSE_CALL Run(void *user) {
                auto &self = *static_cast<Running *>(user);
                self.active = self.adapter->CallbackActive();
                SetEvent(self.started);
                self.worker_finished = WaitForSingleObject(self.finished, 2000) == WAIT_OBJECT_0;
            }
        } data{&adapter, CreateEventW(nullptr, TRUE, FALSE, nullptr), CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        uint64_t token = 0;
        api.post(api.context, Running::Run, &data, &token);
        PulseStatus cancelled = PULSE_OK;
        std::thread worker([&] {
            if (WaitForSingleObject(data.started, 2000) == WAIT_OBJECT_0)
                cancelled = api.cancel(api.context, token);
            SetEvent(data.finished);
        });
        Pump();
        worker.join();
        check(data.active && data.worker_finished && cancelled == PULSE_ERROR_STATE && !adapter.CallbackActive(),
              "cancel racing an active callback returns STATE, never grants userdata release");
        CloseHandle(data.started);
        CloseHandle(data.finished);
    }
    {
        DispatchAdapter adapter([] { return true; });
        auto api = Table(adapter);
        Counter after;
        struct Nested {
            DispatchAdapter *adapter;
            PulseDispatchV1 *api;
            Counter *after;
            bool deferred = false;
            bool active = false;
            static void PULSE_CALL Run(void *user) {
                auto &self = *static_cast<Nested *>(user);
                uint64_t token = 0;
                self.api->post(self.api->context, Count, self.after, &token);
                Pump();
                self.deferred = self.after->calls == 0;
                self.adapter->Close();
                self.active = self.adapter->CallbackActive();
            }
        } data{&adapter, &api, &after};
        uint64_t token = 0;
        api.post(api.context, Nested::Run, &data, &token);
        Pump();
        check(data.deferred && data.active && !adapter.CallbackActive() && !after.calls,
              "callbacks are non-nested and reentrant Close suppresses pending result");
        check(api.post(api.context, Count, &after, &token) == PULSE_ERROR_STATE,
              "retained context rejects post after Close");
    }
    {
        bool alive = true;
        DispatchAdapter adapter([&] { return alive; });
        auto api = Table(adapter);
        Counter counter;
        uint64_t token = 0;
        api.post(api.context, Count, &counter, &token);
        alive = false;
        Pump();
        check(counter.calls == 0 && api.post(api.context, Count, &counter, &token) == PULSE_ERROR_STATE,
              "UI owner revocation discards queued work and stops later producers");
    }
    {
        DispatchAdapter adapter([] { return true; });
        auto api = Table(adapter);
        Counter counter;
        std::atomic<bool> first{false}, closed{false};
        std::atomic<int> accepted_after_close{0};
        std::thread producer([&] {
            uint64_t token = 0;
            api.post(api.context, Count, &counter, &token);
            first.store(true);
            for (int i = 0; i < 10000; ++i) {
                const bool observed_close = closed.load();
                if (api.post(api.context, Count, &counter, &token) == PULSE_OK && observed_close)
                    ++accepted_after_close;
            }
            while (!closed.load())
                std::this_thread::yield();
            if (api.post(api.context, Count, &counter, &token) == PULSE_OK)
                ++accepted_after_close;
        });
        while (!first.load())
            std::this_thread::yield();
        adapter.Close();
        closed.store(true);
        producer.join();
        Pump();
        check(!accepted_after_close.load() && counter.calls == 0,
              "concurrent Close and producer retain safe context and suppress queued callbacks");
    }
    std::printf("%d PASS, %d FAIL\n", passed, failed);
    return failed ? 1 : 0;
}

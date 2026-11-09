#include "../app/shell_cache_readers.h"
#include <cstdio>
#include <future>
#include <semaphore>

int main() {
    int failures = 0;
    auto check = [&](bool value, const char *name) {
        std::printf("[%s] %s\n", value ? "PASS" : "FAIL", name);
        if (!value)
            ++failures;
    };
    pulse::app::ShellCacheReaders readers;
    std::binary_semaphore entered(0), cancel_seen(0), release(0);
    std::atomic<bool> returned = false, finished = false;
    readers.Start([&](std::stop_token stop) {
        std::stop_callback cancelled(stop, [&] { cancel_seen.release(); });
        entered.release();
        release.acquire(); // Model an in-flight registry/icon call that cannot be interrupted.
        finished = true;
    });
    entered.acquire();
    std::thread shutdown([&] {
        readers.Stop();
        returned = true;
    });
    cancel_seen.acquire();
    check(!returned && !finished, "shutdown requests cancellation but waits for in-flight work");
    release.release();
    shutdown.join();
    check(returned && finished, "shutdown returns only after dependency-using work has finished");
    bool admitted = false;
    readers.Start([&](std::stop_token) { admitted = true; });
    readers.Stop();
    check(!admitted, "closed reader group rejects new work and repeated stop is safe");

    std::promise<void> destruction_cancelled;
    auto cancellation = destruction_cancelled.get_future();
    std::atomic<bool> destruction_finished = false;
    {
        pulse::app::ShellCacheReaders scope;
        std::binary_semaphore ready(0);
        scope.Start([&](std::stop_token stop) {
            std::stop_callback cancelled(stop, [&] { destruction_cancelled.set_value(); });
            ready.release();
            cancellation.wait();
            destruction_finished = true;
        });
        ready.acquire();
    }
    check(destruction_finished, "owner destruction cancels and joins without explicit stop");
    return failures ? 1 : 0;
}

#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace pulse::app {
// A stuck Shell/provider call must not hold a navigation worker or spawn an
// unbounded number of threads. This quota is independent of directory listing.
inline std::atomic<unsigned> active_link_batches{0};

template<class Entry, class Resolve, class Cancelled>
void ResolveLinkBatch(std::vector<Entry>& entries, Resolve resolve, Cancelled cancelled,
    std::chrono::milliseconds budget = std::chrono::milliseconds(100)) {
    if (entries.empty() || cancelled()) return;
    unsigned active = active_link_batches.load();
    do {
        if (active >= 2) return;
    } while (!active_link_batches.compare_exchange_weak(active, active + 1));

    struct State {
        std::atomic_bool stop{false};
        std::mutex mutex;
        std::condition_variable ready;
        std::vector<Entry> results;
        size_t completed = 0;
        bool done = false;
    };
    std::shared_ptr<State> state;
    std::thread worker;
    const auto deadline = std::chrono::steady_clock::now() + budget;
    try {
        state = std::make_shared<State>();
        state->results = entries;
        worker = std::thread([state, resolve = std::move(resolve)]() mutable {
            try {
                for (size_t i = 0; i < state->results.size() && !state->stop; ++i) {
                    auto entry = state->results[i];
                    resolve(entry);
                    std::lock_guard lock(state->mutex);
                    if (state->stop) break;
                    state->results[i] = std::move(entry);
                    state->completed = i + 1;
                    state->ready.notify_one();
                }
            } catch (...) {
                // Link metadata is optional; the directory remains usable.
            }
            {
                std::lock_guard lock(state->mutex);
                state->done = true;
            }
            --active_link_batches;
            state->ready.notify_one();
        });
    } catch (...) {
        --active_link_batches;
        return;
    }
    bool done = false;
    try {
        std::unique_lock lock(state->mutex);
        while (!state->done && !cancelled() && std::chrono::steady_clock::now() < deadline)
            state->ready.wait_until(lock, (std::min)(deadline,
                std::chrono::steady_clock::now() + std::chrono::milliseconds(5)));
        state->stop = true;
        if (!cancelled()) {
            for (size_t i = 0; i < state->completed; ++i)
                entries[i] = std::move(state->results[i]);
        }
        done = state->done;
    } catch (...) {
        state->stop = true;
    }
    // The provider owns all inputs. Late completion never writes to the caller's
    // snapshot; cancellation cannot interrupt arbitrary in-process Shell code.
    if (done) worker.join();
    else worker.detach();
}
} // namespace pulse::app

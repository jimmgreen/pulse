#pragma once
#include <thread>
#include <utility>
#ifdef PULSE_RESOURCE_REUSE_TEST
#include <atomic>
#include <system_error>
#include <chrono>
#include <cstdint>
#endif

namespace pulse {
#ifdef PULSE_RESOURCE_REUSE_TEST
inline std::atomic<int> worker_start_failure{-1};
inline std::atomic<uint64_t> worker_starts{0}, worker_start_ns{0};
#endif
template<class Work>
std::thread StartWorkerThread(Work&& work) {
#ifdef PULSE_RESOURCE_REUSE_TEST
    if (worker_start_failure.load() >= 0 && worker_start_failure.fetch_sub(1) == 0)
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
#endif
#ifdef PULSE_RESOURCE_REUSE_TEST
    const auto start = std::chrono::steady_clock::now();
    std::thread thread(std::forward<Work>(work));
    ++worker_starts;
    worker_start_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count();
    return thread;
#else
    return std::thread(std::forward<Work>(work));
#endif
}
}

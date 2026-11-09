#pragma once
#include <windows.h>
#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>

namespace pulse::ops::test {
// One-shot observer for isolated real-I/O acceptance; never part of the extension ABI.
inline std::mutex controlled_probe_mutex;
inline std::function<void(uint64_t)> controlled_probe;
inline void SetControlledTransferProbe(std::function<void(uint64_t)> observer) {
    if (observer && (!GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", nullptr, 0) ||
        !GetEnvironmentVariableW(L"PULSE_TEST_EXTENSION_CAPTURE", nullptr, 0))) return;
    std::lock_guard lock(controlled_probe_mutex);
    controlled_probe = std::move(observer);
}
inline std::function<void(uint64_t)> TakeControlledTransferProbe() {
    std::lock_guard lock(controlled_probe_mutex);
    return std::exchange(controlled_probe, {});
}
}

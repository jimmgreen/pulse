#pragma once
#include <atomic>
#include <cstdint>
#include <limits>
namespace pulse::extensions {
inline uint64_t NextFileOperationRequestId() noexcept {
    static std::atomic<uint64_t> next{0};
    auto value = next.load();
    while (value != (std::numeric_limits<uint64_t>::max)()) {
        if (next.compare_exchange_weak(value, value + 1)) return value + 1;
    }
    return 0;
}
}

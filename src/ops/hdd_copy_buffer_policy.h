#pragma once
#include <cstdint>

namespace pulse::ops {
// Uses enumerated size estimates to select cache-backed completion, not a
// hard memory limit or durable-write guarantee. A source that grows during
// CopyFile2 does not switch the active transfer to another buffering mode.
constexpr bool UseBoundedHddCopyBuffer(bool source_ssd, bool copy, bool verify, bool enumerating,
                                     bool spinning, bool slow, bool memory_known,
                                     uint64_t task_bytes, uint64_t file_bytes, uint64_t available) {
    return source_ssd && copy && !verify && !enumerating && spinning && !slow && memory_known &&
        task_bytes > 0 && task_bytes <= (2ull << 30) &&
        file_bytes >= (64ull << 20) && file_bytes <= (512ull << 20) &&
        task_bytes <= available / 4;
}
}

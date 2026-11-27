#pragma once
#include <cstdint>

namespace pulse::ops {

enum class EtaStage : uint8_t { Legacy, Learning, Range, Unknown, Finishing };
enum class EtaSource : uint8_t { None, RecentCopy, DirectorySamples, KnownWork };

// Advisory presentation only: never use these fields as an operation's totals,
// completion condition, undo authority, or permission to touch a path.
struct OperationEta {
    uint64_t task_id = 0;
    uint64_t sample_tick = 0;
    uint64_t processed_items = 0;
    uint64_t lower_seconds = 0;
    uint64_t upper_seconds = 0;
    EtaStage stage = EtaStage::Legacy;
    EtaSource source = EtaSource::None;
    bool has_processed_items = false;
};

} // namespace pulse::ops

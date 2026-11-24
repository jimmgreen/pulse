#pragma once

#include "../../../sdk/native/include/pulse/fileevents.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pulse::extensions::fileevents {

// Process-wide fan-out of finished file operations to plugin subscribers (pulse.fileevents).
// The app publishes from WM_OPS_NOTIFY after its own bookkeeping. UI thread only.
class Hub {
public:
    using Listener = std::function<void(const PulseFileEvent&)>;
    static constexpr size_t kMaxItems = 1024;

    static Hub& Instance();

    uint64_t Add(uint64_t owner, uint32_t kinds, Listener listener);  // kinds: PULSE_FILE_EVENT_MASK bits, 0 = all
    bool Remove(uint64_t id);
    void RemoveOwner(uint64_t owner);
    size_t CountForOwner(uint64_t owner) const;
    bool Wants(uint32_t kind) const;  // a subscriber takes this kind (skip building the event otherwise)

    // sources / destinations as the operation reported them (index-aligned when both exist).
    void Publish(uint32_t kind, const std::vector<std::wstring>& sources, const std::vector<std::wstring>& destinations);
    uint64_t Sequence() const noexcept { return sequence_; }

private:
    struct Entry {
        uint64_t id = 0, owner = 0;
        uint32_t kinds = 0;
        Listener listener;
    };
    std::vector<Entry> entries_;
    uint64_t next_id_ = 1;
    uint64_t sequence_ = 0;
};

} // namespace pulse::extensions::fileevents

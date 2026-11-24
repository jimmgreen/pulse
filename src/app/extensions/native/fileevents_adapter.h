#pragma once

#include "../../../../sdk/native/include/pulse/fileevents.h"
#include "../file_event_hub.h"
#include <windows.h>
#include <functional>
#include <memory>
#include <string>

namespace pulse::extensions::native {

class DiagnosticsSink;

// pulse.fileevents for one plugin session: subscriptions live in the shared
// fileevents::Hub and are removed on Close.
class FileEventsAdapter {
public:
    FileEventsAdapter(std::string package_id,
                      std::function<bool()> alive,
                      std::shared_ptr<DiagnosticsSink> diagnostics = nullptr,
                      fileevents::Hub* hub = &fileevents::Hub::Instance());
    ~FileEventsAdapter();
    FileEventsAdapter(const FileEventsAdapter&) = delete;
    FileEventsAdapter& operator=(const FileEventsAdapter&) = delete;

    PulseStatus Query(uint32_t version, void* out, uint32_t size) const noexcept;
    void Close() noexcept;
    bool CallbackActive() const noexcept;

    static constexpr size_t kMaxSubscriptions = 8;

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace pulse::extensions::native

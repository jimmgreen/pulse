#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <windows.h>

namespace pulse::extension_host {
struct LoopbackEvidenceResult {
    DWORD error = ERROR_NOT_READY;
    bool subscribed = false;
    bool capability_drop = false;
    uint64_t filter_id = 0;
    uint32_t capability = 0;
};
// Read-only WFP observer. Never enables logging or changes filtering policy.
class LoopbackEvidence {
  public:
    LoopbackEvidence();
    ~LoopbackEvidence();
    LoopbackEvidence(const LoopbackEvidence &) = delete;
    LoopbackEvidence &operator=(const LoopbackEvidence &) = delete;
    bool Start(const std::wstring &executable, PSID container, uint16_t port);
    LoopbackEvidenceResult Finish(DWORD grace_ms = 250);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace pulse::extension_host

#pragma once
#include "../../common/json_value.h"
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace pulse::extensions {
inline constexpr size_t kAuditQueueRecords = 256;
inline constexpr size_t kAuditSegmentBytes = 65536;
inline constexpr size_t kAuditSegments = 4;
// Four committed segments, one bounded atomic temporary, status and a zero-byte lock.
inline constexpr size_t kAuditMaximumDiskBytes = 5 * kAuditSegmentBytes + 4096;
struct AuditPersistenceStatus {
    uint64_t accepted = 0, persisted = 0, dropped = 0, failures = 0;
    size_t queued = 0;
    std::string last_error;
};
class AuditPersistence {
  public:
    explicit AuditPersistence(std::wstring root);
    ~AuditPersistence();
    AuditPersistence(const AuditPersistence&) = delete;
    AuditPersistence& operator=(const AuditPersistence&) = delete;
    // Memory-only, nonblocking admission. True is not a durability acknowledgement.
    bool Enqueue(std::string_view package, std::string_view operation, std::string_view outcome);
    AuditPersistenceStatus Status() const;
    // Drains the bounded queue. File locks never wait/retry; no filesystem work on this thread.
    void Stop();
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Host-local diagnostics only; never exposed by ResourceService/audit.list.
// Synchronous: call on a worker or before starting a GUI message loop.
json::Value ReadPersistentAudit(const std::wstring& root);
json::Value ExportPersistentAudit(const std::wstring& root, const std::wstring& destination);
void StartPersistentExtensionAudit(std::wstring root);
void StopPersistentExtensionAudit();
void SetPersistentExtensionAuditStartupFailure(bool missing_root);
void PersistExtensionAudit(std::string_view package, std::string_view operation, std::string_view outcome);
AuditPersistenceStatus PersistentExtensionAuditStatus();
}

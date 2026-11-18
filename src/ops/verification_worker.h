#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
namespace pulse::ops {
struct VerificationStamp {
    DWORD volume = 0, index_high = 0, index_low = 0;
    uint64_t bytes = 0;
    FILETIME last_write{};
};
bool CaptureVerificationStamp(const std::wstring& path, VerificationStamp& stamp, DWORD& error);
enum class VerificationError { None, Cancelled, Provider, SourceOpen, SourceRead, SourceStat,
    SourceHash, TargetOpen, TargetRead, TargetStat, TargetHash, SourceChanged, TargetChanged, Mismatch };
struct VerificationResult {
    VerificationError error = VerificationError::None;
    DWORD win32_error = ERROR_SUCCESS;
    NTSTATUS crypto_status = 0;
    uint64_t source_bytes = 0, target_bytes = 0, source_wall_ns = 0, target_wall_ns = 0;
    bool Success() const noexcept { return error == VerificationError::None; }
};
// One instance per worker, never used concurrently. No file data retained across jobs.
// The coordinator owns bounded queued+active+completed slots and device permits.
class SHA256VerificationWorker {
public:
    SHA256VerificationWorker();
    ~SHA256VerificationWorker();
    SHA256VerificationWorker(const SHA256VerificationWorker&) = delete;
    SHA256VerificationWorker& operator=(const SHA256VerificationWorker&) = delete;
    VerificationResult Verify(const std::wstring& source, const std::wstring& target,
        const std::atomic<bool>& cancel, const std::function<void()>& wait_if_paused,
        const VerificationStamp* before_copy = nullptr,
        const VerificationStamp* expected_target = nullptr);
private:
    bool Read(const std::wstring& path, bool source, const std::atomic<bool>& cancel,
        const std::function<void()>& wait_if_paused, std::array<uint8_t, 32>& digest,
        VerificationStamp& stamp, VerificationResult& result);
    BCRYPT_ALG_HANDLE provider_ = nullptr;
    NTSTATUS initialization_status_ = 0;
    std::vector<uint8_t> object_, buffer_;
};
}

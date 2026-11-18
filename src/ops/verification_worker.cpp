#include "verification_worker.h"
#include <chrono>
namespace pulse::ops {
namespace {
struct FileLease { HANDLE value; ~FileLease() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); } };
bool Stamp(HANDLE file, VerificationStamp& out, DWORD& error) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info)) { error = GetLastError(); return false; }
    out = {info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow,
        (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow, info.ftLastWriteTime};
    error = ERROR_SUCCESS;
    return true;
}
bool Same(const VerificationStamp& a, const VerificationStamp& b) {
    return a.volume == b.volume && a.index_high == b.index_high && a.index_low == b.index_low &&
        a.bytes == b.bytes && CompareFileTime(&a.last_write, &b.last_write) == 0;
}
HANDLE Open(const std::wstring& path) {
    return CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
}
}
bool CaptureVerificationStamp(const std::wstring& path, VerificationStamp& stamp, DWORD& error) {
    FileLease file{Open(path)};
    if (file.value == INVALID_HANDLE_VALUE) { error = GetLastError(); return false; }
    return Stamp(file.value, stamp, error);
}
SHA256VerificationWorker::SHA256VerificationWorker() : buffer_(1024 * 1024) {
    initialization_status_ = BCryptOpenAlgorithmProvider(&provider_, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (initialization_status_ < 0) return;
    DWORD bytes = 0, returned = 0;
    initialization_status_ = BCryptGetProperty(provider_, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&bytes), sizeof(bytes), &returned, 0);
    if (initialization_status_ >= 0) object_.resize(bytes);
}
SHA256VerificationWorker::~SHA256VerificationWorker() {
    if (provider_) BCryptCloseAlgorithmProvider(provider_, 0);
}
bool SHA256VerificationWorker::Read(const std::wstring& path, bool source, const std::atomic<bool>& cancel,
    const std::function<void()>& wait, std::array<uint8_t, 32>& digest, VerificationStamp& stamp,
    VerificationResult& result) {
    using E = VerificationError;
    const auto started = std::chrono::steady_clock::now();
    struct Timer {
        uint64_t& output; std::chrono::steady_clock::time_point started;
        ~Timer() { output = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count()); }
    } timer{source ? result.source_wall_ns : result.target_wall_ns, started};
    auto fail = [&](E error) { result.error = error; return false; };
    if (cancel.load()) return fail(E::Cancelled);
    FileLease file{Open(path)};
    if (file.value == INVALID_HANDLE_VALUE) { result.win32_error = GetLastError(); return fail(source ? E::SourceOpen : E::TargetOpen); }
    if (!Stamp(file.value, stamp, result.win32_error)) return fail(source ? E::SourceStat : E::TargetStat);
    BCRYPT_HASH_HANDLE hash = nullptr;
    result.crypto_status = BCryptCreateHash(provider_, &hash, object_.data(), static_cast<ULONG>(object_.size()), nullptr, 0, 0);
    if (result.crypto_status < 0) return fail(source ? E::SourceHash : E::TargetHash);
    struct HashLease { BCRYPT_HASH_HANDLE value; ~HashLease() { BCryptDestroyHash(value); } } lease{hash};
    for (;;) {
        if (wait) wait();
        if (cancel.load()) return fail(E::Cancelled);
        DWORD bytes = 0;
        if (!ReadFile(file.value, buffer_.data(), static_cast<DWORD>(buffer_.size()), &bytes, nullptr)) {
            result.win32_error = GetLastError(); return fail(source ? E::SourceRead : E::TargetRead);
        }
        if (!bytes) break;
        (source ? result.source_bytes : result.target_bytes) += bytes;
        result.crypto_status = BCryptHashData(hash, buffer_.data(), bytes, 0);
        if (result.crypto_status < 0) return fail(source ? E::SourceHash : E::TargetHash);
    }
    VerificationStamp after;
    if (!Stamp(file.value, after, result.win32_error)) return fail(source ? E::SourceStat : E::TargetStat);
    if (!Same(stamp, after)) return fail(source ? E::SourceChanged : E::TargetChanged);
    if (cancel.load()) return fail(E::Cancelled);
    result.crypto_status = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    return result.crypto_status >= 0 || fail(source ? E::SourceHash : E::TargetHash);
}
VerificationResult SHA256VerificationWorker::Verify(const std::wstring& source, const std::wstring& target,
    const std::atomic<bool>& cancel, const std::function<void()>& wait, const VerificationStamp* before_copy,
    const VerificationStamp* expected_target) {
    VerificationResult result;
    if (cancel.load()) { result.error = VerificationError::Cancelled; return result; }
    if (initialization_status_ < 0) { result.error = VerificationError::Provider; result.crypto_status = initialization_status_; return result; }
    std::array<uint8_t, 32> source_hash{}, target_hash{};
    VerificationStamp source_stamp, target_stamp, final_stamp;
    if (!Read(source, true, cancel, wait, source_hash, source_stamp, result)) return result;
    if (before_copy && !Same(*before_copy, source_stamp)) { result.error = VerificationError::SourceChanged; return result; }
    if (!Read(target, false, cancel, wait, target_hash, target_stamp, result)) return result;
    // Copy callbacks can sample size/time before completion; only identity is stable.
    if (expected_target && (expected_target->volume != target_stamp.volume ||
        expected_target->index_high != target_stamp.index_high || expected_target->index_low != target_stamp.index_low)) {
        result.error = VerificationError::TargetChanged;
        return result;
    }
    if (cancel.load()) { result.error = VerificationError::Cancelled; return result; }
    // Detect identity replacement as well as changes while hashing the other side.
    if (!CaptureVerificationStamp(source, final_stamp, result.win32_error)) result.error = VerificationError::SourceStat;
    else if (!Same(source_stamp, final_stamp)) result.error = VerificationError::SourceChanged;
    else if (!CaptureVerificationStamp(target, final_stamp, result.win32_error)) result.error = VerificationError::TargetStat;
    else if (!Same(target_stamp, final_stamp)) result.error = VerificationError::TargetChanged;
    else if (source_hash != target_hash) result.error = VerificationError::Mismatch;
    return result;
}
}

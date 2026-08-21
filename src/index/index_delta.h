// index_delta.h — Append-only per-volume USN delta log.
//
// The mmap base (pulse-index.bin) is rewritten only on merge. Everyday
// create/rename/delete/attr patches append here so CatchUp never copies the
// full store or rebuilds sort arrays.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

namespace pulse::index {

inline constexpr uint32_t kDeltaVer = 1;

enum class DeltaOp : uint8_t {
    Add = 1,
    Patch = 2,
    Tomb = 3,
    UsnCkpt = 4,
};

enum class PatchBits : uint8_t {
    Meta = 1,
    Name = 2,
    Attr = 4,
};

class DeltaLog {
public:
    DeltaLog() = default;
    ~DeltaLog() { Close(); }
    DeltaLog(const DeltaLog&) = delete;
    DeltaLog& operator=(const DeltaLog&) = delete;

    bool Open(const std::wstring& path, uint64_t base_built);
    void Close();
    // Replace the log contents while retaining the canonical path. This is
    // used after a base snapshot is published; the old implementation lost
    // path_ while Close() was running and could not reopen the log.
    bool Reset(uint64_t base_built);

    void QueueAdd(int32_t parent, uint8_t flags, uint32_t mtime, uint64_t size,
                  std::wstring_view name, uint64_t frn);
    void QueuePatch(int32_t idx, uint8_t which, int32_t parent, uint8_t flags,
                    uint32_t mtime, uint64_t size, std::wstring_view name);
    void QueueTomb(int32_t idx);
    void QueueUsn(uint64_t journal_id, int64_t next_usn);

    bool Flush();
    uint64_t BytesOnDisk() const { return bytes_on_disk_; }
    bool HasPending() const { return !pending_.empty(); }
    uint64_t BaseBuilt() const { return base_built_; }

    using ReplayFn = std::function<void(DeltaOp op, int32_t idx, int32_t parent,
                                        uint8_t flags, uint8_t which,
                                        uint32_t mtime, uint64_t size,
                                        std::wstring_view name, uint64_t frn,
                                        uint64_t journal_id, int64_t next_usn)>;
    static bool Replay(const std::wstring& path, uint64_t expected_built, ReplayFn fn);

private:
    HANDLE file_ = INVALID_HANDLE_VALUE;
    std::wstring path_;
    std::vector<uint8_t> pending_;
    uint64_t bytes_on_disk_ = 0;
    uint64_t base_built_ = 0;

    void PutU8(uint8_t v);
    void PutU16(uint16_t v);
    void PutI32(int32_t v);
    void PutU64(uint64_t v);
    void PutBytes(const void* p, size_t n);
};

// Walk-mode log (letter 0), or legacy fallback when a volume has no stable id.
std::wstring DeltaFilePath(wchar_t letter);
// Delta file for a fixed NTFS volume, keyed by a hash of the stable volume
// id so a reassigned drive letter can never alias another volume's log.
std::wstring DeltaFilePathForVolume(const std::wstring& volume_id);

} // namespace pulse::index

#pragma once
#include <windows.h>
#include <array>
#include <cstdint>
#include <string>

namespace pulse::ops {
// A receipt, not permission to perform a later path-based destructive action.
// USN is advisory: journal lifetime and close-time updates are not pinned here.
struct UndoFileVersion {
    DWORD volume = 0, index_high = 0, index_low = 0, attributes = 0;
    uint64_t bytes = 0;
    LARGE_INTEGER modified{}, changed{};
    bool has_usn = false, has_sha256 = false;
    LONGLONG usn = 0;
    std::array<unsigned char, 32> sha256{};
};
// Default performs no full-content read. Hashing is opt-in and bounded per file.
// SHA covers the default data stream only, not ADS, ACLs or malicious mapped writers.
// Directory/reparse receipts cannot authorize recursive operations and are rejected.
bool CaptureUndoFileVersion(const std::wstring& path, UndoFileVersion& out, DWORD& error,
                            uint64_t hash_budget_bytes = 0);
class UndoFileLease {
public:
    ~UndoFileLease();
    UndoFileLease() = default;
    UndoFileLease(const UndoFileLease&) = delete;
    UndoFileLease& operator=(const UndoFileLease&) = delete;
    // Requires the same version and holds this file against new write/delete
    // opens. The returned handle must be used by any eventual mutation.
    bool Acquire(const std::wstring& path, const UndoFileVersion& expected, DWORD& error);
    HANDLE Get() const { return handle_; }
    void Reset();
private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};
}

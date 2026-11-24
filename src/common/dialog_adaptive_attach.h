#pragma once
#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pulse::dialog_adaptive {

inline constexpr uint32_t kMatchNone = 0u;
inline constexpr uint32_t kMatchLegacyImports = 1u << 0;
inline constexpr uint32_t kMatchWinForms = 1u << 1;
inline constexpr uint32_t kMatchWinRt = 1u << 2;
inline constexpr uint32_t kMatchDynamicGui = 1u << 3;
inline constexpr uint32_t kMatchLearned = 1u << 4;
inline constexpr uint32_t kMatchComDialog = 1u << 5;

// Hard exclusion check for system directories, Pulse's own directory, browsers,
// anti-cheat, security software and console hosts.
bool IsExcludedExecutable(const std::wstring& image_path,
                          const std::wstring& windows_dir,
                          const std::wstring& install_root) noexcept;

// Read-only remote PE import/delay-import and loaded-framework preflight.
// Returns ERROR_SUCCESS and populates match_mask (0 if no adaptive signal found).
DWORD ScanRemoteProcessForAdaptiveMatch(HANDLE process,
                                        DWORD pid,
                                        WORD expected_machine,
                                        const std::wstring& windows_dir,
                                        const std::wstring& install_root,
                                        uint32_t& match_mask) noexcept;

// Fast non-blocking check for whether a visible top-level window is a native
// Win32/Shell file or folder dialog (#32770 with file/browse controls).
bool IsNativeFileDialogWindow(HWND hwnd) noexcept;

// True for fatal crash/NTSTATUS exit codes (e.g. AV, failfast, stack overflow).
bool IsCrashExitCode(DWORD exit_code) noexcept;

// Computes a stable binary fingerprint combining lowercase canonical path,
// file size, and last write timestamp (hex string, empty on error).
// Legacy (v1) key: changes on every update or re-copy of the binary.
std::string ComputeBinaryFingerprint(const std::wstring& image_path) noexcept;

// Version-stable key (v2): lowercase path plus the VS_FIXEDFILEINFO file and
// product versions, so a learned application survives a re-copy or a same-version
// patch but not a version change. Images without a usable version resource fall
// back to the v1 key. Empty on error.
std::string ComputeBinaryFingerprintV2(const std::wstring& image_path) noexcept;

struct LearnedTarget {
    std::string basename;
    uint64_t updated_ft = 0;
};

struct BlockedTarget {
    std::string basename;
    uint32_t failures = 0;
    bool blocked = false;
    uint64_t updated_ft = 0;
};

// An active circuit breaker, as the settings page lists it.
struct PausedTarget {
    std::string fingerprint;
    std::string basename;      // UTF-8 executable name
    uint32_t failures = 0;
    uint64_t paused_ft = 0;    // FILETIME of the last failure
    uint64_t retry_ft = 0;     // FILETIME when the pause expires by itself
};

// Persistent per-user store for #32770 observation-learned targets and automatic
// circuit-breaker exclusions (%LOCALAPPDATA%\Pulse\dialog_adaptive_targets.json).
class AdaptiveTargetStore {
public:
    static constexpr size_t kMaxEntries = 512;
    static constexpr uint32_t kFailureThreshold = 2;
    // A tripped breaker is retried after this long (FILETIME units, 7 days), so a
    // transient failure never excludes an application permanently.
    static constexpr uint64_t kBlockedRetryAfter = 7ull * 24 * 60 * 60 * 10000000ull;

    bool Load(const std::filesystem::path& path) noexcept;
    bool Save(const std::filesystem::path& path) const noexcept;

    bool IsLearned(const std::string& fingerprint) const noexcept;
    bool IsBlocked(const std::string& fingerprint) const noexcept;
    // `now` is a FILETIME value; blocks older than kBlockedRetryAfter have expired.
    bool IsBlockedAt(const std::string& fingerprint, uint64_t now) const noexcept;

    // v1 -> v2 migration: moves a learned legacy entry to the version key.
    // Returns true when the store changed (caller saves).
    bool AdoptLegacyLearned(const std::string& fingerprint, const std::string& legacy) noexcept;

    // Returns true if newly learned or updated.
    bool RecordLearned(const std::string& fingerprint, std::wstring_view basename) noexcept;

    // Returns true if this call tripped (or kept) the circuit breaker.
    bool RecordFailure(const std::string& fingerprint, std::wstring_view basename, bool immediate_block) noexcept;

    void ClearFailureCount(const std::string& fingerprint) noexcept;
    void ClearBlocked() noexcept;

    // Breakers still active at `now` (FILETIME), newest first.
    std::vector<PausedTarget> PausedTargets(uint64_t now) const;
    // Ends every active pause recorded for this executable name (any version key,
    // case-insensitive). The next start of the program is attached again. Returns
    // how many entries were removed.
    size_t ResumeBasename(std::string_view basename) noexcept;
    // A long-running helper keeps this store in memory while the settings page may
    // replace the file. Reloads when the file differs from what this store last
    // read or wrote. Returns true when it reloaded.
    bool ReloadIfChanged(const std::filesystem::path& path) noexcept;

    size_t LearnedCount() const noexcept { return learned_.size(); }
    size_t BlockedCount() const noexcept;

private:
    struct FileStamp { uint64_t write = 0, size = 0; bool present = false; bool operator==(const FileStamp&) const = default; };
    static FileStamp Stamp(const std::filesystem::path& path) noexcept;
    std::unordered_map<std::string, LearnedTarget> learned_;
    std::unordered_map<std::string, BlockedTarget> failures_;
    mutable FileStamp stamp_{};
    mutable bool stamped_ = false;
};

} // namespace pulse::dialog_adaptive

#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::dialog_bridge_state {

inline constexpr uint32_t kStateMagic = 0x50444253u; // 'PDBS'
inline constexpr uint32_t kStateVersion = 1u;
inline constexpr size_t kMaxPathChars = 260;
inline constexpr size_t kMaxExclusions = 64;

inline constexpr wchar_t kDefaultGlobalStateName[] = L"Global\\PulseDialogBridgeState";
inline constexpr wchar_t kDefaultActiveTakeoverName[] = L"Local\\PulseDialogActiveTakeover";

inline constexpr uint32_t kFlagGlobalEnabled       = 0x00000001u;
inline constexpr uint32_t kFlagSinkHooksEnabled    = 0x00000002u;
inline constexpr uint32_t kFlagWinRtEnabled        = 0x00000004u;
inline constexpr uint32_t kFlagDirectRevealEnabled = 0x00000008u;
inline constexpr uint32_t kFlagOverlayEnabled      = 0x00000010u;
inline constexpr uint32_t kFlagOverlayModeB        = 0x00000020u;

#pragma pack(push, 8)
// Read-only global configuration section published by PulseDialogBroker (SYSTEM/Admins write,
// Builtin Users + ALL APPLICATION PACKAGES read-only). Readers validate seqlock `sequence`.
struct DialogBridgeStateV1 {
    uint32_t magic = kStateMagic;
    uint32_t version = kStateVersion;
    uint32_t size = sizeof(DialogBridgeStateV1);
    uint32_t flags = 0;
    volatile LONG sequence = 0; // Seqlock: odd = writer active, even = stable snapshot.
    uint32_t reserved0 = 0;
    uint64_t generation = 0;
    uint64_t updated_filetime = 0;
    wchar_t bridge_root[kMaxPathChars]{};
    wchar_t core_dir_x64[kMaxPathChars]{};
    wchar_t core_dir_x86[kMaxPathChars]{};
    wchar_t host_exe[kMaxPathChars]{};
    uint32_t exclusion_count = 0;
    uint32_t reserved1 = 0;
    uint64_t excluded_name_hashes[kMaxExclusions]{};
};

// Companion per-user deduplication table (Layer 1/2 marks active dialogs so Layer 3
// overlay never double-takes over the same process/window).
inline constexpr uint32_t kActiveMagic = 0x50444154u; // 'PDAT'
inline constexpr size_t kMaxActiveDialogs = 32;

struct ActiveDialogEntry {
    uint32_t pid;
    uint32_t layer; // 1 = COM, 2 = SinkHook, 3 = Overlay
    uint64_t owner_hwnd;
    uint64_t timestamp_ms;
};

struct ActiveDialogTableV1 {
    uint32_t magic = kActiveMagic;
    uint32_t version = 1;
    volatile LONG lock_word = 0;
    uint32_t reserved = 0;
    ActiveDialogEntry entries[kMaxActiveDialogs]{};
};
#pragma pack(pop)

// Case-insensitive FNV-1a 64-bit hash for process basenames (e.g. L"chrome.exe").
uint64_t HashProcessName(std::wstring_view basename) noexcept;

struct StateSnapshot {
    uint32_t flags = 0;
    uint64_t generation = 0;
    uint64_t updated_filetime = 0;
    std::wstring bridge_root;
    std::wstring core_dir_x64;
    std::wstring core_dir_x86;
    std::wstring host_exe;
    std::vector<uint64_t> excluded_hashes;

    bool IsProcessExcluded(std::wstring_view basename) const noexcept;
};

// Writer side (PulseDialogBroker or isolated test).
class StatePublisher {
public:
    StatePublisher() = default;
    ~StatePublisher();
    StatePublisher(const StatePublisher&) = delete;
    StatePublisher& operator=(const StatePublisher&) = delete;

    // Creates or opens the named section. `restricted_dacl` applies
    // SYSTEM/Admins GA + Users/AppContainer GR when creating Global\ sections.
    DWORD Open(const std::wstring& section_name = kDefaultGlobalStateName,
               bool restricted_dacl = true) noexcept;
    DWORD Publish(const StateSnapshot& snapshot) noexcept;
    void Close() noexcept;
    bool IsOpen() const noexcept { return view_ != nullptr; }

private:
    HANDLE mapping_ = nullptr;
    DialogBridgeStateV1* view_ = nullptr;
};

// Lock-free reader side (any target process, Stub, Core, or Overlay).
class StateReader {
public:
    StateReader() = default;
    ~StateReader();
    StateReader(const StateReader&) = delete;
    StateReader& operator=(const StateReader&) = delete;

    DWORD Open(const std::wstring& section_name = kDefaultGlobalStateName) noexcept;
    bool Read(StateSnapshot& out) const noexcept;
    void Close() noexcept;
    bool IsOpen() const noexcept { return view_ != nullptr; }

private:
    HANDLE mapping_ = nullptr;
    const DialogBridgeStateV1* view_ = nullptr;
};

// Per-user active-dialog deduplication ledger shared between Layer 1/2 and Layer 3.
class ActiveDialogLedger {
public:
    ActiveDialogLedger() = default;
    ~ActiveDialogLedger();
    ActiveDialogLedger(const ActiveDialogLedger&) = delete;
    ActiveDialogLedger& operator=(const ActiveDialogLedger&) = delete;

    DWORD OpenOrCreate(const std::wstring& section_name = kDefaultActiveTakeoverName) noexcept;
    void MarkActive(uint32_t pid, uint64_t owner_hwnd, uint32_t layer) noexcept;
    void ClearActive(uint32_t pid, uint64_t owner_hwnd = 0) noexcept;
    // Returns true if another layer has an active takeover for `pid` (within `max_age_ms`).
    bool IsTakenOver(uint32_t pid, uint64_t max_age_ms = 300000) const noexcept;
    void Close() noexcept;

private:
    HANDLE mapping_ = nullptr;
    ActiveDialogTableV1* view_ = nullptr;
};

} // namespace pulse::dialog_bridge_state

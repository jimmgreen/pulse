#include "dialog_bridge_state.h"

#include <sddl.h>
#include <algorithm>
#include <cstring>

namespace pulse::dialog_bridge_state {
namespace {

constexpr wchar_t kGlobalStateSddl[] =
    L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;BU)(A;;GR;;;AC)";

void CopyBounded(wchar_t (&dst)[kMaxPathChars], const std::wstring& src) noexcept {
    std::wmemset(dst, 0, kMaxPathChars);
    const size_t n = std::min(src.size(), kMaxPathChars - 1);
    if (n > 0) std::wmemcpy(dst, src.data(), n);
}

std::wstring ReadBounded(const wchar_t (&src)[kMaxPathChars]) {
    const size_t len = wcsnlen(src, kMaxPathChars);
    return std::wstring(src, len);
}

void AcquireSpin(volatile LONG* lock) noexcept {
    while (InterlockedCompareExchange(lock, 1, 0) != 0) {
        YieldProcessor();
    }
}

void ReleaseSpin(volatile LONG* lock) noexcept {
    InterlockedExchange(lock, 0);
}

} // namespace

uint64_t HashProcessName(std::wstring_view basename) noexcept {
    uint64_t hash = 14695981039346656037ull;
    for (wchar_t ch : basename) {
        if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
        const uint16_t u = static_cast<uint16_t>(ch);
        hash = (hash ^ (u & 0xFFu)) * 1099511628211ull;
        hash = (hash ^ (u >> 8)) * 1099511628211ull;
    }
    return hash;
}

bool StateSnapshot::IsProcessExcluded(std::wstring_view basename) const noexcept {
    if (basename.empty() || excluded_hashes.empty()) return false;
    const uint64_t h = HashProcessName(basename);
    for (uint64_t item : excluded_hashes) {
        if (item == h) return true;
    }
    return false;
}

StatePublisher::~StatePublisher() { Close(); }

DWORD StatePublisher::Open(const std::wstring& section_name, bool restricted_dacl) noexcept {
    Close();
    PSECURITY_DESCRIPTOR sd = nullptr;
    SECURITY_ATTRIBUTES sa{};
    SECURITY_ATTRIBUTES* sa_ptr = nullptr;
    if (restricted_dacl) {
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                kGlobalStateSddl, SDDL_REVISION_1, &sd, nullptr)) {
            return GetLastError();
        }
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = sd;
        sa.bInheritHandle = FALSE;
        sa_ptr = &sa;
    }

    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, sa_ptr, PAGE_READWRITE,
                                    0, static_cast<DWORD>(sizeof(DialogBridgeStateV1)),
                                    section_name.c_str());
    const DWORD map_err = map ? ERROR_SUCCESS : GetLastError();
    if (sd) LocalFree(sd);
    if (!map) return map_err;

    void* mem = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(DialogBridgeStateV1));
    if (!mem) {
        const DWORD err = GetLastError();
        CloseHandle(map);
        return err;
    }

    mapping_ = map;
    view_ = static_cast<DialogBridgeStateV1*>(mem);
    if (view_->magic != kStateMagic || view_->version != kStateVersion) {
        std::memset(view_, 0, sizeof(DialogBridgeStateV1));
        view_->magic = kStateMagic;
        view_->version = kStateVersion;
        view_->size = sizeof(DialogBridgeStateV1);
        InterlockedExchange(&view_->sequence, 2);
    }
    return ERROR_SUCCESS;
}

DWORD StatePublisher::Publish(const StateSnapshot& snapshot) noexcept {
    if (!view_) return ERROR_INVALID_HANDLE;

    // Seqlock begin: advance to an odd value with release fence.
    LONG seq = InterlockedIncrement(&view_->sequence);
    if ((seq & 1) == 0) seq = InterlockedIncrement(&view_->sequence);
    MemoryBarrier();

    view_->magic = kStateMagic;
    view_->version = kStateVersion;
    view_->size = sizeof(DialogBridgeStateV1);
    view_->flags = snapshot.flags;
    view_->generation = snapshot.generation;
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const uint64_t ft = (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    view_->updated_filetime = snapshot.updated_filetime ? snapshot.updated_filetime : ft;

    CopyBounded(view_->bridge_root, snapshot.bridge_root);
    CopyBounded(view_->core_dir_x64, snapshot.core_dir_x64);
    CopyBounded(view_->core_dir_x86, snapshot.core_dir_x86);
    CopyBounded(view_->host_exe, snapshot.host_exe);

    const size_t count = std::min(snapshot.excluded_hashes.size(), kMaxExclusions);
    view_->exclusion_count = static_cast<uint32_t>(count);
    std::memset(view_->excluded_name_hashes, 0, sizeof(view_->excluded_name_hashes));
    for (size_t i = 0; i < count; ++i) {
        view_->excluded_name_hashes[i] = snapshot.excluded_hashes[i];
    }

    MemoryBarrier();
    // Seqlock commit: advance to even value so lock-free readers observe consistent data.
    InterlockedIncrement(&view_->sequence);
    return ERROR_SUCCESS;
}

void StatePublisher::Close() noexcept {
    if (view_) {
        UnmapViewOfFile(view_);
        view_ = nullptr;
    }
    if (mapping_) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
    }
}

StateReader::~StateReader() { Close(); }

DWORD StateReader::Open(const std::wstring& section_name) noexcept {
    Close();
    HANDLE map = OpenFileMappingW(FILE_MAP_READ, FALSE, section_name.c_str());
    if (!map) return GetLastError();
    void* mem = MapViewOfFile(map, FILE_MAP_READ, 0, 0, sizeof(DialogBridgeStateV1));
    if (!mem) {
        const DWORD err = GetLastError();
        CloseHandle(map);
        return err;
    }
    mapping_ = map;
    view_ = static_cast<const DialogBridgeStateV1*>(mem);
    return ERROR_SUCCESS;
}

bool StateReader::Read(StateSnapshot& out) const noexcept {
    out = {};
    if (!view_) return false;
    try {
        DialogBridgeStateV1 local{};
        for (int attempt = 0; attempt < 32; ++attempt) {
            // Read-only view (FILE_MAP_READ): must use pure acquire load, NEVER
            // InterlockedCompareExchange (which issues a write cycle and faults PAGE_READONLY).
            const LONG seq1 = ReadAcquire(&view_->sequence);
            if (seq1 & 1) {
                YieldProcessor();
                continue;
            }
            std::memcpy(&local, view_, sizeof(local));
            MemoryBarrier();
            const LONG seq2 = ReadAcquire(&view_->sequence);
            if (seq1 == seq2 && local.magic == kStateMagic &&
                local.version == kStateVersion && local.size == sizeof(DialogBridgeStateV1)) {
                out.flags = local.flags;
                out.generation = local.generation;
                out.updated_filetime = local.updated_filetime;
                out.bridge_root = ReadBounded(local.bridge_root);
                out.core_dir_x64 = ReadBounded(local.core_dir_x64);
                out.core_dir_x86 = ReadBounded(local.core_dir_x86);
                out.host_exe = ReadBounded(local.host_exe);
                const size_t n = std::min<size_t>(local.exclusion_count, kMaxExclusions);
                out.excluded_hashes.assign(local.excluded_name_hashes, local.excluded_name_hashes + n);
                return true;
            }
        }
        return false;
    } catch (...) {
        return false;
    }
}

void StateReader::Close() noexcept {
    if (view_) {
        UnmapViewOfFile(view_);
        view_ = nullptr;
    }
    if (mapping_) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
    }
}

ActiveDialogLedger::~ActiveDialogLedger() { Close(); }

DWORD ActiveDialogLedger::OpenOrCreate(const std::wstring& section_name) noexcept {
    Close();
    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                    0, static_cast<DWORD>(sizeof(ActiveDialogTableV1)),
                                    section_name.c_str());
    if (!map) return GetLastError();
    void* mem = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ActiveDialogTableV1));
    if (!mem) {
        const DWORD err = GetLastError();
        CloseHandle(map);
        return err;
    }
    mapping_ = map;
    view_ = static_cast<ActiveDialogTableV1*>(mem);
    if (view_->magic != kActiveMagic) {
        std::memset(view_, 0, sizeof(ActiveDialogTableV1));
        view_->magic = kActiveMagic;
        view_->version = 1;
    }
    return ERROR_SUCCESS;
}

void ActiveDialogLedger::MarkActive(uint32_t pid, uint64_t owner_hwnd, uint32_t layer) noexcept {
    if (!view_ || !pid) return;
    const uint64_t now = GetTickCount64();
    AcquireSpin(&view_->lock_word);
    size_t target = kMaxActiveDialogs;
    size_t oldest_idx = 0;
    uint64_t oldest_ts = UINT64_MAX;
    for (size_t i = 0; i < kMaxActiveDialogs; ++i) {
        auto& e = view_->entries[i];
        if (e.pid == pid && (owner_hwnd == 0 || e.owner_hwnd == owner_hwnd)) {
            target = i;
            break;
        }
        if (e.pid == 0 && target == kMaxActiveDialogs) {
            target = i;
        }
        if (e.timestamp_ms < oldest_ts) {
            oldest_ts = e.timestamp_ms;
            oldest_idx = i;
        }
    }
    if (target == kMaxActiveDialogs) target = oldest_idx;
    view_->entries[target] = ActiveDialogEntry{pid, layer, owner_hwnd, now};
    ReleaseSpin(&view_->lock_word);
}

void ActiveDialogLedger::ClearActive(uint32_t pid, uint64_t owner_hwnd) noexcept {
    if (!view_ || !pid) return;
    AcquireSpin(&view_->lock_word);
    for (size_t i = 0; i < kMaxActiveDialogs; ++i) {
        auto& e = view_->entries[i];
        if (e.pid == pid && (owner_hwnd == 0 || e.owner_hwnd == owner_hwnd)) {
            e = {};
        }
    }
    ReleaseSpin(&view_->lock_word);
}

bool ActiveDialogLedger::IsTakenOver(uint32_t pid, uint64_t max_age_ms) const noexcept {
    if (!view_ || !pid) return false;
    const uint64_t now = GetTickCount64();
    AcquireSpin(&view_->lock_word);
    bool found = false;
    for (size_t i = 0; i < kMaxActiveDialogs; ++i) {
        const auto& e = view_->entries[i];
        if (e.pid == pid && e.layer != 0 && now >= e.timestamp_ms && (now - e.timestamp_ms) <= max_age_ms) {
            found = true;
            break;
        }
    }
    ReleaseSpin(&view_->lock_word);
    return found;
}

void ActiveDialogLedger::Close() noexcept {
    if (view_) {
        UnmapViewOfFile(view_);
        view_ = nullptr;
    }
    if (mapping_) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
    }
}

} // namespace pulse::dialog_bridge_state

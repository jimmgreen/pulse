#pragma once
// Drives that behave like network storage although Windows reports them as
// local: cloud drives mounted through a user-mode file system (WinFsp, Dokan,
// CBFS: Baidu's virtual disk, CloudDrive, rclone, Google Drive, ...). Their
// "disk" is a remote service, so Pulse treats them like mapped network
// drives: no index, no folder sizes, no recycle-bin scan, the remote copy
// strategy, and "Network drive" as the This PC type.
//
// Header-only on purpose: the app, the index service, the content indexer
// and the tests each compile their own source lists.
#include <windows.h>
#include <winioctl.h>
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

namespace pulse::fs {

// QueryDosDevice target of a drive letter. User-mode file systems create
// \Device\Volume{GUID} disk devices; real partitions, VHDs and BitLocker
// volumes are \Device\HarddiskVolumeN, subst drives \??\C:\..., RAM disks and
// encrypted containers use their own driver names.
inline bool IsUserModeVolumeDevice(std::wstring_view target) {
    constexpr std::wstring_view kPrefix = L"\\Device\\Volume{";
    return target.size() > kPrefix.size() &&
           CompareStringOrdinal(target.data(), static_cast<int>(kPrefix.size()), kPrefix.data(),
                                static_cast<int>(kPrefix.size()), TRUE) == CSTR_EQUAL;
}

// File system names (GetVolumeInformationW) of user-mode / cloud mounts.
// Only for callers that already read the volume information: asking a stuck
// mount for it can block, the device test above cannot.
inline bool IsRemoteLikeFileSystemName(std::wstring_view name) {
    constexpr std::wstring_view kNames[] = {
        L"FUSE", L"WinFsp", L"Dokan", L"CloudDrive", L"rclone", L"Baidu", L"CBFS",
    };
    for (const auto needle : kNames) {
        if (name.size() < needle.size()) continue;
        for (size_t at = 0; at + needle.size() <= name.size(); ++at)
            if (CompareStringOrdinal(name.data() + at, static_cast<int>(needle.size()), needle.data(),
                                     static_cast<int>(needle.size()), TRUE) == CSTR_EQUAL)
                return true;
    }
    return false;
}

// Drive letter of "X:...", "\\?\X:..." or "\\.\X:..."; 0 for anything else
// (UNC paths are network paths in their own right).
inline wchar_t DriveLetterOf(std::wstring_view path) {
    if (path.size() >= 4 && path[0] == L'\\' && path[1] == L'\\' && (path[2] == L'?' || path[2] == L'.') &&
        path[3] == L'\\')
        path.remove_prefix(4);
    if (path.size() < 2 || path[1] != L':') return 0;
    const wchar_t c = path[0];
    if (c >= L'a' && c <= L'z') return static_cast<wchar_t>(c - L'a' + L'A');
    return c >= L'A' && c <= L'Z' ? c : 0;
}

namespace detail {
inline bool TestMarksDriveList(const wchar_t* env_name, wchar_t letter) {
    if (!GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", nullptr, 0)) return false;
    wchar_t letters[32]{};
    const DWORD n = GetEnvironmentVariableW(env_name, letters, ARRAYSIZE(letters));
    if (!n || n >= ARRAYSIZE(letters)) return false;
    for (DWORD i = 0; i < n; ++i) {
        wchar_t c = letters[i];
        if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
        if (c == letter) return true;
    }
    return false;
}

// --test-instance only: PULSE_TEST_REMOTE_LIKE_DRIVES=XY marks letters as
// user-mode mounts, so shots and tests need no real WinFsp drive.
inline bool TestMarksRemoteLike(wchar_t letter) {
    return TestMarksDriveList(L"PULSE_TEST_REMOTE_LIKE_DRIVES", letter);
}

// --test-instance only: PULSE_TEST_SPINNING_DRIVES=XY marks letters as
// mechanical HDDs (IncursSeekPenalty=true) for deterministic testing.
inline bool TestMarksSpinning(wchar_t letter) {
    return TestMarksDriveList(L"PULSE_TEST_SPINNING_DRIVES", letter);
}

inline std::atomic<uint8_t>(&SeekPenaltySlots())[26] {
    static std::atomic<uint8_t> slots[26]{};
    return slots;
}

inline std::atomic<uint32_t>& SeekPenaltyInFlight() {
    static std::atomic<uint32_t> mask{0};
    return mask;
}
} // namespace detail

// True for mapped network drives and user-mode (cloud) mounts. Costs a
// GetDriveTypeW and a QueryDosDeviceW: object-manager lookups, no media
// access, safe on a drive whose service hangs.
inline bool IsRemoteLikeDrive(std::wstring_view path) {
    const wchar_t letter = DriveLetterOf(path);
    if (!letter) return false;
    const wchar_t root[4] = {letter, L':', L'\\', L'\0'};
    const UINT type = GetDriveTypeW(root);
    if (type == DRIVE_REMOTE) return true;
    if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) return false;
    if (detail::TestMarksRemoteLike(letter)) return true;
    const wchar_t device[3] = {letter, L':', L'\0'};
    wchar_t target[512]{};
    if (!QueryDosDeviceW(device, target, ARRAYSIZE(target))) return false;
    return IsUserModeVolumeDevice(target);  // first string = current mapping
}

enum class SeekPenalty : uint8_t { Unknown = 0, Solid = 1, Spinning = 2 };

// Pure in-memory lookup; never touches the disk or blocks the UI thread.
inline SeekPenalty CachedSeekPenalty(std::wstring_view path) {
    const wchar_t letter = DriveLetterOf(path);
    if (!letter) return SeekPenalty::Unknown;
    if (detail::TestMarksSpinning(letter)) return SeekPenalty::Spinning;
    const uint8_t state = detail::SeekPenaltySlots()[static_cast<size_t>(letter - L'A')]
        .load(std::memory_order_acquire);
    if (state == 1) return SeekPenalty::Solid;
    if (state == 2) return SeekPenalty::Spinning;
    return SeekPenalty::Unknown;
}

inline bool IsKnownSpinningDrive(std::wstring_view path) {
    return CachedSeekPenalty(path) == SeekPenalty::Spinning;
}

inline bool SeekPenaltyProbed(std::wstring_view path) {
    const wchar_t letter = DriveLetterOf(path);
    if (!letter) return true;
    if (detail::TestMarksSpinning(letter)) return true;
    return detail::SeekPenaltySlots()[static_cast<size_t>(letter - L'A')]
        .load(std::memory_order_acquire) != 0;
}

// Claims one background probe per unprobed drive letter so concurrent UI
// navigations or hovers do not flood the I/O worker with duplicate queries.
inline bool TryBeginSeekPenaltyProbe(std::wstring_view path) {
    const wchar_t letter = DriveLetterOf(path);
    if (!letter || detail::TestMarksSpinning(letter)) return false;
    const size_t idx = static_cast<size_t>(letter - L'A');
    if (detail::SeekPenaltySlots()[idx].load(std::memory_order_acquire) != 0) return false;
    const uint32_t bit = 1u << idx;
    return (detail::SeekPenaltyInFlight().fetch_or(bit, std::memory_order_acq_rel) & bit) == 0;
}

// Background-thread query for StorageDeviceSeekPenaltyProperty. Caches the
// outcome per drive letter until InvalidateDriveKindCache (WM_DEVICECHANGE).
inline SeekPenalty ProbeSeekPenalty(std::wstring_view path) {
    const wchar_t letter = DriveLetterOf(path);
    if (!letter) return SeekPenalty::Unknown;
    if (detail::TestMarksSpinning(letter)) return SeekPenalty::Spinning;
    const size_t idx = static_cast<size_t>(letter - L'A');
    auto& slot = detail::SeekPenaltySlots()[idx];
    const uint8_t cached = slot.load(std::memory_order_acquire);
    if (cached == 1) return SeekPenalty::Solid;
    if (cached == 2) return SeekPenalty::Spinning;
    if (cached == 3) return SeekPenalty::Unknown;

    SeekPenalty result = SeekPenalty::Unknown;
    const wchar_t root[4] = {letter, L':', L'\\', L'\0'};
    if (!IsRemoteLikeDrive(root) && GetDriveTypeW(root) == DRIVE_FIXED) {
        std::wstring device;
        wchar_t volume[MAX_PATH]{};
        if (GetVolumeNameForVolumeMountPointW(root, volume, MAX_PATH)) {
            device = volume;
            if (!device.empty() && device.back() == L'\\') device.pop_back();
        } else {
            device = std::wstring(L"\\\\.\\") + letter + L":";
        }
        HANDLE handle = CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            STORAGE_PROPERTY_QUERY query{};
            query.PropertyId = StorageDeviceSeekPenaltyProperty;
            query.QueryType = PropertyStandardQuery;
            DEVICE_SEEK_PENALTY_DESCRIPTOR seek{};
            DWORD returned = 0;
            if (DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                                &seek, sizeof(seek), &returned, nullptr) &&
                returned >= sizeof(seek)) {
                result = seek.IncursSeekPenalty ? SeekPenalty::Spinning : SeekPenalty::Solid;
            }
            CloseHandle(handle);
        }
    }
    const uint8_t stored = result == SeekPenalty::Solid ? 1u
        : (result == SeekPenalty::Spinning ? 2u : 3u);
    slot.store(stored, std::memory_order_release);
    detail::SeekPenaltyInFlight().fetch_and(~(1u << idx), std::memory_order_release);
    return result;
}

inline void InvalidateDriveKindCache(wchar_t letter = 0) {
    auto& slots = detail::SeekPenaltySlots();
    if (letter >= L'a' && letter <= L'z') letter = static_cast<wchar_t>(letter - L'a' + L'A');
    if (letter >= L'A' && letter <= L'Z') {
        const size_t idx = static_cast<size_t>(letter - L'A');
        slots[idx].store(0, std::memory_order_release);
        detail::SeekPenaltyInFlight().fetch_and(~(1u << idx), std::memory_order_release);
        return;
    }
    for (auto& slot : slots) slot.store(0, std::memory_order_release);
    detail::SeekPenaltyInFlight().store(0, std::memory_order_release);
}

} // namespace pulse::fs

#include "index_volume_walk.h"
#include <algorithm>
#include <cstring>
#include <cwctype>
#include <vector>

namespace pulse::index {
namespace {

bool EqualsOrdinalIgnoreCase(std::wstring_view a, std::wstring_view b) {
    return a.size() == b.size() &&
        CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
                             b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

std::wstring_view TrimView(std::wstring_view s) {
    while (!s.empty() && iswspace(s.front())) s.remove_prefix(1);
    while (!s.empty() && iswspace(s.back())) s.remove_suffix(1);
    return s;
}

using NtStatus = LONG;

struct NtIoStatusBlock {
    union {
        NtStatus Status;
        PVOID Pointer;
    };
    ULONG_PTR Information;
};

#pragma pack(push, 8)
struct NtFileFullDirectoryInfo {
    ULONG NextEntryOffset;
    ULONG FileIndex;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG FileAttributes;
    ULONG FileNameLength;
    ULONG EaSize;
    WCHAR FileName[1];
};
#pragma pack(pop)

using NtQueryDirectoryFileFn = NtStatus(NTAPI*)(
    HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext,
    NtIoStatusBlock* IoStatusBlock, PVOID FileInformation, ULONG Length,
    ULONG FileInformationClass, BOOLEAN ReturnSingleEntry,
    PVOID FileName, BOOLEAN RestartScan);

using RtlNtStatusToDosErrorFn = ULONG(NTAPI*)(NtStatus Status);

NtQueryDirectoryFileFn GetNtQueryDirectoryFile() {
    static const NtQueryDirectoryFileFn fn = []() -> NtQueryDirectoryFileFn {
        if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
            return reinterpret_cast<NtQueryDirectoryFileFn>(
                GetProcAddress(ntdll, "NtQueryDirectoryFile"));
        }
        return nullptr;
    }();
    return fn;
}

DWORD NtStatusToWin32(NtStatus status) {
    static const RtlNtStatusToDosErrorFn fn = []() -> RtlNtStatusToDosErrorFn {
        if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
            return reinterpret_cast<RtlNtStatusToDosErrorFn>(
                GetProcAddress(ntdll, "RtlNtStatusToDosError"));
        }
        return nullptr;
    }();
    return fn ? fn(status) : ERROR_GEN_FAILURE;
}

bool IsDotName(std::wstring_view name) {
    return (name.size() == 1 && name[0] == L'.') ||
           (name.size() == 2 && name[0] == L'.' && name[1] == L'.');
}

} // namespace

std::wstring ToWin32LongPath(std::wstring_view path) {
    std::wstring clean(path);
    std::replace(clean.begin(), clean.end(), L'/', L'\\');
    if (clean.empty()) return clean;
    if (clean.rfind(L"\\\\?\\", 0) == 0) {
        if (clean.size() == 6 && clean[5] == L':') clean.push_back(L'\\');
        return clean;
    }
    if (clean.size() == 2 && clean[1] == L':') clean.push_back(L'\\');
    if (clean.rfind(L"\\\\", 0) == 0) {
        return L"\\\\?\\UNC\\" + clean.substr(2);
    }
    if (clean.size() >= 3 && clean[1] == L':' && clean[2] == L'\\') {
        return L"\\\\?\\" + clean;
    }
    return clean;
}

VolumeFsTag FsTagFromName(std::wstring_view file_system) {
    if (EqualsOrdinalIgnoreCase(file_system, L"FAT32")) return VolumeFsTag::Fat32;
    if (EqualsOrdinalIgnoreCase(file_system, L"EXFAT")) return VolumeFsTag::ExFat;
    if (EqualsOrdinalIgnoreCase(file_system, L"FAT") ||
        EqualsOrdinalIgnoreCase(file_system, L"FAT16") ||
        EqualsOrdinalIgnoreCase(file_system, L"FAT12")) {
        return VolumeFsTag::Fat;
    }
    return VolumeFsTag::UnknownOrNtfs;
}

std::wstring_view FsNameFromTag(VolumeFsTag tag, VolumeBackend backend) {
    switch (tag) {
    case VolumeFsTag::Fat32: return L"FAT32";
    case VolumeFsTag::ExFat: return L"exFAT";
    case VolumeFsTag::Fat: return L"FAT";
    case VolumeFsTag::UnknownOrNtfs:
    default:
        return backend == VolumeBackend::DirectoryWalk ? L"FAT/exFAT" : L"NTFS";
    }
}

uint16_t PackDiskVolKind(VolumeKind kind, VolumeBackend backend, VolumeFsTag fs_tag) {
    const uint16_t k = static_cast<uint16_t>(static_cast<uint32_t>(kind) & 0x0Fu);
    const uint16_t b = static_cast<uint16_t>(static_cast<uint8_t>(backend) & 0x0Fu);
    const uint16_t f = static_cast<uint16_t>(static_cast<uint8_t>(fs_tag) & 0x0Fu);
    return static_cast<uint16_t>(k | (b << 4) | (f << 8));
}

void UnpackDiskVolMeta(uint16_t raw_kind, uint64_t journal_id, int64_t next_usn,
                       VolumeKind& kind, VolumeBackend& backend,
                       VolumeFsTag& fs_tag, uint32_t& serial) {
    kind = static_cast<VolumeKind>(raw_kind & 0x0Fu);
    const uint8_t raw_backend = static_cast<uint8_t>((raw_kind >> 4) & 0x0Fu);
    const uint8_t raw_tag = static_cast<uint8_t>((raw_kind >> 8) & 0x0Fu);
    if (raw_backend == 0) {
        // Legacy V9/V11 snapshot without explicit backend bits.
        backend = (journal_id != 0 || next_usn != 0)
            ? VolumeBackend::NtfsMftUsn
            : VolumeBackend::Unsupported;
        fs_tag = VolumeFsTag::UnknownOrNtfs;
    } else {
        backend = static_cast<VolumeBackend>(raw_backend);
        fs_tag = static_cast<VolumeFsTag>(raw_tag);
    }
    serial = (backend == VolumeBackend::DirectoryWalk)
        ? static_cast<uint32_t>(static_cast<uint64_t>(next_usn) & 0xFFFFFFFFull)
        : 0u;
}

bool VolumeIdentityMatches(std::wstring_view drive_id, std::wstring_view drive_fs,
                           uint32_t drive_serial, std::wstring_view state_id,
                           VolumeBackend state_backend, VolumeFsTag state_fs_tag,
                           uint32_t state_serial) {
    auto trim_id = [](std::wstring_view s) {
        s = TrimView(s);
        if (s.size() > 4 && s.back() == L'\\') s.remove_suffix(1);
        return s;
    };
    const auto a = trim_id(drive_id);
    const auto b = trim_id(state_id);
    if (a.empty() || b.empty() || !EqualsOrdinalIgnoreCase(a, b)) return false;

    const VolumeBackend drive_backend = SelectVolumeBackend(drive_fs);
    if (drive_backend != VolumeBackend::Unsupported &&
        state_backend != VolumeBackend::Unsupported &&
        drive_backend != state_backend) {
        return false;
    }
    if (state_backend == VolumeBackend::DirectoryWalk) {
        const VolumeFsTag drive_tag = FsTagFromName(drive_fs);
        if (drive_tag != VolumeFsTag::UnknownOrNtfs &&
            state_fs_tag != VolumeFsTag::UnknownOrNtfs &&
            drive_tag != state_fs_tag) {
            return false;
        }
        if (drive_serial != 0 && state_serial != 0 && drive_serial != state_serial) {
            return false;
        }
    }
    return true;
}

bool WalkDirectoryTree(const std::wstring& root_path, int32_t root_id,
                       bool root_is_volume_root, size_t max_items,
                       const WalkCallbacks& cb, WalkStats* out_stats) {
    WalkStats stats;
    auto finish = [&](bool ok) {
        if (out_stats) *out_stats = stats;
        return ok;
    };
    if (root_path.empty() || root_id < 0 || !cb.emit_node) {
        stats.root_error = ERROR_INVALID_PARAMETER;
        return finish(false);
    }

    struct Frame {
        int32_t parent_id = -1;
        std::wstring path;
        bool is_root = false;
        bool hidden_parent = false;
        bool check_exclusions = false;
    };

    std::wstring clean_root = root_path;
    std::replace(clean_root.begin(), clean_root.end(), L'/', L'\\');
    while (clean_root.size() > 3 && clean_root.back() == L'\\') clean_root.pop_back();
    if (clean_root.size() == 3 && clean_root[1] == L':' && clean_root[2] == L'\\') {
        clean_root.pop_back(); // keep "X:" so child join cleanly forms "X:\name"
    }

    std::vector<Frame> stack;
    stack.reserve(128);
    stack.push_back(Frame{
        root_id,
        std::move(clean_root),
        root_is_volume_root,
        false,
        cb.is_excluded_path && (!cb.may_contain_exclusion || cb.may_contain_exclusion(root_path))
    });

    constexpr ULONG kNtFileFullDirectoryInformation = 2;
    constexpr NtStatus kStatusPending = 0x00000103L;
    constexpr NtStatus kStatusNoMoreFiles = static_cast<NtStatus>(0x80000006L);
    constexpr NtStatus kStatusNoSuchFile = static_cast<NtStatus>(0xC000000FL);
    constexpr NtStatus kStatusNotImplemented = static_cast<NtStatus>(0xC0000002L);
    constexpr NtStatus kStatusInvalidInfoClass = static_cast<NtStatus>(0xC0000003L);
    constexpr NtStatus kStatusInvalidParameter = static_cast<NtStatus>(0xC000000DL);
    constexpr NtStatus kStatusNotSupported = static_cast<NtStatus>(0xC00000BBL);

    const NtQueryDirectoryFileFn nt_query_dir = GetNtQueryDirectoryFile();
    std::vector<BYTE> nt_buffer;
    if (nt_query_dir) nt_buffer.resize(64 * 1024);

    std::wstring scratch_child;
    std::wstring long_dir;
    std::wstring pattern;
    std::vector<Frame> pending_subdirs;

    size_t progress_counter = 0;

    auto cancelled = [&]() -> bool {
        if (cb.should_cancel && cb.should_cancel()) {
            stats.cancelled = true;
            return true;
        }
        return false;
    };

    auto push_entry = [&](const Frame& frame, std::wstring_view name,
                          DWORD attrs, uint64_t size, uint64_t mtime_ft) -> bool {
        if (name.empty() || IsDotName(name)) return true;
        if (cb.should_skip_name && cb.should_skip_name(frame.is_root, name)) return true;
        if (stats.added >= max_items) {
            stats.hit_cap = true;
            return false;
        }
        if (frame.path.size() + 1 + name.size() > 32766) return true;

        const bool is_dir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
        const bool is_reparse = (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        bool hidden = frame.hidden_parent;
        bool need_path = is_dir && !is_reparse;
        if (!hidden && frame.check_exclusions && cb.is_excluded_path) {
            scratch_child.assign(frame.path);
            if (!scratch_child.empty() && scratch_child.back() != L'\\') scratch_child.push_back(L'\\');
            scratch_child.append(name);
            hidden = cb.is_excluded_path(scratch_child);
            if (hidden || !need_path) need_path = false;
        }
        const int32_t id = cb.emit_node(frame.parent_id, name, is_dir, hidden,
                                        is_dir ? 0 : size, mtime_ft);
        if (id < 0) return true;
        ++stats.added;
        ++progress_counter;
        if (cb.on_progress && (progress_counter & 0x3FFFu) == 0) {
            cb.on_progress(stats.added);
        }
        if (is_dir && !is_reparse && !hidden) {
            std::wstring sub_path;
            if (!scratch_child.empty() && frame.check_exclusions && cb.is_excluded_path) {
                sub_path = std::move(scratch_child);
            } else {
                sub_path.reserve(frame.path.size() + 1 + name.size());
                sub_path.assign(frame.path);
                if (!sub_path.empty() && sub_path.back() != L'\\') sub_path.push_back(L'\\');
                sub_path.append(name);
            }
            const bool check_sub = cb.is_excluded_path &&
                (!cb.may_contain_exclusion || cb.may_contain_exclusion(sub_path));
            pending_subdirs.push_back(Frame{id, std::move(sub_path), false, false, check_sub});
        }
        scratch_child.clear();
        return stats.added < max_items;
    };

    bool processed_root = false;
    while (!stack.empty()) {
        if (cancelled()) return finish(false);
        Frame frame = std::move(stack.back());
        stack.pop_back();
        const bool is_initial_root = !processed_root;
        processed_root = true;

        pending_subdirs.clear();
        long_dir = ToWin32LongPath(frame.path);

        bool enumerated = false;
        DWORD open_error = ERROR_SUCCESS;

        if (nt_query_dir) {
            HANDLE dir_handle = CreateFileW(
                long_dir.c_str(), FILE_LIST_DIRECTORY | SYNCHRONIZE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (dir_handle == INVALID_HANDLE_VALUE) {
                open_error = GetLastError();
                if (is_initial_root) {
                    stats.root_error = open_error ? open_error : ERROR_OPEN_FAILED;
                    SetLastError(stats.root_error);
                    return finish(false);
                }
                ++stats.skipped_dirs;
                continue;
            }

            bool restart = true;
            bool first_call = true;
            bool fallback_find = false;
            for (;;) {
                if (cancelled()) {
                    CloseHandle(dir_handle);
                    return finish(false);
                }
                NtIoStatusBlock iosb{};
                const NtStatus status = nt_query_dir(
                    dir_handle, nullptr, nullptr, nullptr, &iosb,
                    nt_buffer.data(), static_cast<ULONG>(nt_buffer.size()),
                    kNtFileFullDirectoryInformation, FALSE, nullptr,
                    restart ? TRUE : FALSE);
                restart = false;

                if (status == kStatusNoMoreFiles || status == kStatusNoSuchFile) {
                    enumerated = true;
                    break;
                }
                if (status == kStatusPending) {
                    fallback_find = true;
                    break;
                }
                if (status < 0) {
                    if (first_call &&
                        (status == kStatusNotImplemented ||
                         status == kStatusInvalidInfoClass ||
                         status == kStatusInvalidParameter ||
                         status == kStatusNotSupported)) {
                        fallback_find = true;
                        break;
                    }
                    const DWORD dos_err = NtStatusToWin32(status);
                    if (is_initial_root && first_call) {
                        CloseHandle(dir_handle);
                        stats.root_error = dos_err ? dos_err : ERROR_READ_FAULT;
                        SetLastError(stats.root_error);
                        return finish(false);
                    }
                    enumerated = true;
                    if (!first_call || dos_err == ERROR_ACCESS_DENIED) {
                        ++stats.skipped_dirs;
                    }
                    break;
                }
                first_call = false;

                const size_t total_bytes = static_cast<size_t>(iosb.Information);
                if (total_bytes == 0 || total_bytes > nt_buffer.size()) {
                    enumerated = true;
                    break;
                }

                size_t offset = 0;
                size_t batch_seen = 0;
                bool keep_going = true;
                while (offset + offsetof(NtFileFullDirectoryInfo, FileName) <= total_bytes) {
                    if ((++batch_seen & 0xFFu) == 0 && cancelled()) {
                        CloseHandle(dir_handle);
                        return finish(false);
                    }
                    const auto* info = reinterpret_cast<const NtFileFullDirectoryInfo*>(
                        nt_buffer.data() + offset);
                    const size_t avail = total_bytes - offset - offsetof(NtFileFullDirectoryInfo, FileName);
                    if (info->FileNameLength > avail || (info->FileNameLength % sizeof(WCHAR)) != 0) {
                        break;
                    }
                    const std::wstring_view name(info->FileName, info->FileNameLength / sizeof(WCHAR));
                    const uint64_t size = info->EndOfFile.QuadPart > 0
                        ? static_cast<uint64_t>(info->EndOfFile.QuadPart) : 0;
                    const uint64_t mtime_ft =
                        (static_cast<uint64_t>(info->LastWriteTime.HighPart) << 32) |
                        static_cast<uint64_t>(info->LastWriteTime.LowPart);

                    keep_going = push_entry(frame, name, info->FileAttributes, size, mtime_ft);
                    if (!keep_going || info->NextEntryOffset == 0) break;
                    if (info->NextEntryOffset > total_bytes - offset || info->NextEntryOffset < sizeof(ULONG)) {
                        break;
                    }
                    offset += info->NextEntryOffset;
                }
                if (!keep_going) {
                    enumerated = true;
                    break;
                }
            }
            CloseHandle(dir_handle);
            if (!fallback_find && enumerated) {
                if (stats.hit_cap || stats.added >= max_items) {
                    stats.hit_cap = true;
                    break;
                }
                for (auto it = pending_subdirs.rbegin(); it != pending_subdirs.rend(); ++it) {
                    stack.push_back(std::move(*it));
                }
                continue;
            }
        }

        // FindFirstFileExW fallback.
        pattern = long_dir;
        if (!pattern.empty() && pattern.back() != L'\\') pattern.push_back(L'\\');
        pattern.push_back(L'*');

        WIN32_FIND_DATAW fd{};
        HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd,
                                       FindExSearchNameMatch, nullptr,
                                       FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) {
            open_error = GetLastError();
            if (is_initial_root && open_error != ERROR_FILE_NOT_FOUND && open_error != ERROR_NO_MORE_FILES) {
                stats.root_error = open_error ? open_error : ERROR_OPEN_FAILED;
                SetLastError(stats.root_error);
                return finish(false);
            }
            if (open_error != ERROR_FILE_NOT_FOUND && open_error != ERROR_NO_MORE_FILES) {
                ++stats.skipped_dirs;
            }
            continue;
        }

        size_t batch_seen = 0;
        do {
            if ((++batch_seen & 0xFFu) == 0 && cancelled()) {
                FindClose(find);
                return finish(false);
            }
            const std::wstring_view name(fd.cFileName);
            const uint64_t size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            const uint64_t mtime_ft =
                (static_cast<uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) |
                fd.ftLastWriteTime.dwLowDateTime;
            if (!push_entry(frame, name, fd.dwFileAttributes, size, mtime_ft)) break;
        } while (FindNextFileW(find, &fd));
        FindClose(find);

        if (stats.hit_cap || stats.added >= max_items) {
            stats.hit_cap = true;
            break;
        }
        for (auto it = pending_subdirs.rbegin(); it != pending_subdirs.rend(); ++it) {
            stack.push_back(std::move(*it));
        }
    }

    return finish(!stats.cancelled && !stats.hit_cap);
}

} // namespace pulse::index

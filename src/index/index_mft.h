// index_mft.h — Sequential NTFS $MFT reader (plan.md 阶段 2.5 C).
// Names, parent FRN, directory flag, data size and last-write time from
// STANDARD_INFORMATION + FILE_NAME + unnamed $DATA. Used instead of
// FSCTL_ENUM_USN_DATA so size:/dm: work after an admin MFT build.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <windows.h>
#include <winioctl.h>

namespace pulse::index {

// Additional hard link name of a file: a Win32/POSIX FILE_NAME whose
// (parent, name) differs from MftFile::name. DOS 8.3 aliases are not links.
struct MftLink {
    uint64_t parent = 0;
    std::wstring name;
};

struct MftFile {
    uint64_t frn = 0;
    uint64_t parent = 0;
    uint64_t size = 0;
    uint64_t mtime = 0; // FILETIME as u64
    std::wstring name;
    bool is_dir = false;
    uint8_t name_type = 0; // NTFS FILE_NAME.NameType
    // Other hard links of a file (never set for directories), including names
    // stored in $ATTRIBUTE_LIST extension records.
    std::vector<MftLink> links;
};

// Core enumeration accepts a positional reader so complete/failure semantics
// can be verified without opening a real volume.
bool EnumerateMftRecords(const NTFS_VOLUME_DATA_BUFFER& geometry,
    const std::function<bool(uint64_t, void*, DWORD)>& read,
    std::atomic<bool>* running, const std::function<void(size_t)>& progress,
    const std::function<bool(MftFile&&)>& emit);

enum class MftReadResult { Complete, Stopped, Failed };

// Walk $MFT on an already-opened volume handle (FILE_READ_DATA).
// `emit` returns false to stop. `progress` is invoked roughly every 50k records.
MftReadResult EnumerateMft(HANDLE volume,
                  std::atomic<bool>* running,
                  const std::function<void(size_t)>& progress,
                  const std::function<bool(MftFile&&)>& emit);

} // namespace pulse::index

namespace pulse::index {
MftReadResult EnumerateMftRecordsResult(const NTFS_VOLUME_DATA_BUFFER& geometry,
    const std::function<bool(uint64_t, void*, DWORD)>& read, std::atomic<bool>* running,
    const std::function<void(size_t)>& progress, const std::function<bool(MftFile&&)>& emit);
}

// ai_cleanup_scan.h - Read-only measurements behind the assistant's space
// tools (flow D): where a drive's
// space went, and what in the user's own folders can safely go.
//
// Nothing here changes a file. Folder sizes come from Pulse's index (the
// MFT totals behind 文件夹大小), so measuring a whole drive does not walk it.
// Cleanup candidates come only from the user's Downloads, Temp and Videos;
// system and program folders are never offered.
#pragma once
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace pulse::app::ai {

// The folders the cleanup looks in. PULSE_AI_SPACE_ROOT (tests) replaces
// all of them with subfolders of one fixture folder, so a live test never
// sees the user's files.
struct SpaceRoots {
    std::wstring profile, downloads, desktop, videos, temp, local_appdata, roaming_appdata;
    bool fixture = false;
};
SpaceRoots CurrentSpaceRoots();

enum class DiskPlace : uint8_t { User, Profile, AppData, System, Programs, Hibernate, Pagefile, OtherUsers, Other };

struct DiskTop {
    std::wstring path;
    uint64_t bytes = 0;
    DiskPlace place = DiskPlace::Other;
    bool is_file = false;
    std::vector<std::pair<std::wstring, uint64_t>> parts;   // AppData: its biggest folders
};

struct DiskUsage {
    wchar_t drive = L'C';
    uint64_t total = 0, free = 0;
    uint64_t system = 0, programs = 0, users = 0, other = 0;
    uint64_t profile = 0;                 // the current user's folder (when on this drive)
    std::wstring user_name;               // its leaf (SS)
    uint64_t recycle_bytes = 0, recycle_items = 0;   // the current user's bin on this drive
    bool indexed = false;                 // folder sizes came from the index
    size_t unindexed = 0;                 // top-level folders the index had no size for
    std::vector<DiskTop> top;             // biggest places, at most 5
    uint64_t ms = 0;
};

// False when the drive does not exist or is not a local disk.
bool MeasureDisk(wchar_t drive, DiskUsage& out, const std::atomic<bool>& cancel);

enum class CleanKind : uint8_t { Installer, Extracted, Copy, Temp, OldVideo, Count };

struct CleanItem {
    std::wstring path;
    uint64_t bytes = 0;
    uint64_t files = 1;                   // a Temp folder counts what is inside
    FILETIME mtime{};
    bool dir = false;
    // What the reason names: the installed program (Installer), the
    // extracted folder (Extracted), the original (Copy). Temp / OldVideo:
    // empty, `days` says how long it went untouched.
    std::wstring arg;
    bool verified = false;                // Extracted: the zip's entries match the folder
    uint32_t days = 0;
};

struct CleanCategory {
    CleanKind kind = CleanKind::Installer;
    std::vector<CleanItem> items;
    uint64_t bytes = 0, files = 0;
    bool default_on = true;               // OldVideo: off
};

// Shared with the UI thread while the scan runs.
struct CleanProgress {
    std::atomic<int> stage{0};            // 0 installers, 1 archives, 2 copies, 3 temp, 4 videos
    std::atomic<float> fraction{0.0f};
};

struct CleanupScan {
    std::vector<CleanCategory> cats;      // only kinds with something to clean
    uint64_t ms = 0;
};

// drive: only items on that drive (0 = any).
void FindCleanup(wchar_t drive, CleanupScan& out, CleanProgress& progress, const std::atomic<bool>& cancel);

// The current user's Recycle Bin on one drive.
struct BinState {
    bool available = false;               // local fixed disk with the bin on
    uint64_t max_bytes = 0, used_bytes = 0, items = 0;
};
BinState ReadBin(wchar_t drive);

// Items now in the bin that came from `originals` and were deleted at or
// after `since`: their $R payload and $I record, for a permanent delete of
// just this batch.
struct BinHit {
    std::wstring original, payload, index;
    uint64_t bytes = 0;
};
std::vector<BinHit> FindInBin(const std::vector<std::wstring>& originals, FILETIME since);

// 查看 on a cleanup category: the rows a pulse:ai-list:<id> tab shows (the
// reason in the 类型 column; checked rows open selected). UI thread only.
struct AiListRow {
    std::wstring path, reason;
    uint64_t bytes = 0;
    FILETIME mtime{};
    bool dir = false;
    bool on = true;
    // Duplicates (flow E): rows of one group share `group` (1..254) and its
    // header `group_label`; 0 lists flat.
    uint8_t group = 0;
    std::wstring group_label;
};
// `column` names the 类型 column (empty: 原因).
void PutAiList(uint32_t id, std::wstring title, std::vector<AiListRow> rows, std::wstring column = {});
bool GetAiList(uint32_t id, std::wstring& title, std::vector<AiListRow>& rows);
std::wstring AiListColumn(uint32_t id);

}  // namespace pulse::app::ai

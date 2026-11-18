#pragma once

// find_duplicates (second batch, flow E): files with the same content in the
// folders the user named. Everything is compared on this machine: sizes,
// then the first 64 KB, then the whole file byte by byte. The model only
// sees paths, names and dates; Pulse picks the copy each group keeps.

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace pulse::app::ai {

// A folder to compare. `key` is the known-folder name the model used
// (downloads, pictures, ...) or empty for a path.
struct DupRoot {
    std::wstring path;
    std::wstring key;
};

// Known folder names (downloads, pictures, desktop, documents, videos,
// music; 下载 / 图片 / … too) or absolute folder paths; empty picks
// `current`. PULSE_AI_SPACE_ROOT=<dir> maps the known names to <dir>\Downloads,
// <dir>\Pictures, ... (fixtures). Fails with a short English reason for the
// model: a missing folder, a system or program folder.
bool ResolveDupRoots(const std::vector<std::wstring>& names, const std::wstring& current,
                     std::vector<DupRoot>& out, std::string& error);

// The user's Downloads and Desktop (fixture-aware): files there are not in
// an organised folder.
std::vector<std::wstring> MessyFolders();

struct DupFile {
    std::wstring path;
    uint64_t bytes = 0;
    uint64_t mtime = 0;            // FILETIME ticks (UTC)
    size_t root = 0;               // index into DupScan::roots
    bool organized = false;        // in a subfolder, not right in Downloads / Desktop
    bool clean = true;             // no (1), 副本, - Copy in the name
    bool keep = false;
};

// Why a group keeps what it keeps (the card's last line).
enum class DupWhy : uint8_t { Organized, CleanName, Earliest, Newest, Both, Picked };

struct DupGroup {
    std::vector<DupFile> files;
    DupWhy why = DupWhy::Earliest;
    bool both = false;             // two organised copies in different folders: all stay
    bool picked = false;           // the user chose the keeper; rules leave it alone
    size_t Kept() const;
    size_t Extra() const { return files.size() - Kept(); }
    uint64_t ExtraBytes() const;
};

// Applies the keep rules unless the user picked: organised folder › clean name
// › earliest (or newest). Two organised copies in different folders all stay.
void ApplyKeepRules(DupGroup& g, bool newest);
// The user clicked a copy: it stays, the others go.
void PickKeeper(DupGroup& g, size_t file);

struct DupProgress {
    std::atomic<int> stage{0};                 // 0 sizes, 1 heads, 2 full, 3 done
    std::atomic<uint64_t> files{0};            // files seen (stage 0)
    std::atomic<uint64_t> bytes{0};            // their sizes (the 重复文件 tab's 已看过 … · 3.1 GB)
    std::atomic<uint64_t> done{0}, todo{0};    // stage 1 / 2 work
};

struct DupScan {
    std::vector<DupRoot> roots;
    std::vector<DupGroup> groups;              // biggest waste first
    uint64_t total_files = 0;                  // stage 0: files looked at
    uint64_t size_matches = 0;                 // files sharing a size with another
    uint64_t head_matches = 0;                 // after the first 64 KB
    uint64_t dup_files = 0;                    // in a group
    uint64_t read_bytes = 0;                   // read by the full compare
    uint32_t ms[3]{};                          // per stage
    bool truncated = false;                    // stopped at the file cap
    uint64_t online_skipped = 0;               // cloud placeholders left alone (reading would download them)
};

void FindDuplicates(DupScan& scan, uint64_t min_bytes, DupProgress& progress,
                    const std::atomic<bool>& cancel);

// The file still has this size and time, and the same bytes as `keeper`.
bool DupStillSame(const DupFile& keeper, const DupFile& extra, const std::atomic<bool>& cancel);

// "图片 › 2026 杭州 › " for a file under root `r` (label = the root's name).
std::wstring DupDirLabel(const DupScan& scan, const DupFile& f, const std::wstring& root_label);

}  // namespace pulse::app::ai

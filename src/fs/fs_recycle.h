// fs_recycle.h — Enumerate the per-user Recycle Bin without Shell COM.
#pragma once
#include "fs_enum.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pulse::fs {

struct RecycleBinInfo {
    uint64_t bytes = 0;
    uint64_t items = 0;
    bool valid = false;
};

struct RecycleItem {
    std::wstring name;
    std::wstring original_path;
    std::wstring content_path; // $R* payload
    std::wstring index_path;   // $I* metadata
    uint64_t size = 0;
    FILETIME deleted{};
    bool is_dir = false;
};

bool QueryRecycleBinInfo(RecycleBinInfo& out);
bool ReadRecycleIndex(const std::wstring& index_path, RecycleItem& out);
std::wstring RecycleIndexPath(const std::wstring& content_path);
// Progress hook for a long bin scan: called with the items gathered so far (the vector
// being filled; do not modify it). Returning false stops the scan.
using RecycleProgress = std::function<bool(const std::vector<DirEntry>&)>;
// Append live items belonging to the current user under one $Recycle.Bin root.
// Missing directories are empty; access/enumeration failures return false.
bool EnumerateRecycleBinAtRoot(const std::wstring& recycle_root, std::vector<DirEntry>& out,
                               const RecycleProgress& progress = {});
// List the bin. The whole listing never blocks on SHQueryRecycleBinW (occupancy has its
// own query); info receives the listing-derived totals. Returns false when a progress
// callback asked to stop — a read failure only marks the totals incomplete.
bool EnumerateRecycleBin(std::vector<DirEntry>& out, RecycleBinInfo* info = nullptr,
                         const RecycleProgress& progress = {});

inline bool IsRecycleViewPath(const std::wstring& path) {
    return path == L"pulse:recycle";
}

} // namespace pulse::fs

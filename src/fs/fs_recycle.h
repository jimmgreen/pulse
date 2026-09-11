// fs_recycle.h — Enumerate the per-user Recycle Bin without Shell COM.
#pragma once
#include "fs_enum.h"
#include <cstdint>
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
// Append live items belonging to the current user under one $Recycle.Bin root.
// Missing directories are empty; access/enumeration failures return false.
bool EnumerateRecycleBinAtRoot(const std::wstring& recycle_root, std::vector<DirEntry>& out);
void EnumerateRecycleBin(std::vector<DirEntry>& out, RecycleBinInfo* info = nullptr);

inline bool IsRecycleViewPath(const std::wstring& path) {
    return path == L"pulse:recycle";
}

} // namespace pulse::fs

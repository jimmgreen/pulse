// ai_archive_tools.h - Safe archive inspection, Zip-Slip-guarded extraction
// (via System32 archiveint.dll), and folder comparison for Pulse 助手.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace pulse::app::ai {

struct ArchiveEntryInfo {
    std::wstring path;     // normalized '/' path inside archive
    uint64_t size = 0;
    bool is_dir = false;
    bool encrypted = false;
    int64_t mtime = 0;     // Unix epoch seconds (0 if unknown)
};

struct ArchiveInspectResult {
    bool ok = false;
    std::string format;
    size_t total_entries = 0;
    size_t file_count = 0;
    size_t dir_count = 0;
    uint64_t total_uncompressed = 0;
    bool truncated = false;
    bool has_encrypted = false;
    std::vector<ArchiveEntryInfo> entries;
    std::string error_code;
    std::string error_message;
};

struct ArchiveExtractResult {
    bool ok = false;
    size_t extracted_files = 0;
    size_t extracted_dirs = 0;
    size_t skipped_unsafe = 0;
    uint64_t extracted_bytes = 0;
    std::vector<std::wstring> top_level_created;
    std::string error_code;
    std::string error_message;
};

// Validates and normalizes an archive entry pathname against Zip-Slip path
// traversal, absolute/drive/UNC paths, illegal Win32 chars, and reserved device names.
bool SanitizeArchiveRelativePath(std::wstring_view raw_path, std::wstring& out_rel_win_path);

// Lists entries in ZIP/7Z/RAR/TAR/GZ/XZ/BZ2 without extracting anything to disk.
ArchiveInspectResult InspectArchive(const std::wstring& archive_path, size_t max_entries,
                                    const std::atomic<bool>& cancelled);

// Safely extracts entries into `dest_folder` (never overwrites existing files;
// auto-renames collisions with `(2)`, blocks Zip-Slip, enforces 10k files / 4 GiB caps).
// If `selected_entries` is non-empty, only extracts matching normalized paths.
ArchiveExtractResult ExtractArchive(const std::wstring& archive_path, const std::wstring& dest_folder,
                                    const std::vector<std::wstring>& selected_entries,
                                    const std::atomic<bool>& cancelled);

struct FolderDiffItem {
    std::wstring name;
    bool is_dir = false;
    uint64_t left_size = 0;
    uint64_t right_size = 0;
};

struct FolderCompareSummary {
    bool ok = false;
    std::wstring left_path;
    std::wstring right_path;
    size_t left_total = 0;
    size_t right_total = 0;
    size_t same_count = 0;
    std::vector<FolderDiffItem> only_left;
    std::vector<FolderDiffItem> only_right;
    std::vector<FolderDiffItem> newer_left;
    std::vector<FolderDiffItem> newer_right;
    std::vector<FolderDiffItem> different_size;
    std::string error_code;
    std::string error_message;
};

// Top-level directory comparison matching Pulse's built-in 2-second timestamp tolerance.
FolderCompareSummary CompareFolderPaths(const std::wstring& left_dir, const std::wstring& right_dir,
                                        const std::atomic<bool>& cancelled);

} // namespace pulse::app::ai

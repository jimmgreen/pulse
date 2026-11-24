#pragma once
// In-app Git: merge conflict detection, 2-way/3-way marker parsing, hunk and
// binary resolution (including saving the unchosen copy), complete/abort merge.
// Blocking; call off the UI thread.
#include "git_changes.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::app {

enum class GitConflictChoice : uint8_t { None = 0, Ours, Theirs, Both, Custom };

struct GitConflictHunk {
    std::wstring context_before;
    std::wstring context_after;
    std::wstring ours_label;
    std::wstring theirs_label;
    std::wstring ours_text;
    std::wstring theirs_text;
    std::wstring base_text;
    std::wstring custom_text;
    // Exact UTF-8 payloads for lossless text reconstruction.
    std::string ours_utf8;
    std::string theirs_utf8;
    std::string base_utf8;
    std::string custom_utf8;
    GitConflictChoice choice = GitConflictChoice::None;
};

struct GitConflictFile {
    std::wstring path;
    bool is_binary = false;
    bool is_image = false;
    int hunk_count = 0;
    bool resolved = false;
};

struct GitConflictState {
    bool ok = false;
    bool git_missing = false;
    bool merging = false;
    std::wstring merge_head_short;
    std::wstring ours_author;
    std::wstring ours_time;
    std::wstring theirs_author;
    std::wstring theirs_time;
    std::vector<GitConflictFile> conflict_files;
    std::vector<std::wstring> auto_merged_files;
};

bool GitIsMerging(const std::wstring& root);
GitConflictState GitReadConflictState(const std::wstring& root, DWORD timeout_ms = 20000);

// Pure conflict marker parser and lossless composer (unit tested).
// `segments.size()` is always `hunks.size() + 1`.
bool ParseConflictText(std::string_view content,
                       std::vector<GitConflictHunk>& hunks,
                       std::vector<std::string>& segments);
std::string ComposeResolvedText(const std::vector<std::string>& segments,
                                const std::vector<GitConflictHunk>& hunks);

// Reads stage 1 (common base), 2 (ours), or 3 (theirs) blob of a conflicted path.
bool GitReadConflictStageBlob(const std::wstring& root, int stage, const std::wstring& path,
                              std::string& bytes, uint64_t max_bytes, bool* too_large = nullptr);

// Writes resolved UTF-8 content to the working tree and stages the file.
GitOpResult GitWriteResolvedTextFile(const std::wstring& root, const std::wstring& path,
                                     std::string_view utf8_bytes);

// Resolves a binary/image conflict: writes the chosen side and stages it; when
// `save_other_copy` is true, writes the unchosen side next to it without overwriting.
GitOpResult GitResolveBinaryConflict(const std::wstring& root, const std::wstring& path,
                                     bool pick_ours, bool save_other_copy,
                                     const std::wstring& other_suffix,
                                     std::wstring* saved_copy_path = nullptr);

GitOpResult GitCompleteMerge(const std::wstring& root);
GitOpResult GitAbortMerge(const std::wstring& root);

}  // namespace pulse::app

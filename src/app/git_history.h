#pragma once
// In-app Git: commit history, branch topology graph, commit files, file tree,
// single-file restore and reset operations. Blocking; call off the UI thread.
#include "git_changes.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace pulse::app {

struct GitRefTag {
    enum class Kind : uint8_t { Local, Remote, RemoteNew, Tag };
    std::wstring name;
    Kind kind = Kind::Local;
};

struct GitGraphEdge {
    int to_row = -1;
    int to_lane = 0;
    bool is_merge = false;
    bool is_remote = false;
};

struct GitCommitItem {
    std::wstring full_hash;
    std::wstring short_hash;
    std::vector<std::wstring> parents;
    std::vector<GitRefTag> refs;
    std::wstring author;
    std::wstring email;
    std::wstring time_text;
    std::wstring subject;
    std::wstring body;
    int64_t timestamp = 0;
    bool is_merge = false;
    bool is_remote_only = false;
    bool is_unpushed = false;
    int lane = 0;
    std::vector<GitGraphEdge> edges;
};

struct GitCommitFile {
    std::wstring path;
    std::wstring orig_path;
    wchar_t status_char = L'M';
    int added = 0;
    int deleted = 0;
    bool is_binary = false;
};

struct GitHistoryFilter {
    enum class Scope : uint8_t { All, CurrentPath, Mine };
    Scope scope = Scope::All;
    std::wstring rel_path;
    std::wstring query;
    std::wstring mine_author; // email or name
};

struct GitTreeEntry {
    std::wstring path;
    std::wstring name;
    uint64_t size = 0;
};

// Pure parsers and graph layout (unit tested).
void ParseGitLogOutput(std::string_view out,
                       const std::unordered_set<std::wstring>& unpushed_hashes,
                       const std::unordered_set<std::wstring>& remote_only_hashes,
                       std::vector<GitCommitItem>& items);
void ComputeCommitGraph(std::vector<GitCommitItem>& items);

// Reads commit history with optional scope/query filter.
GitOpResult GitReadHistory(const std::wstring& root, const GitHistoryFilter& filter,
                           int limit, std::vector<GitCommitItem>& items, int& out_behind,
                           DWORD timeout_ms = 20000);

// Reads files touched by `hash`.
GitOpResult GitReadCommitFiles(const std::wstring& root, const std::wstring& hash,
                               std::vector<GitCommitFile>& files, DWORD timeout_ms = 15000);

// Reads a blob at `hash:path`.
bool GitReadCommitFileBlob(const std::wstring& root, const std::wstring& hash,
                           const std::wstring& path, std::string& bytes,
                           uint64_t max_bytes, bool* too_large = nullptr);

// Reads the full file tree at `hash`.
GitOpResult GitReadCommitTree(const std::wstring& root, const std::wstring& hash,
                              std::vector<GitTreeEntry>& entries, DWORD timeout_ms = 20000);

// Restores one file from `hash`; stashes current dirty changes first when `auto_stash`.
GitOpResult GitRestoreFileFromCommit(const std::wstring& root, const std::wstring& hash,
                                     const std::wstring& rel_path, bool auto_stash);

// Resets current branch to `hash` (`--soft` or `--hard`).
GitOpResult GitResetToCommit(const std::wstring& root, const std::wstring& hash, bool hard);

}  // namespace pulse::app

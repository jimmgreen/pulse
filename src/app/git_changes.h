#pragma once
// In-app Git, stage 1 (Source Control window): the working-tree changes of a
// repository split into staged / unstaged / merge conflicts, read from
// `git status --porcelain=v2 -z`, plus the write commands the window runs
// (stage, unstage, discard, commit) and blob reads for the diff view.
//
// Everything that runs git blocks: call it off the UI thread. Paths are
// root-relative with '/' separators, exactly as git prints them.
#include "git_status.h"
#include "../ui/git_mark.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::app {

enum class GitChangeKind : uint8_t {
    Modified, Added, Deleted, Renamed, Copied, TypeChanged, Untracked, Conflict
};

struct GitChange {
    std::wstring path;       // root-relative, '/' separated
    std::wstring orig_path;  // renames and copies: the source path
    GitChangeKind kind = GitChangeKind::Modified;
    bool submodule = false;
};

struct GitChangeSet {
    std::wstring root;
    std::wstring branch;     // empty when HEAD is detached
    std::wstring head;       // short commit id; empty before the first commit
    std::wstring upstream;
    int ahead = 0, behind = 0;
    bool has_upstream_ab = false;
    std::vector<GitChange> staged;     // index vs HEAD
    std::vector<GitChange> unstaged;   // work tree vs index, untracked files last
    std::vector<GitChange> conflicts;  // unmerged paths
    bool HasHead() const noexcept { return !head.empty(); }
    size_t Total() const noexcept { return staged.size() + unstaged.size() + conflicts.size(); }
};

// Parses `git status --porcelain=v2 --branch -z --untracked-files=all`.
// False when the text has no branch header (not status output).
bool ParseGitChanges(std::string_view out, GitChangeSet& set);

// The one-letter code VS Code and `git status -s` use: M A D R C T U (untracked) !
wchar_t GitChangeLetter(GitChangeKind kind) noexcept;
ui::GitMark GitChangeMark(GitChangeKind kind) noexcept;

// CommandLineToArgvW-safe quoting of one argument (always quoted).
std::wstring GitQuoteArg(std::wstring_view arg);
std::string GitWideToUtf8(std::wstring_view text);
// root + '\' + rel with '/' turned into '\'.
std::wstring GitJoinPath(const std::wstring& root, const std::wstring& rel);

struct GitOpResult {
    bool ok = false;
    bool git_missing = false;
    std::wstring message;  // git's own words on failure (last error lines), else empty
};

// Reads the change set (status), blocking. `timeout_ms` bounds git.
GitOpResult GitReadChanges(const std::wstring& root, GitChangeSet& set, DWORD timeout_ms = 30000);

// Stage the work-tree state of `paths` (adds, modifications and deletions).
GitOpResult GitStagePaths(const std::wstring& root, const std::vector<std::wstring>& paths);
// Back out of the index; before the first commit this removes them from it.
GitOpResult GitUnstagePaths(const std::wstring& root, const std::vector<std::wstring>& paths, bool has_head);
// Restore tracked files in the work tree from the index (unstaged changes only).
GitOpResult GitDiscardPaths(const std::wstring& root, const std::vector<std::wstring>& paths);
// Untracked files go to the Recycle Bin (recoverable), never `git clean`.
GitOpResult GitRecycleUntracked(const std::wstring& root, const std::vector<std::wstring>& paths);
// `git commit -F <message file>`; amend with an empty message keeps the old one.
GitOpResult GitCommit(const std::wstring& root, const std::wstring& message, bool amend);
// The last commit's message (for amend); empty when there is none.
std::wstring GitLastCommitMessage(const std::wstring& root);
// Full id of HEAD; empty before the first commit or on failure.
std::wstring GitHeadId(const std::wstring& root);
// Undo a commit made a moment ago, keeping its changes staged (`reset --soft`;
// before it there was no commit: HEAD is unborn again). Refuses when HEAD is
// no longer `expect_head` (something else committed meanwhile).
GitOpResult GitUndoCommit(const std::wstring& root, const std::wstring& expect_head);

enum class GitBlobSource : uint8_t { Head, Index };
// The bytes of `path` at HEAD or in the index. False when it does not exist
// there (new file, deleted from the index) or git failed.
bool GitReadBlob(const std::wstring& root, GitBlobSource source, const std::wstring& path, std::string& bytes,
                 uint64_t max_bytes, bool* too_large = nullptr);

// Writes `bytes` to a fresh file in %TEMP%\Pulse\git (kept until the window
// closes; GitCleanupTempFiles removes the folder's stale files).
std::wstring GitWriteTempFile(std::string_view bytes, const std::wstring& name_hint);
void GitCleanupTempFiles();

// Image types the diff view shows as pictures rather than text.
bool GitIsImagePath(std::wstring_view path) noexcept;

struct GitIdentity {
    std::wstring name;
    std::wstring email;
};
GitIdentity GitReadIdentity(const std::wstring& root);

// Suggests a commit message based on staged files ("更新 A、B 等 N 个文件").
std::wstring GitSuggestCommitMessage(const std::vector<GitChange>& staged, bool plain_terms);

struct GitDiffHunkPatch {
    int old_start = 0;
    int old_count = 0;
    int new_start = 0;
    int new_count = 0;
    std::wstring header;       // the @@ ... @@ line
    std::string patch_bytes;   // complete valid patch for `git apply`
};
std::vector<GitDiffHunkPatch> ParseUnifiedDiffHunks(std::string_view raw_diff);
GitOpResult GitReadFileHunks(const std::wstring& root, const std::wstring& path, bool cached,
                             std::vector<GitDiffHunkPatch>& out, DWORD timeout_ms = 20000);
GitOpResult GitApplyHunkPatch(const std::wstring& root, std::string_view patch_bytes,
                              bool cached, bool reverse, DWORD timeout_ms = 20000);

struct GitBlameLine {
    std::wstring hash;
    std::wstring author;
    int64_t timestamp = 0;
    std::wstring date_text;
    bool uncommitted = false;
};
bool ParseGitBlamePorcelain(std::string_view out, std::vector<GitBlameLine>& lines);
bool GitReadBlame(const std::wstring& root, const std::wstring& path,
                  std::vector<GitBlameLine>& lines, DWORD timeout_ms = 20000);

struct GitFileMiniDiffLine {
    wchar_t sign = L' ';  // L'+', L'-', or L' '
    int line_no = 0;
    std::wstring text;
};
struct GitFileHistoryMini {
    std::wstring hash;
    std::wstring subject;
    std::wstring author;
    std::wstring time_text;
};
struct GitFileSummary {
    bool valid = false;
    bool staged = false;
    bool unstaged = false;
    bool untracked = false;
    bool is_binary = false;
    int added = 0;
    int deleted = 0;
    int first_hunk_line = 0;
    std::vector<GitFileMiniDiffLine> preview_lines;
    std::vector<GitFileHistoryMini> history;
};
GitFileSummary GitReadFileSummary(const std::wstring& root, const std::wstring& rel_path);

// Formats a unix timestamp into compact relative/short date text ("今天 14:20", "11/15").
std::wstring GitFormatShortTime(int64_t unix_seconds);

}  // namespace pulse::app

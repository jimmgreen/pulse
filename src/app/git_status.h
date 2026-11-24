#pragma once
// In-app Git: the working-tree state of a repository (branch, ahead/behind,
// changed paths) read from `git status --porcelain=v2`, and a small runner for
// git commands. Nothing here touches the UI; git_ui.cpp drives it.
#include "../ui/git_mark.h"
#include <windows.h>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace pulse::app {

struct GitRepoStatus {
    std::wstring root;
    std::wstring branch;        // empty when HEAD is detached
    std::wstring head;          // short commit id; empty before the first commit
    std::wstring upstream;      // e.g. origin/main; empty when none is set
    int ahead = 0, behind = 0;
    int changed = 0;            // entries git reports (files and untracked folders)
    // Keys are root-relative, lowercase, backslash-separated. `dirs` holds
    // every folder above a change with its strongest mark; `untracked_dirs`
    // the folders git reports as wholly untracked ("dir/").
    std::unordered_map<std::wstring, ui::GitMark> files;
    std::unordered_map<std::wstring, ui::GitMark> dirs;
    std::unordered_map<std::wstring, int> dir_counts;
    std::unordered_set<std::wstring> untracked_dirs;
};

// Parses `git status --porcelain=v2 --branch -z`. False when the text has no
// branch header (not status output).
bool ParseGitStatusV2(std::string_view out, GitRepoStatus& status);
ui::GitMark StrongerGitMark(ui::GitMark a, ui::GitMark b);
// The mark of a row whose root-relative lowercase path is `rel`.
ui::GitMark GitMarkFor(const GitRepoStatus& status, const std::wstring& rel, bool is_dir);
// Number of changed entries under a folder (or 1 when an untracked folder itself).
int GitDirChangeCount(const GitRepoStatus& status, const std::wstring& rel);
// "main ↑1 ↓2"; a detached HEAD shows its short commit id.
std::wstring GitBranchLabel(const GitRepoStatus& status);
// The sidebar badge: the label with a long branch name shortened.
std::wstring GitBadgeText(const GitRepoStatus& status);
std::wstring GitLowerPath(std::wstring path);

struct GitRunResult {
    bool started = false;       // false: git.exe was not found / could not start
    bool timed_out = false;
    DWORD exit_code = 0;
    std::string out;            // stdout, or stdout+stderr when merged
};
// Runs `git <args>` in `cwd` without a console or prompts (credential helpers
// with their own window still work). Blocks; call it off the UI thread.
GitRunResult RunGit(const std::wstring& cwd, const std::wstring& args, DWORD timeout_ms,
                    bool merge_stderr);
std::wstring GitUtf8ToWide(std::string_view text);

// Latest status per repository root (UI thread only); null before the first
// answer. `revision` bumps on every stored answer.
std::shared_ptr<const GitRepoStatus> GitStatusFor(const std::wstring& root);
void StoreGitStatus(std::shared_ptr<const GitRepoStatus> status);
uint64_t GitStatusRevision();

}  // namespace pulse::app

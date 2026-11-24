#pragma once
// In-app Git: branches, remote sync (fetch / pull / push / sync), and stashes.
// All git invocations block and must run off the UI thread.
#include "git_changes.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::app {

struct GitBranchInfo {
    std::wstring name;       // e.g. "main" or "origin/feature"
    std::wstring short_name; // "feature" for "origin/feature"
    std::wstring upstream;
    std::wstring last_author;
    std::wstring last_date;
    std::wstring subject;
    int64_t committer_time = 0;
    bool is_current = false;
    bool is_remote = false;
};

struct GitStashInfo {
    std::wstring ref;        // e.g. "stash@{0}"
    std::wstring message;
    std::wstring date_text;
    int file_count = 0;
};

struct GitBranchOverview {
    bool ok = false;
    bool git_missing = false;
    std::wstring root;
    std::wstring current_branch; // empty when detached
    std::wstring head;
    std::wstring upstream;
    int ahead = 0;
    int behind = 0;
    int changed_count = 0;
    bool merging = false;
    std::vector<GitBranchInfo> local_branches;
    std::vector<GitBranchInfo> remote_branches; // remote branches without a matching local branch
    std::vector<GitStashInfo> stashes;
};

struct GitRemoteOpResult {
    bool ok = false;
    bool git_missing = false;
    bool has_conflicts = false;
    bool push_rejected_non_ff = false;
    bool pull_divergent = false;
    std::wstring message;
    std::wstring summary;
};

// Pure parsers for unit testing.
void ParseBranchesOutput(std::string_view out,
                         std::vector<GitBranchInfo>& local,
                         std::vector<GitBranchInfo>& remote);
void ParseStashListOutput(std::string_view out, std::vector<GitStashInfo>& stashes);

// Reads branch, sync, and stash overview for `root`.
GitBranchOverview GitReadBranchOverview(const std::wstring& root, DWORD timeout_ms = 20000);

// Branch operations.
GitOpResult GitSwitchBranch(const std::wstring& root, const std::wstring& branch, bool auto_stash);
GitOpResult GitCheckoutRemoteBranch(const std::wstring& root, const std::wstring& remote_ref);
GitOpResult GitCreateBranch(const std::wstring& root, const std::wstring& name,
                            const std::wstring& start_point = {});
GitRemoteOpResult GitMergeBranch(const std::wstring& root, const std::wstring& branch);

// Stash operations.
GitOpResult GitStashPush(const std::wstring& root, const std::wstring& message,
                         bool include_untracked = true);
GitRemoteOpResult GitStashApply(const std::wstring& root, const std::wstring& ref);
GitOpResult GitStashDrop(const std::wstring& root, const std::wstring& ref);

// Remote operations (never --force).
GitRemoteOpResult GitFetch(const std::wstring& root);
GitRemoteOpResult GitPull(const std::wstring& root);
GitRemoteOpResult GitPush(const std::wstring& root, bool set_upstream, const std::wstring& branch);
GitRemoteOpResult GitSync(const std::wstring& root, const std::wstring& branch, bool set_upstream);

}  // namespace pulse::app

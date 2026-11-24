#pragma once
#include "git_status.h"
#include "../fs/fs_enum.h"
#include <functional>
#include <vector>

namespace pulse::app {
inline std::wstring GitRowPlainPath(const std::wstring& path) {
    if (path.starts_with(L"\\\\?\\UNC\\")) return L"\\\\" + path.substr(8);
    if (path.starts_with(L"\\\\?\\")) return path.substr(4);
    return path;
}

// Immutable frame inputs: lookup cost follows painted/hit-tested rows, not
// directory size. Keeping both owners also makes older frames self-contained.
inline std::function<ui::GitMark(int)> MakeGitRowLookup(
    std::shared_ptr<const std::vector<fs::DirEntry>> listing,
    std::shared_ptr<const GitRepoStatus> status, std::wstring folder, std::wstring root) {
    folder = GitLowerPath(GitRowPlainPath(folder));
    root = GitLowerPath(GitRowPlainPath(root));
    if (!listing || !status || root.empty() || folder.size() < root.size() ||
        folder.compare(0, root.size(), root) != 0 ||
        (folder.size() > root.size() && folder[root.size()] != L'\\')) return {};
    std::wstring prefix = folder.substr(root.size());
    while (!prefix.empty() && prefix.front() == L'\\') prefix.erase(prefix.begin());
    if (!prefix.empty()) prefix += L'\\';
    return [listing = std::move(listing), status = std::move(status),
            prefix = std::move(prefix), root = std::move(root)](int source) {
        if (source < 0 || static_cast<size_t>(source) >= listing->size()) return ui::GitMark::None;
        const auto& entry = (*listing)[static_cast<size_t>(source)];
        if (entry.name == L".git" || entry.change_record_only) return ui::GitMark::None;
        std::wstring rel;
        if (!entry.full_path.empty()) {
            const auto full = GitLowerPath(GitRowPlainPath(entry.full_path));
            if (full.size() <= root.size() || full.compare(0, root.size(), root) != 0 ||
                full[root.size()] != L'\\') return ui::GitMark::None;
            rel = full.substr(root.size() + 1);
        } else {
            rel = prefix + GitLowerPath(entry.name);
        }
        return GitMarkFor(*status, rel, entry.is_dir);
    };
}

inline std::function<int(int)> MakeGitDirCountLookup(
    std::shared_ptr<const std::vector<fs::DirEntry>> listing,
    std::shared_ptr<const GitRepoStatus> status, std::wstring folder, std::wstring root) {
    folder = GitLowerPath(GitRowPlainPath(folder));
    root = GitLowerPath(GitRowPlainPath(root));
    if (!listing || !status || root.empty() || folder.size() < root.size() ||
        folder.compare(0, root.size(), root) != 0 ||
        (folder.size() > root.size() && folder[root.size()] != L'\\')) return {};
    std::wstring prefix = folder.substr(root.size());
    while (!prefix.empty() && prefix.front() == L'\\') prefix.erase(prefix.begin());
    if (!prefix.empty()) prefix += L'\\';
    return [listing = std::move(listing), status = std::move(status),
            prefix = std::move(prefix), root = std::move(root)](int source) {
        if (source < 0 || static_cast<size_t>(source) >= listing->size()) return 0;
        const auto& entry = (*listing)[static_cast<size_t>(source)];
        if (!entry.is_dir || entry.name == L".git" || entry.change_record_only) return 0;
        std::wstring rel;
        if (!entry.full_path.empty()) {
            const auto full = GitLowerPath(GitRowPlainPath(entry.full_path));
            if (full.size() <= root.size() || full.compare(0, root.size(), root) != 0 ||
                full[root.size()] != L'\\') return 0;
            rel = full.substr(root.size() + 1);
        } else {
            rel = prefix + GitLowerPath(entry.name);
        }
        return GitDirChangeCount(*status, rel);
    };
}
} // namespace pulse::app

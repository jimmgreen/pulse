#include "git_branches.h"

#include "../common/localization.h"
#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <unordered_set>

namespace pulse::app {
namespace {

std::vector<std::string_view> SplitFields(std::string_view line, char sep) {
    std::vector<std::string_view> parts;
    size_t pos = 0;
    for (;;) {
        const size_t next = line.find(sep, pos);
        if (next == std::string_view::npos) {
            parts.push_back(line.substr(pos));
            break;
        }
        parts.push_back(line.substr(pos, next - pos));
        pos = next + 1;
    }
    return parts;
}

std::wstring TrimWide(std::string_view sv) {
    std::wstring w = GitUtf8ToWide(sv);
    while (!w.empty() && std::iswspace(w.back())) w.pop_back();
    size_t lead = 0;
    while (lead < w.size() && std::iswspace(w[lead])) ++lead;
    if (lead > 0) w.erase(0, lead);
    return w;
}

std::wstring ExtractFailure(const std::string& out) {
    const std::wstring text = GitUtf8ToWide(out);
    std::vector<std::wstring> lines, picked;
    size_t at = 0;
    while (at <= text.size()) {
        size_t end = text.find_first_of(L"\r\n", at);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(at, end - at);
        while (!line.empty() && std::iswspace(line.back())) line.pop_back();
        if (!line.empty() && !line.starts_with(L"hint:")) lines.push_back(std::move(line));
        at = end + 1;
    }
    for (const auto& line : lines) {
        if (line.starts_with(L"! ") || line.starts_with(L"fatal:") || line.starts_with(L"error:") ||
            line.starts_with(L"CONFLICT"))
            picked.push_back(line);
    }
    if (picked.empty()) picked = lines;
    if (picked.size() > 3) picked.erase(picked.begin(), picked.end() - 3);
    std::wstring joined;
    for (const auto& line : picked) {
        if (!joined.empty()) joined += L"\n";
        joined += line;
    }
    if (joined.size() > 360) joined = joined.substr(0, 359) + L"\u2026";
    return joined;
}

std::wstring ExtractSummary(const std::string& out) {
    const std::wstring text = GitUtf8ToWide(out);
    std::vector<std::wstring> lines;
    size_t at = 0;
    while (at <= text.size()) {
        size_t end = text.find_first_of(L"\r\n", at);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(at, end - at);
        while (!line.empty() && std::iswspace(line.back())) line.pop_back();
        if (!line.empty() && !line.starts_with(L"hint:")) lines.push_back(std::move(line));
        at = end + 1;
    }
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        if (it->find(L"changed") != std::wstring::npos || it->find(L"up to date") != std::wstring::npos ||
            it->find(L"->") != std::wstring::npos)
            return *it;
    }
    return lines.empty() ? std::wstring() : lines.back();
}

GitOpResult OpFromRun(const GitRunResult& run) {
    GitOpResult res;
    if (!run.started) {
        res.git_missing = true;
        return res;
    }
    if (run.timed_out) {
        res.message = l10n::Get(l10n::StringId::GitTimedOut);
        return res;
    }
    res.ok = run.exit_code == 0;
    if (!res.ok) res.message = ExtractFailure(run.out);
    return res;
}

bool HasWorkingConflicts(const std::wstring& root) {
    GitChangeSet cs;
    if (!GitReadChanges(root, cs, 10000).ok) return false;
    return !cs.conflicts.empty();
}

bool HasOriginRemote(const std::wstring& root) {
    const GitRunResult remotes = RunGit(root, L"remote", 10000, false);
    if (!remotes.started || remotes.timed_out || remotes.exit_code != 0) return false;
    return remotes.out == "origin\n" || remotes.out == "origin\r\n" ||
           remotes.out.starts_with("origin\n") || remotes.out.starts_with("origin\r\n") ||
           remotes.out.find("\norigin\n") != std::string::npos ||
           remotes.out.find("\norigin\r\n") != std::string::npos;
}

bool IsMergingRepo(const std::wstring& root) {
    const GitRunResult run = RunGit(root, L"rev-parse -q --verify MERGE_HEAD", 5000, false);
    return run.started && !run.timed_out && run.exit_code == 0 && !run.out.empty();
}

}  // namespace

void ParseBranchesOutput(std::string_view out,
                         std::vector<GitBranchInfo>& local,
                         std::vector<GitBranchInfo>& remote) {
    local.clear();
    remote.clear();
    std::unordered_set<std::wstring> local_names;
    std::unordered_set<std::wstring> local_upstreams;
    std::vector<GitBranchInfo> raw_remote;

    size_t pos = 0;
    while (pos < out.size()) {
        size_t nl = out.find('\n', pos);
        const size_t end = (nl == std::string_view::npos) ? out.size() : nl;
        std::string_view line = out.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        pos = (nl == std::string_view::npos) ? out.size() : nl + 1;
        if (line.empty()) continue;

        const auto cols = SplitFields(line, '\x1f');
        if (cols.size() < 7) continue;
        const std::wstring head_mark = TrimWide(cols[0]);
        const std::string_view full_ref = cols[1];
        const std::wstring short_ref = TrimWide(cols[2]);
        const std::wstring upstream = TrimWide(cols[3]);
        const std::wstring author = TrimWide(cols[4]);
        const int64_t ts = _strtoi64(std::string(cols[5]).c_str(), nullptr, 10);
        const std::wstring subject = GitUtf8ToWide(cols[6]);

        if (full_ref.starts_with("refs/heads/")) {
            GitBranchInfo b;
            b.name = short_ref;
            b.short_name = short_ref;
            b.upstream = upstream;
            b.last_author = author;
            b.committer_time = ts;
            b.last_date = GitFormatShortTime(ts);
            b.subject = subject;
            b.is_current = (head_mark == L"*");
            b.is_remote = false;
            local_names.insert(b.name);
            if (!b.upstream.empty()) local_upstreams.insert(b.upstream);
            local.push_back(std::move(b));
        } else if (full_ref.starts_with("refs/remotes/")) {
            if (short_ref == L"origin/HEAD" || short_ref.ends_with(L"/HEAD")) continue;
            GitBranchInfo b;
            b.name = short_ref;
            const size_t slash = short_ref.find(L'/');
            b.short_name = (slash == std::wstring::npos) ? short_ref : short_ref.substr(slash + 1);
            b.last_author = author;
            b.committer_time = ts;
            b.last_date = GitFormatShortTime(ts);
            b.subject = subject;
            b.is_remote = true;
            raw_remote.push_back(std::move(b));
        }
    }

    // Current branch goes first in local_branches.
    std::stable_sort(local.begin(), local.end(), [](const GitBranchInfo& a, const GitBranchInfo& b) {
        return a.is_current > b.is_current;
    });

    for (auto& rb : raw_remote) {
        if (local_upstreams.contains(rb.name) || local_names.contains(rb.short_name)) continue;
        remote.push_back(std::move(rb));
    }
}

void ParseStashListOutput(std::string_view out, std::vector<GitStashInfo>& stashes) {
    stashes.clear();
    constexpr std::string_view kMarker = "__PULSE_STASH__";
    size_t pos = 0;
    while (pos < out.size()) {
        size_t nl = out.find('\n', pos);
        const size_t end = (nl == std::string_view::npos) ? out.size() : nl;
        std::string_view line = out.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        pos = (nl == std::string_view::npos) ? out.size() : nl + 1;
        if (line.empty()) continue;

        if (line.starts_with(kMarker)) {
            const auto cols = SplitFields(line.substr(kMarker.size()), '\x1f');
            if (cols.size() >= 3) {
                GitStashInfo st;
                st.ref = TrimWide(cols[0]);
                const int64_t ts = _strtoi64(std::string(cols[1]).c_str(), nullptr, 10);
                st.date_text = GitFormatShortTime(ts);
                st.message = GitUtf8ToWide(cols[2]);
                stashes.push_back(std::move(st));
            }
        } else if (!stashes.empty()) {
            ++stashes.back().file_count;
        }
    }
}

GitBranchOverview GitReadBranchOverview(const std::wstring& root, DWORD timeout_ms) {
    GitBranchOverview ov;
    ov.root = root;
    GitChangeSet cs;
    const GitOpResult st_res = GitReadChanges(root, cs, timeout_ms);
    if (st_res.git_missing) {
        ov.git_missing = true;
        return ov;
    }
    if (!st_res.ok) return ov;

    ov.ok = true;
    ov.current_branch = cs.branch;
    ov.head = cs.head;
    ov.upstream = cs.upstream;
    ov.ahead = cs.ahead;
    ov.behind = cs.behind;
    ov.changed_count = static_cast<int>(cs.Total());
    ov.merging = !cs.conflicts.empty() || IsMergingRepo(root);

    const GitRunResult br = RunGit(
        root,
        L"-c core.quotepath=false for-each-ref --sort=-committerdate "
        L"--format=%(HEAD)%x1f%(refname)%x1f%(refname:short)%x1f%(upstream:short)%x1f%(authorname)%x1f%(committerdate:unix)%x1f%(subject) "
        L"refs/heads refs/remotes",
        timeout_ms, false);
    if (br.started && !br.timed_out && br.exit_code == 0) {
        ParseBranchesOutput(br.out, ov.local_branches, ov.remote_branches);
    }

    const GitRunResult sl = RunGit(
        root,
        L"-c core.quotepath=false stash list --format=__PULSE_STASH__%gd%x1f%at%x1f%gs --name-only",
        timeout_ms, false);
    if (sl.started && !sl.timed_out && sl.exit_code == 0) {
        ParseStashListOutput(sl.out, ov.stashes);
        for (auto& s : ov.stashes) {
            if (s.file_count == 0) {
                const GitRunResult show = RunGit(
                    root, L"-c core.quotepath=false stash show --include-untracked --name-only " + GitQuoteArg(s.ref),
                    5000, false);
                if (show.started && !show.timed_out && show.exit_code == 0) {
                    int cnt = 0;
                    size_t p = 0;
                    while (p < show.out.size()) {
                        size_t nl = show.out.find('\n', p);
                        size_t e = (nl == std::string::npos) ? show.out.size() : nl;
                        if (e > p) ++cnt;
                        p = (nl == std::string::npos) ? show.out.size() : nl + 1;
                    }
                    s.file_count = cnt;
                }
            }
        }
    }
    return ov;
}

GitOpResult GitSwitchBranch(const std::wstring& root, const std::wstring& branch, bool auto_stash) {
    if (auto_stash) {
        GitChangeSet cs;
        if (GitReadChanges(root, cs, 10000).ok && cs.Total() > 0) {
            const GitOpResult st = GitStashPush(root, L"Pulse: before switching to " + branch, true);
            if (!st.ok) return st;
        }
    }
    return OpFromRun(RunGit(root, L"switch " + GitQuoteArg(branch), 30000, true));
}

GitOpResult GitCheckoutRemoteBranch(const std::wstring& root, const std::wstring& remote_ref) {
    return OpFromRun(RunGit(root, L"switch --track " + GitQuoteArg(remote_ref), 30000, true));
}

GitOpResult GitCreateBranch(const std::wstring& root, const std::wstring& name, const std::wstring& start_point) {
    std::wstring args = L"switch -c " + GitQuoteArg(name);
    if (!start_point.empty()) args += L" " + GitQuoteArg(start_point);
    return OpFromRun(RunGit(root, args, 30000, true));
}

GitRemoteOpResult GitMergeBranch(const std::wstring& root, const std::wstring& branch) {
    GitRemoteOpResult res;
    const GitRunResult run = RunGit(root, L"merge " + GitQuoteArg(branch), 60000, true);
    if (!run.started) {
        res.git_missing = true;
        return res;
    }
    if (run.timed_out) {
        res.message = l10n::Get(l10n::StringId::GitTimedOut);
        return res;
    }
    if (run.exit_code == 0) {
        res.ok = true;
        res.summary = ExtractSummary(run.out);
        return res;
    }
    res.has_conflicts = (run.out.find("CONFLICT") != std::string::npos) || HasWorkingConflicts(root);
    res.message = ExtractFailure(run.out);
    return res;
}

GitOpResult GitStashPush(const std::wstring& root, const std::wstring& message, bool include_untracked) {
    std::wstring args = L"stash push ";
    if (include_untracked) args += L"-u ";
    if (!message.empty()) args += L"-m " + GitQuoteArg(message);
    return OpFromRun(RunGit(root, args, 30000, true));
}

GitRemoteOpResult GitStashApply(const std::wstring& root, const std::wstring& ref) {
    GitRemoteOpResult res;
    const GitRunResult run = RunGit(root, L"stash apply " + GitQuoteArg(ref), 30000, true);
    if (!run.started) {
        res.git_missing = true;
        return res;
    }
    if (run.timed_out) {
        res.message = l10n::Get(l10n::StringId::GitTimedOut);
        return res;
    }
    if (run.exit_code == 0) {
        res.ok = true;
        res.summary = ExtractSummary(run.out);
        return res;
    }
    res.has_conflicts = (run.out.find("CONFLICT") != std::string::npos) || HasWorkingConflicts(root);
    res.message = ExtractFailure(run.out);
    return res;
}

GitOpResult GitStashDrop(const std::wstring& root, const std::wstring& ref) {
    return OpFromRun(RunGit(root, L"stash drop " + GitQuoteArg(ref), 15000, true));
}

GitRemoteOpResult GitFetch(const std::wstring& root) {
    GitRemoteOpResult res;
    const GitRunResult run = RunGit(root, L"fetch --prune", 120000, true);
    if (!run.started) {
        res.git_missing = true;
        return res;
    }
    if (run.timed_out) {
        res.message = l10n::Get(l10n::StringId::GitTimedOut);
        return res;
    }
    res.ok = run.exit_code == 0;
    if (res.ok) res.summary = ExtractSummary(run.out);
    else res.message = ExtractFailure(run.out);
    return res;
}

GitRemoteOpResult GitPull(const std::wstring& root) {
    GitRemoteOpResult res;
    const GitRunResult run = RunGit(root, L"pull", 5 * 60 * 1000, true);
    if (!run.started) {
        res.git_missing = true;
        return res;
    }
    if (run.timed_out) {
        res.message = l10n::Get(l10n::StringId::GitTimedOut);
        return res;
    }
    if (run.exit_code == 0) {
        res.ok = true;
        res.summary = ExtractSummary(run.out);
        return res;
    }
    res.has_conflicts = (run.out.find("CONFLICT") != std::string::npos) || HasWorkingConflicts(root);
    if (run.out.find("divergent branches") != std::string::npos) {
        res.pull_divergent = true;
        res.message = l10n::Get(l10n::StringId::GitPullDivergent);
    } else {
        res.message = ExtractFailure(run.out);
    }
    return res;
}

GitRemoteOpResult GitPush(const std::wstring& root, bool set_upstream, const std::wstring& branch) {
    GitRemoteOpResult res;
    std::wstring args = L"push";
    if (set_upstream && !branch.empty() && HasOriginRemote(root)) {
        args = L"push -u origin " + GitQuoteArg(branch);
    }
    const GitRunResult run = RunGit(root, args, 5 * 60 * 1000, true);
    if (!run.started) {
        res.git_missing = true;
        return res;
    }
    if (run.timed_out) {
        res.message = l10n::Get(l10n::StringId::GitTimedOut);
        return res;
    }
    if (run.exit_code == 0) {
        res.ok = true;
        res.summary = ExtractSummary(run.out);
        return res;
    }
    if (run.out.find("[rejected]") != std::string::npos &&
        (run.out.find("fetch first") != std::string::npos || run.out.find("non-fast-forward") != std::string::npos)) {
        res.push_rejected_non_ff = true;
        res.message = l10n::Get(l10n::StringId::GitPushRejected);
    } else {
        res.message = ExtractFailure(run.out);
    }
    return res;
}

GitRemoteOpResult GitSync(const std::wstring& root, const std::wstring& branch, bool set_upstream) {
    GitChangeSet before;
    const bool has_st = GitReadChanges(root, before, 10000).ok;
    const bool can_pull = !has_st || !before.upstream.empty();
    if (can_pull) {
        GitRemoteOpResult pull = GitPull(root);
        if (!pull.ok) return pull;
    }
    GitChangeSet after;
    if (GitReadChanges(root, after, 10000).ok) {
        if (after.ahead > 0 || (set_upstream && after.upstream.empty() && !branch.empty())) {
            return GitPush(root, set_upstream && after.upstream.empty(), branch);
        }
        GitRemoteOpResult ok_res;
        ok_res.ok = true;
        return ok_res;
    }
    return GitPush(root, set_upstream, branch);
}

}  // namespace pulse::app

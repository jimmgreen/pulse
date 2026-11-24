#include "git_history.h"
#include "git_branches.h"

#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <unordered_map>

namespace pulse::app {
namespace {

std::wstring TrimStr(std::wstring s) {
    while (!s.empty() && std::iswspace(s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::iswspace(s[i])) ++i;
    if (i > 0) s.erase(0, i);
    return s;
}

std::wstring LowerWide(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

bool ContainsInsensitive(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty()) return true;
    return LowerWide(haystack).find(LowerWide(needle)) != std::wstring::npos;
}

std::vector<std::wstring> SplitSpaces(std::wstring_view sv) {
    std::vector<std::wstring> out;
    size_t i = 0;
    while (i < sv.size()) {
        while (i < sv.size() && std::iswspace(sv[i])) ++i;
        if (i >= sv.size()) break;
        size_t j = i;
        while (j < sv.size() && !std::iswspace(sv[j])) ++j;
        out.emplace_back(sv.substr(i, j - i));
        i = j;
    }
    return out;
}

void ParseDecorations(std::wstring_view dec, bool remote_only, std::vector<GitRefTag>& refs) {
    size_t pos = 0;
    while (pos < dec.size()) {
        const size_t comma = dec.find(L',', pos);
        std::wstring item = TrimStr(std::wstring(comma == std::wstring_view::npos ? dec.substr(pos)
                                                                                 : dec.substr(pos, comma - pos)));
        pos = (comma == std::wstring_view::npos) ? dec.size() : comma + 1;
        if (item.empty()) continue;
        if (const size_t arrow = item.find(L"-> "); arrow != std::wstring::npos) {
            item = TrimStr(item.substr(arrow + 3));
        }
        if (item.starts_with(L"tag: ")) {
            std::wstring name = TrimStr(item.substr(5));
            if (name.starts_with(L"refs/tags/")) name.erase(0, 10);
            refs.push_back({std::move(name), GitRefTag::Kind::Tag});
        } else if (item.starts_with(L"refs/heads/")) {
            refs.push_back({item.substr(11), GitRefTag::Kind::Local});
        } else if (item.starts_with(L"refs/remotes/")) {
            std::wstring name = item.substr(13);
            if (name.ends_with(L"/HEAD")) continue;
            refs.push_back({std::move(name), remote_only ? GitRefTag::Kind::RemoteNew : GitRefTag::Kind::Remote});
        } else if (item != L"HEAD") {
            if (!item.ends_with(L"/HEAD")) refs.push_back({std::move(item), GitRefTag::Kind::Local});
        }
    }
}

std::unordered_set<std::wstring> ReadRevListHashes(const std::wstring& root, const std::wstring& range) {
    std::unordered_set<std::wstring> set;
    const GitRunResult run = RunGit(root, L"rev-list " + range, 10000, false);
    if (!run.started || run.timed_out || run.exit_code != 0) return set;
    size_t pos = 0;
    while (pos < run.out.size()) {
        size_t nl = run.out.find('\n', pos);
        size_t end = (nl == std::string::npos) ? run.out.size() : nl;
        std::string_view line(run.out.data() + pos, end - pos);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        if (!line.empty()) set.insert(GitUtf8ToWide(line));
        pos = (nl == std::string::npos) ? run.out.size() : nl + 1;
    }
    return set;
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
    if (!res.ok) {
        res.message = TrimStr(GitUtf8ToWide(run.out));
        if (res.message.size() > 360) res.message = res.message.substr(0, 359) + L"\u2026";
    }
    return res;
}

}  // namespace

void ParseGitLogOutput(std::string_view out,
                       const std::unordered_set<std::wstring>& unpushed_hashes,
                       const std::unordered_set<std::wstring>& remote_only_hashes,
                       std::vector<GitCommitItem>& items) {
    items.clear();
    size_t pos = 0;
    while (pos < out.size()) {
        const size_t start = out.find('\x1e', pos);
        if (start == std::string_view::npos) break;
        const size_t next = out.find('\x1e', start + 1);
        const std::string_view rec = (next == std::string_view::npos)
                                         ? out.substr(start + 1)
                                         : out.substr(start + 1, next - (start + 1));
        pos = (next == std::string_view::npos) ? out.size() : next;

        std::vector<std::string_view> f;
        size_t fp = 0;
        for (int k = 0; k < 7; ++k) {
            const size_t sep = rec.find('\x1f', fp);
            if (sep == std::string_view::npos) break;
            f.push_back(rec.substr(fp, sep - fp));
            fp = sep + 1;
        }
        f.push_back(fp <= rec.size() ? rec.substr(fp) : std::string_view{});
        if (f.size() < 7) continue;

        GitCommitItem item;
        item.full_hash = TrimStr(GitUtf8ToWide(f[0]));
        if (item.full_hash.empty()) continue;
        item.short_hash = item.full_hash.substr(0, (std::min<size_t>)(7, item.full_hash.size()));
        item.parents = SplitSpaces(GitUtf8ToWide(f[1]));
        item.is_merge = item.parents.size() > 1;
        item.is_remote_only = remote_only_hashes.contains(item.full_hash);
        item.is_unpushed = unpushed_hashes.contains(item.full_hash);
        ParseDecorations(GitUtf8ToWide(f[2]), item.is_remote_only, item.refs);
        item.author = GitUtf8ToWide(f[3]);
        item.email = GitUtf8ToWide(f[4]);
        item.timestamp = _strtoi64(std::string(f[5]).c_str(), nullptr, 10);
        item.time_text = GitFormatShortTime(item.timestamp);
        item.subject = TrimStr(GitUtf8ToWide(f[6]));
        if (f.size() >= 8) item.body = TrimStr(GitUtf8ToWide(f[7]));
        items.push_back(std::move(item));
    }
}

void ComputeCommitGraph(std::vector<GitCommitItem>& items) {
    std::unordered_map<std::wstring, int> row_by_hash;
    row_by_hash.reserve(items.size());
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        items[static_cast<size_t>(i)].edges.clear();
        row_by_hash[items[static_cast<size_t>(i)].full_hash] = i;
    }

    std::vector<std::wstring> lanes;
    for (auto& item : items) {
        int my_lane = -1;
        for (int i = 0; i < static_cast<int>(lanes.size()); ++i) {
            if (lanes[static_cast<size_t>(i)] == item.full_hash) {
                if (my_lane < 0) my_lane = i;
                else lanes[static_cast<size_t>(i)].clear();
            }
        }
        if (my_lane < 0) {
            for (int i = 0; i < static_cast<int>(lanes.size()); ++i) {
                if (lanes[static_cast<size_t>(i)].empty()) {
                    my_lane = i;
                    break;
                }
            }
        }
        if (my_lane < 0) {
            my_lane = static_cast<int>(lanes.size());
            lanes.emplace_back();
        }
        item.lane = my_lane;

        if (item.parents.empty()) {
            lanes[static_cast<size_t>(my_lane)].clear();
        } else {
            auto has_lane = [&](const std::wstring& h) {
                return std::find(lanes.begin(), lanes.end(), h) != lanes.end();
            };
            if (!has_lane(item.parents[0])) {
                lanes[static_cast<size_t>(my_lane)] = item.parents[0];
            } else {
                lanes[static_cast<size_t>(my_lane)].clear();
            }
            for (size_t p = 1; p < item.parents.size(); ++p) {
                if (!has_lane(item.parents[p])) {
                    int free_slot = -1;
                    for (int i = 0; i < static_cast<int>(lanes.size()); ++i) {
                        if (lanes[static_cast<size_t>(i)].empty()) {
                            free_slot = i;
                            break;
                        }
                    }
                    if (free_slot >= 0) lanes[static_cast<size_t>(free_slot)] = item.parents[p];
                    else lanes.push_back(item.parents[p]);
                }
            }
        }
    }

    for (auto& item : items) {
        for (size_t p = 0; p < item.parents.size(); ++p) {
            const auto it = row_by_hash.find(item.parents[p]);
            if (it == row_by_hash.end()) continue;
            const auto& parent_item = items[static_cast<size_t>(it->second)];
            GitGraphEdge edge;
            edge.to_row = it->second;
            edge.to_lane = parent_item.lane;
            edge.is_merge = (p > 0);
            edge.is_remote = item.is_remote_only || parent_item.is_remote_only;
            item.edges.push_back(edge);
        }
    }
}

GitOpResult GitReadHistory(const std::wstring& root, const GitHistoryFilter& filter,
                           int limit, std::vector<GitCommitItem>& items, int& out_behind,
                           DWORD timeout_ms) {
    items.clear();
    out_behind = 0;
    GitChangeSet cs;
    const GitOpResult st = GitReadChanges(root, cs, 10000);
    if (st.git_missing) return st;

    std::unordered_set<std::wstring> unpushed, remote_only;
    if (st.ok && !cs.upstream.empty()) {
        unpushed = ReadRevListHashes(root, L"@{u}..HEAD");
        remote_only = ReadRevListHashes(root, L"HEAD..@{u}");
        out_behind = cs.behind;
    }

    const int capped = std::clamp(limit, 20, 500);
    std::wstring args = L"-c core.quotepath=false log --topo-order --decorate=full -n " +
                        std::to_wstring(capped) +
                        L" --format=%x1e%H%x1f%P%x1f%D%x1f%an%x1f%ae%x1f%at%x1f%s%x1f%b ";

    if (filter.scope == GitHistoryFilter::Scope::All) {
        args += L"--branches --remotes ";
    } else if (filter.scope == GitHistoryFilter::Scope::Mine) {
        args += L"--branches --remotes ";
        if (!filter.mine_author.empty()) args += L"--author=" + GitQuoteArg(filter.mine_author) + L" ";
    }
    if (filter.scope == GitHistoryFilter::Scope::CurrentPath && !filter.rel_path.empty()) {
        std::wstring p = filter.rel_path;
        std::replace(p.begin(), p.end(), L'\\', L'/');
        args += L"-- " + GitQuoteArg(p);
    }

    const GitRunResult run = RunGit(root, args, timeout_ms, true);
    if (!run.started) {
        GitOpResult r;
        r.git_missing = true;
        return r;
    }
    if (run.timed_out) {
        GitOpResult r;
        r.message = l10n::Get(l10n::StringId::GitTimedOut);
        return r;
    }
    // An empty repository before the first commit exits non-zero ("does not have any commits yet").
    if (run.exit_code != 0) {
        if (!cs.HasHead()) {
            GitOpResult ok;
            ok.ok = true;
            return ok;
        }
        return OpFromRun(run);
    }

    std::vector<GitCommitItem> parsed;
    ParseGitLogOutput(run.out, unpushed, remote_only, parsed);

    if (!filter.query.empty()) {
        for (auto& item : parsed) {
            if (ContainsInsensitive(item.subject, filter.query) ||
                ContainsInsensitive(item.body, filter.query) ||
                ContainsInsensitive(item.author, filter.query) ||
                ContainsInsensitive(item.short_hash, filter.query) ||
                ContainsInsensitive(item.full_hash, filter.query)) {
                items.push_back(std::move(item));
            }
        }
    } else {
        items = std::move(parsed);
    }
    ComputeCommitGraph(items);
    GitOpResult ok;
    ok.ok = true;
    return ok;
}

GitOpResult GitReadCommitFiles(const std::wstring& root, const std::wstring& hash,
                               std::vector<GitCommitFile>& files, DWORD timeout_ms) {
    files.clear();
    const std::wstring qh = GitQuoteArg(hash);
    const GitRunResult ns_run = RunGit(
        root, L"-c core.quotepath=false diff-tree --no-commit-id --name-status -r -z -m --first-parent --root " + qh,
        timeout_ms, true);
    GitOpResult res = OpFromRun(ns_run);
    if (!res.ok) return res;

    const GitRunResult num_run = RunGit(
        root, L"-c core.quotepath=false diff-tree --no-commit-id --numstat -r -z -m --first-parent --root " + qh,
        timeout_ms, false);

    struct Stat { int a = 0, d = 0; bool bin = false; };
    std::unordered_map<std::wstring, Stat> stats;
    if (num_run.started && !num_run.timed_out && num_run.exit_code == 0) {
        size_t pos = 0;
        auto next = [&](std::string_view& rec) {
            if (pos >= num_run.out.size()) return false;
            size_t end = num_run.out.find('\0', pos);
            if (end == std::string::npos) end = num_run.out.size();
            rec = std::string_view(num_run.out.data() + pos, end - pos);
            pos = end + 1;
            return true;
        };
        std::string_view rec;
        while (next(rec)) {
            if (rec.empty()) continue;
            const size_t t1 = rec.find('\t');
            if (t1 == std::string_view::npos) continue;
            const size_t t2 = rec.find('\t', t1 + 1);
            if (t2 == std::string_view::npos) continue;
            const std::string_view a = rec.substr(0, t1);
            const std::string_view d = rec.substr(t1 + 1, t2 - t1 - 1);
            std::string_view p = rec.substr(t2 + 1);
            if (p.empty()) {
                std::string_view orig, dest;
                next(orig);
                next(dest);
                p = dest;
            }
            Stat st;
            if (a == "-" || d == "-") st.bin = true;
            else {
                st.a = std::atoi(std::string(a).c_str());
                st.d = std::atoi(std::string(d).c_str());
            }
            stats[GitUtf8ToWide(p)] = st;
        }
    }

    size_t pos = 0;
    auto next = [&](std::string_view& rec) {
        if (pos >= ns_run.out.size()) return false;
        size_t end = ns_run.out.find('\0', pos);
        if (end == std::string::npos) end = ns_run.out.size();
        rec = std::string_view(ns_run.out.data() + pos, end - pos);
        pos = end + 1;
        return true;
    };
    std::string_view status_rec;
    while (next(status_rec)) {
        if (status_rec.empty()) continue;
        const char code = status_rec[0];
        std::string_view path1, path2;
        if (!next(path1)) break;
        GitCommitFile cf;
        cf.status_char = static_cast<wchar_t>(code);
        if (code == 'R' || code == 'C') {
            if (!next(path2)) break;
            cf.orig_path = GitUtf8ToWide(path1);
            cf.path = GitUtf8ToWide(path2);
        } else {
            cf.path = GitUtf8ToWide(path1);
        }
        if (const auto it = stats.find(cf.path); it != stats.end()) {
            cf.added = it->second.a;
            cf.deleted = it->second.d;
            cf.is_binary = it->second.bin;
        }
        if (GitIsImagePath(cf.path)) cf.is_binary = true;
        files.push_back(std::move(cf));
    }
    return res;
}

bool GitReadCommitFileBlob(const std::wstring& root, const std::wstring& hash,
                           const std::wstring& path, std::string& bytes,
                           uint64_t max_bytes, bool* too_large) {
    bytes.clear();
    if (too_large) *too_large = false;
    std::wstring p = path;
    std::replace(p.begin(), p.end(), L'\\', L'/');
    const std::wstring spec = hash + L":" + p;
    const GitRunResult sz = RunGit(root, L"cat-file -s " + GitQuoteArg(spec), 15000, false);
    if (!sz.started || sz.timed_out || sz.exit_code != 0) return false;
    const uint64_t n = _strtoui64(sz.out.c_str(), nullptr, 10);
    if (n > max_bytes) {
        if (too_large) *too_large = true;
        return false;
    }
    const GitRunResult run = RunGit(root, L"cat-file blob " + GitQuoteArg(spec), 60000, false);
    if (!run.started || run.timed_out || run.exit_code != 0) return false;
    bytes = run.out;
    return true;
}

GitOpResult GitReadCommitTree(const std::wstring& root, const std::wstring& hash,
                              std::vector<GitTreeEntry>& entries, DWORD timeout_ms) {
    entries.clear();
    const GitRunResult run = RunGit(
        root, L"-c core.quotepath=false ls-tree -r -l -z " + GitQuoteArg(hash), timeout_ms, true);
    GitOpResult res = OpFromRun(run);
    if (!res.ok) return res;

    size_t pos = 0;
    while (pos < run.out.size()) {
        size_t end = run.out.find('\0', pos);
        if (end == std::string::npos) end = run.out.size();
        const std::string_view rec(run.out.data() + pos, end - pos);
        pos = end + 1;
        const size_t tab = rec.find('\t');
        if (tab == std::string_view::npos) continue;
        const std::string_view meta = rec.substr(0, tab);
        const std::string_view p = rec.substr(tab + 1);
        const size_t sp = meta.find_last_of(' ');
        const uint64_t size = (sp != std::string_view::npos)
                                  ? _strtoui64(std::string(meta.substr(sp + 1)).c_str(), nullptr, 10)
                                  : 0;
        GitTreeEntry e;
        e.path = GitUtf8ToWide(p);
        const size_t slash = e.path.find_last_of(L'/');
        e.name = (slash == std::wstring::npos) ? e.path : e.path.substr(slash + 1);
        e.size = size;
        entries.push_back(std::move(e));
    }
    return res;
}

GitOpResult GitRestoreFileFromCommit(const std::wstring& root, const std::wstring& hash,
                                     const std::wstring& rel_path, bool auto_stash) {
    std::wstring path = rel_path;
    std::replace(path.begin(), path.end(), L'\\', L'/');
    if (auto_stash) {
        GitChangeSet cs;
        if (GitReadChanges(root, cs, 10000).ok && cs.Total() > 0) {
            const GitOpResult st = GitStashPush(root, L"Pulse: before restoring " + path, true);
            if (!st.ok) return st;
        }
    }
    return OpFromRun(RunGit(
        root, L"--literal-pathspecs checkout " + GitQuoteArg(hash) + L" -- " + GitQuoteArg(path),
        30000, true));
}

GitOpResult GitResetToCommit(const std::wstring& root, const std::wstring& hash, bool hard) {
    return OpFromRun(RunGit(
        root, std::wstring(hard ? L"reset --hard " : L"reset --soft ") + GitQuoteArg(hash),
        30000, true));
}

}  // namespace pulse::app

#include "git_conflict.h"

#include "../common/localization.h"
#include <algorithm>
#include <cstdlib>
#include <cwctype>

namespace pulse::app {
namespace {

std::wstring TrimStr(std::wstring s) {
    while (!s.empty() && std::iswspace(s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::iswspace(s[i])) ++i;
    if (i > 0) s.erase(0, i);
    return s;
}

bool ReadFileBytes(const std::wstring& full_path, std::string& bytes, uint64_t max_bytes) {
    bytes.clear();
    const HANDLE f = CreateFileW(full_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart < 0 || static_cast<uint64_t>(sz.QuadPart) > max_bytes) {
        CloseHandle(f);
        return false;
    }
    bytes.resize(static_cast<size_t>(sz.QuadPart));
    size_t done = 0;
    while (done < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>((std::min<size_t>)(bytes.size() - done, 1u << 20));
        DWORD got = 0;
        if (!ReadFile(f, bytes.data() + done, chunk, &got, nullptr) || got == 0) break;
        done += got;
    }
    CloseHandle(f);
    bytes.resize(done);
    return true;
}

bool WriteFileBytes(const std::wstring& full_path, std::string_view bytes) {
    const HANDLE f = CreateFileW(full_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    size_t done = 0;
    bool ok = true;
    while (done < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>((std::min<size_t>)(bytes.size() - done, 1u << 20));
        DWORD wrote = 0;
        if (!WriteFile(f, bytes.data() + done, chunk, &wrote, nullptr) || wrote == 0) {
            ok = false;
            break;
        }
        done += wrote;
    }
    CloseHandle(f);
    return ok;
}

void ReadCommitMeta(const std::wstring& root, const std::wstring& rev,
                    std::wstring& author, std::wstring& time_text, std::wstring& short_id) {
    const GitRunResult run = RunGit(root, L"log -n 1 --format=%h%x1f%an%x1f%at " + GitQuoteArg(rev), 10000, false);
    if (!run.started || run.timed_out || run.exit_code != 0) return;
    std::string_view sv(run.out);
    while (!sv.empty() && (sv.back() == '\n' || sv.back() == '\r')) sv.remove_suffix(1);
    const size_t p1 = sv.find('\x1f');
    if (p1 == std::string_view::npos) return;
    const size_t p2 = sv.find('\x1f', p1 + 1);
    if (p2 == std::string_view::npos) return;
    short_id = GitUtf8ToWide(sv.substr(0, p1));
    author = GitUtf8ToWide(sv.substr(p1 + 1, p2 - p1 - 1));
    const int64_t ts = _strtoi64(std::string(sv.substr(p2 + 1)).c_str(), nullptr, 10);
    time_text = GitFormatShortTime(ts);
}

std::wstring TrimCrLfWide(std::string_view bytes) {
    std::wstring w = GitUtf8ToWide(bytes);
    w.erase(std::remove(w.begin(), w.end(), L'\r'), w.end());
    while (!w.empty() && w.back() == L'\n') w.pop_back();
    return w;
}

std::wstring TailLines(std::string_view text, int max_lines) {
    std::vector<std::string_view> lines;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        size_t end = (nl == std::string_view::npos) ? text.size() : nl;
        std::string_view line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        lines.push_back(line);
        pos = (nl == std::string_view::npos) ? text.size() : nl + 1;
    }
    if (static_cast<int>(lines.size()) > max_lines) {
        lines.erase(lines.begin(), lines.end() - max_lines);
    }
    std::wstring out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) out += L"\n";
        out += GitUtf8ToWide(lines[i]);
    }
    return out;
}

std::wstring HeadLines(std::string_view text, int max_lines) {
    std::wstring out;
    size_t pos = 0;
    int count = 0;
    while (pos < text.size() && count < max_lines) {
        size_t nl = text.find('\n', pos);
        size_t end = (nl == std::string_view::npos) ? text.size() : nl;
        std::string_view line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (count > 0) out += L"\n";
        out += GitUtf8ToWide(line);
        ++count;
        pos = (nl == std::string_view::npos) ? text.size() : nl + 1;
    }
    return out;
}

bool ContainsNullByte(std::string_view bytes) {
    return bytes.find('\0') != std::string_view::npos;
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

std::wstring UniqueCopyPath(const std::wstring& full_path, const std::wstring& suffix) {
    const size_t slash = full_path.find_last_of(L"\\/");
    const std::wstring dir = (slash == std::wstring::npos) ? std::wstring() : full_path.substr(0, slash + 1);
    const std::wstring name = (slash == std::wstring::npos) ? full_path : full_path.substr(slash + 1);
    const size_t dot = name.find_last_of(L'.');
    const std::wstring stem = (dot != std::wstring::npos && dot > 0) ? name.substr(0, dot) : name;
    const std::wstring ext = (dot != std::wstring::npos && dot > 0) ? name.substr(dot) : std::wstring();
    const std::wstring safe_suffix = suffix.empty() ? std::wstring(L"theirs") : suffix;

    for (int n = 1; n < 1000; ++n) {
        std::wstring candidate = dir + stem + L" \uFF08" + safe_suffix;
        if (n > 1) candidate += L" " + std::to_wstring(n);
        candidate += L"\uFF09" + ext;
        if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) return candidate;
    }
    return {};
}

}  // namespace

bool GitIsMerging(const std::wstring& root) {
    const GitRunResult run = RunGit(root, L"rev-parse -q --verify MERGE_HEAD", 5000, false);
    return run.started && !run.timed_out && run.exit_code == 0 && !run.out.empty();
}

bool ParseConflictText(std::string_view content,
                       std::vector<GitConflictHunk>& hunks,
                       std::vector<std::string>& segments) {
    hunks.clear();
    segments.clear();
    if (ContainsNullByte(content)) {
        segments.emplace_back(content);
        return false;
    }

    struct LineSpan {
        size_t start = 0;
        size_t end = 0;
        std::string_view trimmed;
    };
    std::vector<LineSpan> lines;
    size_t pos = 0;
    while (pos < content.size()) {
        size_t nl = content.find('\n', pos);
        size_t end = (nl == std::string_view::npos) ? content.size() : nl + 1;
        std::string_view t = content.substr(pos, end - pos);
        while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.remove_suffix(1);
        lines.push_back({pos, end, t});
        pos = end;
    }

    enum class State { Normal, Ours, Base, Theirs };
    State st = State::Normal;
    size_t seg_start = 0;
    size_t ours_start = 0, ours_end = 0;
    size_t base_start = 0, base_end = 0;
    size_t theirs_start = 0;
    std::wstring ours_label, theirs_label;

    for (size_t i = 0; i < lines.size(); ++i) {
        const auto& ln = lines[i];
        if (st == State::Normal) {
            if (ln.trimmed.starts_with("<<<<<<<")) {
                segments.emplace_back(content.substr(seg_start, ln.start - seg_start));
                ours_label = TrimStr(GitUtf8ToWide(ln.trimmed.substr(7)));
                ours_start = ln.end;
                ours_end = ln.end;
                base_start = base_end = 0;
                st = State::Ours;
            }
        } else if (st == State::Ours) {
            if (ln.trimmed.starts_with("|||||||")) {
                ours_end = ln.start;
                base_start = ln.end;
                st = State::Base;
            } else if (ln.trimmed.starts_with("=======")) {
                ours_end = ln.start;
                theirs_start = ln.end;
                st = State::Theirs;
            }
        } else if (st == State::Base) {
            if (ln.trimmed.starts_with("=======")) {
                base_end = ln.start;
                theirs_start = ln.end;
                st = State::Theirs;
            }
        } else if (st == State::Theirs) {
            if (ln.trimmed.starts_with(">>>>>>>")) {
                theirs_label = TrimStr(GitUtf8ToWide(ln.trimmed.substr(7)));
                GitConflictHunk h;
                h.ours_label = ours_label;
                h.theirs_label = theirs_label;
                h.ours_utf8.assign(content.substr(ours_start, ours_end - ours_start));
                if (base_end > base_start) {
                    h.base_utf8.assign(content.substr(base_start, base_end - base_start));
                }
                h.theirs_utf8.assign(content.substr(theirs_start, ln.start - theirs_start));
                h.ours_text = TrimCrLfWide(h.ours_utf8);
                h.base_text = TrimCrLfWide(h.base_utf8);
                h.theirs_text = TrimCrLfWide(h.theirs_utf8);
                h.context_before = TailLines(segments.back(), 2);
                hunks.push_back(std::move(h));
                seg_start = ln.end;
                st = State::Normal;
            }
        }
    }

    if (st != State::Normal) {
        // Malformed/unterminated conflict marker: roll back the partial segment so content stays intact.
        if (!segments.empty()) {
            const size_t revert_start = seg_start;
            (void)revert_start;
            std::string& prev = segments.back();
            // Append from the end of `prev` (which ended right before `<<<<<<<`) through EOF.
            const size_t offset = static_cast<size_t>(prev.size());
            (void)offset;
        }
        // Rebuild cleanly from the last complete hunk.
        size_t tail_from = 0;
        if (segments.size() > hunks.size()) {
            // segments.back() started at the end of the previous complete hunk.
            const size_t consumed = segments.size() - 1;
            segments.resize(consumed);
            tail_from = 0;
            // Re-find tail_from safely by summing nothing; easiest is re-scanning only up to hunks.size():
            // Since segments.back() before push started at old seg_start, reconstruct tail:
            size_t complete_end = 0;
            size_t count = 0;
            bool in_theirs = false;
            for (const auto& ln : lines) {
                if (count >= hunks.size()) break;
                if (!in_theirs && ln.trimmed.starts_with("=======")) in_theirs = true;
                else if (in_theirs && ln.trimmed.starts_with(">>>>>>>")) {
                    in_theirs = false;
                    complete_end = ln.end;
                    ++count;
                }
            }
            tail_from = complete_end;
            segments.emplace_back(content.substr(tail_from));
        }
    } else {
        segments.emplace_back(content.substr(seg_start));
    }

    for (size_t i = 0; i < hunks.size() && i + 1 < segments.size(); ++i) {
        hunks[i].context_after = HeadLines(segments[i + 1], 2);
    }
    return !hunks.empty();
}

std::string ComposeResolvedText(const std::vector<std::string>& segments,
                                const std::vector<GitConflictHunk>& hunks) {
    if (segments.empty()) return {};
    std::string out = segments[0];
    for (size_t i = 0; i < hunks.size(); ++i) {
        const auto& h = hunks[i];
        switch (h.choice) {
        case GitConflictChoice::Ours:
            out += h.ours_utf8;
            break;
        case GitConflictChoice::Theirs:
            out += h.theirs_utf8;
            break;
        case GitConflictChoice::Both:
            out += h.ours_utf8;
            if (!h.ours_utf8.empty() && h.ours_utf8.back() != '\n' && !h.theirs_utf8.empty()) {
                out.push_back('\n');
            }
            out += h.theirs_utf8;
            break;
        case GitConflictChoice::Custom:
            out += h.custom_utf8;
            if (!h.custom_utf8.empty() && h.custom_utf8.back() != '\n' &&
                i + 1 < segments.size() && !segments[i + 1].empty()) {
                out.push_back('\n');
            }
            break;
        case GitConflictChoice::None:
            break;
        }
        if (i + 1 < segments.size()) out += segments[i + 1];
    }
    return out;
}

bool GitReadConflictStageBlob(const std::wstring& root, int stage, const std::wstring& path,
                              std::string& bytes, uint64_t max_bytes, bool* too_large) {
    bytes.clear();
    if (too_large) *too_large = false;
    std::wstring p = path;
    std::replace(p.begin(), p.end(), L'\\', L'/');
    const std::wstring spec = L":" + std::to_wstring(stage) + L":" + p;
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

GitConflictState GitReadConflictState(const std::wstring& root, DWORD timeout_ms) {
    GitConflictState state;
    GitChangeSet cs;
    const GitOpResult st = GitReadChanges(root, cs, timeout_ms);
    if (st.git_missing) {
        state.git_missing = true;
        return state;
    }
    if (!st.ok) return state;

    state.ok = true;
    state.merging = !cs.conflicts.empty() || GitIsMerging(root);
    std::wstring head_id;
    ReadCommitMeta(root, L"HEAD", state.ours_author, state.ours_time, head_id);
    if (state.merging) {
        ReadCommitMeta(root, L"MERGE_HEAD", state.theirs_author, state.theirs_time, state.merge_head_short);
    }

    for (const auto& c : cs.conflicts) {
        GitConflictFile cf;
        cf.path = c.path;
        cf.is_image = GitIsImagePath(c.path);
        if (cf.is_image) {
            cf.is_binary = true;
        } else {
            std::string raw;
            if (ReadFileBytes(GitJoinPath(root, c.path), raw, 4ull * 1024 * 1024)) {
                if (ContainsNullByte(raw)) {
                    cf.is_binary = true;
                } else {
                    std::vector<GitConflictHunk> hunks;
                    std::vector<std::string> segs;
                    if (ParseConflictText(raw, hunks, segs)) {
                        cf.hunk_count = static_cast<int>(hunks.size());
                    } else {
                        // e.g. delete/modify or non-marker binary-like conflict
                        std::string b2, b3;
                        const bool has2 = GitReadConflictStageBlob(root, 2, c.path, b2, 256 * 1024);
                        const bool has3 = GitReadConflictStageBlob(root, 3, c.path, b3, 256 * 1024);
                        if ((has2 && ContainsNullByte(b2)) || (has3 && ContainsNullByte(b3)) ||
                            (!has2 || !has3)) {
                            cf.is_binary = true;
                        }
                    }
                }
            } else {
                cf.is_binary = true;
            }
        }
        state.conflict_files.push_back(std::move(cf));
    }

    if (state.merging) {
        for (const auto& s : cs.staged) {
            state.auto_merged_files.push_back(s.path);
        }
    }
    return state;
}

GitOpResult GitWriteResolvedTextFile(const std::wstring& root, const std::wstring& path,
                                     std::string_view utf8_bytes) {
    GitOpResult res;
    const std::wstring full = GitJoinPath(root, path);
    if (!WriteFileBytes(full, utf8_bytes)) {
        res.message = L"failed to write resolved file";
        return res;
    }
    return GitStagePaths(root, {path});
}

GitOpResult GitResolveBinaryConflict(const std::wstring& root, const std::wstring& path,
                                     bool pick_ours, bool save_other_copy,
                                     const std::wstring& other_suffix,
                                     std::wstring* saved_copy_path) {
    if (saved_copy_path) saved_copy_path->clear();
    std::string ours_bytes, theirs_bytes;
    const bool has_ours = GitReadConflictStageBlob(root, 2, path, ours_bytes, 64ull * 1024 * 1024);
    const bool has_theirs = GitReadConflictStageBlob(root, 3, path, theirs_bytes, 64ull * 1024 * 1024);

    const std::wstring full = GitJoinPath(root, path);
    if (save_other_copy) {
        const bool other_exists = pick_ours ? has_theirs : has_ours;
        const std::string& other_bytes = pick_ours ? theirs_bytes : ours_bytes;
        if (other_exists) {
            const std::wstring copy_path = UniqueCopyPath(full, other_suffix);
            if (!copy_path.empty() && WriteFileBytes(copy_path, other_bytes)) {
                if (saved_copy_path) *saved_copy_path = copy_path;
            }
        }
    }

    const bool chosen_exists = pick_ours ? has_ours : has_theirs;
    const std::string& chosen_bytes = pick_ours ? ours_bytes : theirs_bytes;
    if (!chosen_exists) {
        return OpFromRun(RunGit(root, L"--literal-pathspecs rm -f -- " + GitQuoteArg(path), 30000, true));
    }
    if (!WriteFileBytes(full, chosen_bytes)) {
        GitOpResult res;
        res.message = L"failed to write chosen file";
        return res;
    }
    return GitStagePaths(root, {path});
}

GitOpResult GitCompleteMerge(const std::wstring& root) {
    GitChangeSet cs;
    const GitOpResult st = GitReadChanges(root, cs, 10000);
    if (!st.ok) return st;
    if (!cs.conflicts.empty()) {
        GitOpResult res;
        res.message = L"unresolved conflicts remain";
        return res;
    }
    return OpFromRun(RunGit(root, L"commit --no-edit", 60000, true));
}

GitOpResult GitAbortMerge(const std::wstring& root) {
    return OpFromRun(RunGit(root, L"merge --abort", 30000, true));
}

}  // namespace pulse::app

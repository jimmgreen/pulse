#include "git_changes.h"
#include "git_terms.h"

#include <objbase.h>
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <ctime>
#include <cwctype>
#include <unordered_map>

namespace pulse::app {
namespace {

std::string_view AfterFields(std::string_view rec, int skip) {
    size_t at = 0;
    for (int i = 0; i < skip; ++i) {
        at = rec.find(' ', at);
        if (at == std::string_view::npos) return {};
        ++at;
    }
    return rec.substr(at);
}

std::string_view Field(std::string_view rec, int index) {
    std::string_view rest = AfterFields(rec, index);
    const size_t end = rest.find(' ');
    return end == std::string_view::npos ? rest : rest.substr(0, end);
}

int ParseCount(std::string_view text) {
    int n = 0;
    for (char c : text) {
        if (c < '0' || c > '9') break;
        n = n * 10 + (c - '0');
        if (n > 1000000) break;
    }
    return n;
}

bool KindOf(char code, GitChangeKind& kind) {
    switch (code) {
    case 'M': kind = GitChangeKind::Modified; return true;
    case 'A': kind = GitChangeKind::Added; return true;
    case 'D': kind = GitChangeKind::Deleted; return true;
    case 'R': kind = GitChangeKind::Renamed; return true;
    case 'C': kind = GitChangeKind::Copied; return true;
    case 'T': kind = GitChangeKind::TypeChanged; return true;
    default: return false;
    }
}

// The lines git marked as the problem, else its last lines.
std::wstring FailureLines(const std::string& out) {
    const std::wstring text = GitUtf8ToWide(out);
    std::vector<std::wstring> lines, picked;
    size_t at = 0;
    while (at <= text.size()) {
        size_t end = text.find_first_of(L"\r\n", at);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(at, end - at);
        while (!line.empty() && (line.back() == L' ' || line.back() == L'\t')) line.pop_back();
        if (!line.empty() && !line.starts_with(L"hint:")) lines.push_back(std::move(line));
        at = end + 1;
    }
    for (const auto& line : lines) {
        if (line.starts_with(L"fatal:") || line.starts_with(L"error:") || line.starts_with(L"CONFLICT") ||
            line.starts_with(L"Please tell me who you are") || line.starts_with(L"nothing to commit") ||
            line.starts_with(L"no changes added"))
            picked.push_back(line);
    }
    if (picked.empty()) picked = lines;
    if (picked.size() > 3) picked.erase(picked.begin(), picked.end() - 3);
    std::wstring joined;
    for (const auto& line : picked) {
        if (!joined.empty()) joined += L"\n";
        joined += line;
    }
    if (joined.size() > 400) joined = joined.substr(0, 399) + L"\u2026";
    return joined;
}

GitOpResult FromRun(const GitRunResult& run) {
    GitOpResult r;
    if (!run.started) {
        r.git_missing = true;
        return r;
    }
    if (run.timed_out) {
        r.message = L"git timed out";
        return r;
    }
    r.ok = run.exit_code == 0;
    if (!r.ok) r.message = FailureLines(run.out);
    return r;
}

std::wstring TempDir() {
    wchar_t base[MAX_PATH + 1]{};
    const DWORD n = GetTempPathW(MAX_PATH, base);
    if (n == 0 || n > MAX_PATH) return {};
    std::wstring dir = base;
    if (!dir.empty() && dir.back() != L'\\') dir += L'\\';
    dir += L"Pulse";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\git";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

// A NUL-separated UTF-8 pathspec file for --pathspec-from-file: no command
// line length limit and no quoting of odd names.
std::wstring PathspecFile(const std::vector<std::wstring>& paths) {
    std::string bytes;
    for (const auto& path : paths) {
        bytes += GitWideToUtf8(path);
        bytes.push_back('\0');
    }
    return GitWriteTempFile(bytes, L"pathspec.txt");
}

GitOpResult RunWithPathspec(const std::wstring& root, const std::wstring& command,
                            const std::vector<std::wstring>& paths, DWORD timeout_ms) {
    GitOpResult r;
    if (paths.empty()) {
        r.ok = true;
        return r;
    }
    const std::wstring file = PathspecFile(paths);
    if (file.empty()) {
        r.message = L"could not write a temporary file";
        return r;
    }
    // --literal-pathspecs: a file named "*.txt" or ":x" means itself.
    const std::wstring args = L"--literal-pathspecs " + command + L" " +
                              GitQuoteArg(L"--pathspec-from-file=" + file) + L" --pathspec-file-nul";
    r = FromRun(RunGit(root, args, timeout_ms, true));
    DeleteFileW(file.c_str());
    return r;
}

std::atomic<unsigned> g_temp_counter{0};

}  // namespace

bool ParseGitChanges(std::string_view out, GitChangeSet& set) {
    bool header = false;
    size_t pos = 0;
    auto next = [&](std::string_view& rec) {
        if (pos >= out.size()) return false;
        size_t end = out.find('\0', pos);
        if (end == std::string_view::npos) end = out.size();
        rec = out.substr(pos, end - pos);
        pos = end + 1;
        return true;
    };
    std::vector<GitChange> untracked;
    std::string_view rec;
    while (next(rec)) {
        if (rec.empty()) continue;
        const char type = rec[0];
        if (type == '#') {
            header = true;
            auto value = [&](std::string_view key, std::string_view& v) {
                if (!rec.starts_with(key)) return false;
                v = rec.substr(key.size());
                return true;
            };
            std::string_view v;
            if (value("# branch.oid ", v)) {
                if (v != "(initial)") set.head = GitUtf8ToWide(v.substr(0, 7));
            } else if (value("# branch.head ", v)) {
                if (v != "(detached)") set.branch = GitUtf8ToWide(v);
            } else if (value("# branch.upstream ", v)) {
                set.upstream = GitUtf8ToWide(v);
            } else if (value("# branch.ab ", v)) {
                const size_t plus = v.find('+'), minus = v.find('-');
                if (plus != std::string_view::npos) set.ahead = ParseCount(v.substr(plus + 1));
                if (minus != std::string_view::npos) set.behind = ParseCount(v.substr(minus + 1));
                set.has_upstream_ab = true;
            }
            continue;
        }
        if ((type == '1' || type == '2') && rec.size() > 4) {
            const char x = rec[2], y = rec[3];
            const bool submodule = Field(rec, 2).starts_with('S');
            std::string_view path = AfterFields(rec, type == '1' ? 8 : 9);
            std::string_view original;
            if (type == '2') next(original);  // -z puts the source path in its own record
            if (path.empty()) continue;
            GitChange change;
            change.path = GitUtf8ToWide(path);
            change.submodule = submodule;
            GitChangeKind kind{};
            if (KindOf(x, kind)) {
                GitChange staged = change;
                staged.kind = kind;
                if (type == '2') staged.orig_path = GitUtf8ToWide(original);
                set.staged.push_back(std::move(staged));
            }
            if (KindOf(y, kind)) {
                GitChange work = change;
                work.kind = kind;
                set.unstaged.push_back(std::move(work));
            }
        } else if (type == 'u') {
            std::string_view path = AfterFields(rec, 10);
            if (path.empty()) continue;
            GitChange change;
            change.path = GitUtf8ToWide(path);
            change.kind = GitChangeKind::Conflict;
            set.conflicts.push_back(std::move(change));
        } else if (type == '?' && rec.size() > 2) {
            GitChange change;
            change.path = GitUtf8ToWide(rec.substr(2));
            change.kind = GitChangeKind::Untracked;
            untracked.push_back(std::move(change));
        }
        // '!' (ignored) and anything newer: skipped.
    }
    for (auto& change : untracked) set.unstaged.push_back(std::move(change));
    return header;
}

wchar_t GitChangeLetter(GitChangeKind kind) noexcept {
    switch (kind) {
    case GitChangeKind::Modified: return L'M';
    case GitChangeKind::Added: return L'A';
    case GitChangeKind::Deleted: return L'D';
    case GitChangeKind::Renamed: return L'R';
    case GitChangeKind::Copied: return L'C';
    case GitChangeKind::TypeChanged: return L'T';
    case GitChangeKind::Untracked: return L'U';
    case GitChangeKind::Conflict: return L'!';
    }
    return L'?';
}

ui::GitMark GitChangeMark(GitChangeKind kind) noexcept {
    switch (kind) {
    case GitChangeKind::Added:
    case GitChangeKind::Untracked:
    case GitChangeKind::Copied: return ui::GitMark::Added;
    case GitChangeKind::Renamed: return ui::GitMark::Renamed;
    case GitChangeKind::Deleted: return ui::GitMark::Deleted;
    case GitChangeKind::Conflict: return ui::GitMark::Conflict;
    default: return ui::GitMark::Modified;
    }
}

std::wstring GitQuoteArg(std::wstring_view arg) {
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        if (c == L'"') {
            out.append(slashes * 2 + 1, L'\\');
            out += L'"';
        } else {
            out.append(slashes, L'\\');
            out += c;
        }
        slashes = 0;
    }
    out.append(slashes * 2, L'\\');
    out += L'"';
    return out;
}

std::string GitWideToUtf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string out(static_cast<size_t>((std::max)(n, 0)), '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr,
                            nullptr);
    return out;
}

std::wstring GitJoinPath(const std::wstring& root, const std::wstring& rel) {
    std::wstring path = root;
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    path += L'\\';
    for (wchar_t c : rel) path += c == L'/' ? L'\\' : c;
    return path;
}

GitOpResult GitReadChanges(const std::wstring& root, GitChangeSet& set, DWORD timeout_ms) {
    const GitRunResult run = RunGit(
        root, L"--no-optional-locks status --porcelain=v2 --branch -z --untracked-files=all", timeout_ms, false);
    GitOpResult r = FromRun(run);
    if (!r.ok) return r;
    set = GitChangeSet{};
    set.root = root;
    if (!ParseGitChanges(run.out, set)) {
        r.ok = false;
        r.message = L"unexpected git status output";
    }
    return r;
}

GitOpResult GitStagePaths(const std::wstring& root, const std::vector<std::wstring>& paths) {
    return RunWithPathspec(root, L"add -A", paths, 120000);
}

GitOpResult GitUnstagePaths(const std::wstring& root, const std::vector<std::wstring>& paths, bool has_head) {
    // Before the first commit there is no HEAD to restore from: drop them from the index.
    return RunWithPathspec(root, has_head ? L"restore --staged" : L"rm --cached -r -q", paths, 120000);
}

GitOpResult GitDiscardPaths(const std::wstring& root, const std::vector<std::wstring>& paths) {
    return RunWithPathspec(root, L"restore --worktree", paths, 120000);
}

GitOpResult GitRecycleUntracked(const std::wstring& root, const std::vector<std::wstring>& paths) {
    GitOpResult r;
    if (paths.empty()) {
        r.ok = true;
        return r;
    }
    std::wstring from;
    for (const auto& rel : paths) {
        from += GitJoinPath(root, rel);
        from.push_back(L'\0');
    }
    from.push_back(L'\0');
    // The Recycle Bin wants COM on this (worker) thread.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    const int rc = SHFileOperationW(&op);
    if (SUCCEEDED(com)) CoUninitialize();
    r.ok = rc == 0 && !op.fAnyOperationsAborted;
    if (!r.ok) r.message = L"Recycle Bin error " + std::to_wstring(rc);
    return r;
}

GitOpResult GitCommit(const std::wstring& root, const std::wstring& message, bool amend) {
    std::wstring trimmed = message;
    while (!trimmed.empty() && std::iswspace(trimmed.back())) trimmed.pop_back();
    if (amend && trimmed.empty()) return FromRun(RunGit(root, L"commit --amend --no-edit", 120000, true));
    GitOpResult r;
    if (trimmed.empty()) {
        r.message = L"empty commit message";
        return r;
    }
    std::string bytes = GitWideToUtf8(trimmed);
    bytes.erase(std::remove(bytes.begin(), bytes.end(), '\r'), bytes.end());
    bytes.push_back('\n');
    const std::wstring file = GitWriteTempFile(bytes, L"COMMIT_MSG.txt");
    if (file.empty()) {
        r.message = L"could not write a temporary file";
        return r;
    }
    std::wstring args = L"commit --cleanup=strip -F " + GitQuoteArg(file);
    if (amend) args += L" --amend";
    r = FromRun(RunGit(root, args, 120000, true));
    DeleteFileW(file.c_str());
    return r;
}

std::wstring GitLastCommitMessage(const std::wstring& root) {
    const GitRunResult run = RunGit(root, L"log -1 --format=%B", 15000, false);
    if (!run.started || run.timed_out || run.exit_code != 0) return {};
    std::wstring text = GitUtf8ToWide(run.out);
    while (!text.empty() && std::iswspace(text.back())) text.pop_back();
    return text;
}

std::wstring GitHeadId(const std::wstring& root) {
    const GitRunResult run = RunGit(root, L"rev-parse --verify -q HEAD", 15000, false);
    if (!run.started || run.timed_out || run.exit_code != 0) return {};
    std::wstring id = GitUtf8ToWide(run.out);
    while (!id.empty() && std::iswspace(id.back())) id.pop_back();
    return id;
}

GitOpResult GitUndoCommit(const std::wstring& root, const std::wstring& expect_head) {
    GitOpResult r;
    const std::wstring head = GitHeadId(root);
    if (head.empty() || head != expect_head) {
        r.message = L"HEAD has moved since that commit";
        return r;
    }
    const GitRunResult parent = RunGit(root, L"rev-parse --verify -q HEAD~1", 15000, false);
    if (!parent.started) {
        r.git_missing = true;
        return r;
    }
    if (parent.exit_code == 0) return FromRun(RunGit(root, L"reset -q --soft HEAD~1", 60000, true));
    // The first commit: drop the branch ref, the index (all its files) stays.
    const GitRunResult ref = RunGit(root, L"symbolic-ref -q HEAD", 15000, false);
    std::wstring name = GitUtf8ToWide(ref.out);
    while (!name.empty() && std::iswspace(name.back())) name.pop_back();
    if (ref.exit_code != 0 || name.empty()) {
        r.message = L"HEAD is detached";
        return r;
    }
    return FromRun(RunGit(root, L"update-ref -d " + GitQuoteArg(name) + L" " + GitQuoteArg(head), 15000, true));
}

bool GitReadBlob(const std::wstring& root, GitBlobSource source, const std::wstring& path, std::string& bytes,
                 uint64_t max_bytes, bool* too_large) {
    bytes.clear();
    if (too_large) *too_large = false;
    const std::wstring spec = (source == GitBlobSource::Head ? L"HEAD:" : L":") + path;
    const GitRunResult size = RunGit(root, L"cat-file -s " + GitQuoteArg(spec), 15000, false);
    if (!size.started || size.timed_out || size.exit_code != 0) return false;
    const uint64_t n = _strtoui64(size.out.c_str(), nullptr, 10);
    if (n > max_bytes) {
        if (too_large) *too_large = true;
        return false;
    }
    const GitRunResult run = RunGit(root, L"cat-file blob " + GitQuoteArg(spec), 60000, false);
    if (!run.started || run.timed_out || run.exit_code != 0) return false;
    bytes = run.out;
    return true;
}

std::wstring GitWriteTempFile(std::string_view bytes, const std::wstring& name_hint) {
    const std::wstring dir = TempDir();
    if (dir.empty()) return {};
    std::wstring hint = name_hint;
    for (wchar_t& c : hint)
        if (c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' || c == L'"' || c == L'<' ||
            c == L'>' || c == L'|')
            c = L'_';
    if (hint.size() > 80) hint = hint.substr(hint.size() - 80);
    const std::wstring path = dir + L"\\" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                              std::to_wstring(++g_temp_counter) + L"-" + hint;
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    size_t done = 0;
    bool ok = true;
    while (done < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>((std::min<size_t>)(bytes.size() - done, 1u << 20));
        DWORD wrote = 0;
        if (!WriteFile(file, bytes.data() + done, chunk, &wrote, nullptr) || wrote == 0) {
            ok = false;
            break;
        }
        done += wrote;
    }
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(path.c_str());
        return {};
    }
    return path;
}

void GitCleanupTempFiles() {
    const std::wstring dir = TempDir();
    if (dir.empty()) return;
    WIN32_FIND_DATAW data{};
    const HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return;
    FILETIME now_ft{};
    GetSystemTimeAsFileTime(&now_ft);
    const uint64_t now = (static_cast<uint64_t>(now_ft.dwHighDateTime) << 32) | now_ft.dwLowDateTime;
    constexpr uint64_t kDay = 24ull * 3600ull * 10000000ull;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const uint64_t written =
            (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
        if (now > written && now - written > kDay) DeleteFileW((dir + L"\\" + data.cFileName).c_str());
    } while (FindNextFileW(find, &data));
    FindClose(find);
}

bool GitIsImagePath(std::wstring_view path) noexcept {
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring_view::npos) return false;
    std::wstring ext(path.substr(dot + 1));
    for (wchar_t& c : ext) c = static_cast<wchar_t>(std::towlower(c));
    static constexpr std::wstring_view kImages[] = {L"png", L"jpg", L"jpeg", L"jfif", L"gif", L"bmp",
                                                    L"webp", L"ico", L"tif", L"tiff"};
    return std::find(std::begin(kImages), std::end(kImages), ext) != std::end(kImages);
}

std::wstring GitFormatShortTime(int64_t unix_seconds) {
    if (unix_seconds <= 0) return {};
    const __time64_t ts = static_cast<__time64_t>(unix_seconds);
    const __time64_t now = _time64(nullptr);
    std::tm tm_val{}, tm_now{};
    if (_localtime64_s(&tm_val, &ts) != 0 || _localtime64_s(&tm_now, &now) != 0) return {};
    wchar_t buf[64]{};
    if (tm_val.tm_year == tm_now.tm_year && tm_val.tm_yday == tm_now.tm_yday) {
        swprintf_s(buf, L"%02d:%02d", tm_val.tm_hour, tm_val.tm_min);
        return buf;
    }
    if (tm_val.tm_year == tm_now.tm_year) {
        swprintf_s(buf, L"%02d/%02d", tm_val.tm_mon + 1, tm_val.tm_mday);
        return buf;
    }
    swprintf_s(buf, L"%04d/%02d/%02d", tm_val.tm_year + 1900, tm_val.tm_mon + 1, tm_val.tm_mday);
    return buf;
}

GitIdentity GitReadIdentity(const std::wstring& root) {
    GitIdentity id;
    auto trim = [](std::string_view sv) {
        std::wstring w = GitUtf8ToWide(sv);
        while (!w.empty() && std::iswspace(w.back())) w.pop_back();
        return w;
    };
    const GitRunResult name_run = RunGit(root, L"config user.name", 5000, false);
    if (name_run.started && !name_run.timed_out && name_run.exit_code == 0) id.name = trim(name_run.out);
    const GitRunResult email_run = RunGit(root, L"config user.email", 5000, false);
    if (email_run.started && !email_run.timed_out && email_run.exit_code == 0) id.email = trim(email_run.out);
    return id;
}

std::wstring GitSuggestCommitMessage(const std::vector<GitChange>& staged, bool /*plain_terms*/) {
    if (staged.empty()) return {};
    auto stem_of = [](const std::wstring& rel) {
        const size_t slash = rel.find_last_of(L"\\/");
        const std::wstring file = slash == std::wstring::npos ? rel : rel.substr(slash + 1);
        const size_t dot = file.find_last_of(L'.');
        return (dot != std::wstring::npos && dot > 0) ? file.substr(0, dot) : file;
    };
    auto tmpl = [](l10n::StringId id, const wchar_t* fallback) {
        const std::wstring& s = l10n::Get(id);
        return s.empty() ? std::wstring(fallback) : s;
    };
    if (staged.size() == 1) {
        return GitFmt(tmpl(l10n::StringId::GitSuggestOne, L"Update {name}"), L"{name}", stem_of(staged[0].path));
    }
    if (staged.size() == 2) {
        std::wstring msg = GitFmt(tmpl(l10n::StringId::GitSuggestTwo, L"Update {a}, {b}"), L"{a}", stem_of(staged[0].path));
        return GitFmt(std::move(msg), L"{b}", stem_of(staged[1].path));
    }
    std::wstring msg = GitFmt(tmpl(l10n::StringId::GitSuggestMany, L"Update {a}, {b} ({n})"), L"{a}", stem_of(staged[0].path));
    msg = GitFmt(std::move(msg), L"{b}", stem_of(staged[1].path));
    return GitFmtCount(std::move(msg), static_cast<int>(staged.size()));
}

namespace {

bool ParseHunkRange(std::string_view header, int& old_start, int& old_count, int& new_start, int& new_count) {
    if (!header.starts_with("@@ -")) return false;
    auto parse_pair = [](std::string_view s, int& start, int& count) -> bool {
        size_t i = 0;
        if (i >= s.size() || s[i] < '0' || s[i] > '9') return false;
        start = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            start = start * 10 + (s[i] - '0');
            ++i;
        }
        if (i < s.size() && s[i] == ',') {
            ++i;
            count = 0;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
                count = count * 10 + (s[i] - '0');
                ++i;
            }
        } else {
            count = 1;
        }
        return true;
    };
    const size_t plus = header.find(" +", 4);
    if (plus == std::string_view::npos) return false;
    const size_t end_at = header.find(" @@", plus + 2);
    if (end_at == std::string_view::npos) return false;
    return parse_pair(header.substr(4, plus - 4), old_start, old_count) &&
           parse_pair(header.substr(plus + 2, end_at - (plus + 2)), new_start, new_count);
}

std::vector<std::pair<size_t, size_t>> SplitByteLines(std::string_view text) {
    std::vector<std::pair<size_t, size_t>> spans;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        size_t next = (nl == std::string_view::npos) ? text.size() : nl + 1;
        spans.emplace_back(pos, next);
        pos = next;
    }
    return spans;
}

std::string_view TrimCrLf(std::string_view line) {
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.remove_suffix(1);
    return line;
}

std::wstring FormatBlameDate(int64_t unix_seconds) {
    if (unix_seconds <= 0) return {};
    const __time64_t ts = static_cast<__time64_t>(unix_seconds);
    std::tm tm_val{};
    if (_localtime64_s(&tm_val, &ts) != 0) return {};
    wchar_t buf[32]{};
    swprintf_s(buf, L"%02d/%02d", tm_val.tm_mon + 1, tm_val.tm_mday);
    return buf;
}

}  // namespace

std::vector<GitDiffHunkPatch> ParseUnifiedDiffHunks(std::string_view raw_diff) {
    std::vector<GitDiffHunkPatch> out;
    const auto lines = SplitByteLines(raw_diff);
    std::string current_header;
    bool in_hunk = false;
    GitDiffHunkPatch cur{};
    auto flush_hunk = [&] {
        if (!in_hunk) return;
        if (!current_header.empty()) {
            cur.patch_bytes.insert(0, current_header);
            out.push_back(std::move(cur));
        }
        cur = GitDiffHunkPatch{};
        in_hunk = false;
    };
    for (const auto& [b, e] : lines) {
        const std::string_view full = raw_diff.substr(b, e - b);
        const std::string_view trimmed = TrimCrLf(full);
        if (trimmed.starts_with("diff --git ")) {
            flush_hunk();
            current_header.assign(full);
            continue;
        }
        if (trimmed.starts_with("@@ ")) {
            flush_hunk();
            if (ParseHunkRange(trimmed, cur.old_start, cur.old_count, cur.new_start, cur.new_count)) {
                in_hunk = true;
                cur.header = GitUtf8ToWide(trimmed);
                cur.patch_bytes.assign(full);
            }
            continue;
        }
        if (in_hunk) {
            cur.patch_bytes.append(full);
        } else if (!current_header.empty()) {
            if (trimmed.starts_with("Binary files ") || trimmed.starts_with("GIT binary patch")) {
                current_header.clear();
            } else {
                current_header.append(full);
            }
        }
    }
    flush_hunk();
    return out;
}

GitOpResult GitReadFileHunks(const std::wstring& root, const std::wstring& path, bool cached,
                             std::vector<GitDiffHunkPatch>& out, DWORD timeout_ms) {
    out.clear();
    std::wstring args = L"-c core.quotepath=false --literal-pathspecs diff -U3 --no-color --no-ext-diff ";
    if (cached) args += L"--cached ";
    args += L"-- " + GitQuoteArg(path);
    const GitRunResult run = RunGit(root, args, timeout_ms, true);
    GitOpResult res = FromRun(run);
    if (!res.ok) return res;
    out = ParseUnifiedDiffHunks(run.out);
    return res;
}

GitOpResult GitApplyHunkPatch(const std::wstring& root, std::string_view patch_bytes,
                              bool cached, bool reverse, DWORD timeout_ms) {
    GitOpResult res;
    if (patch_bytes.empty()) {
        res.message = L"empty patch";
        return res;
    }
    const std::wstring patch_file = GitWriteTempFile(patch_bytes, L"hunk.patch");
    if (patch_file.empty()) {
        res.message = L"could not write patch file";
        return res;
    }
    std::wstring args = L"apply --recount --whitespace=nowarn ";
    if (cached) args += L"--cached ";
    if (reverse) args += L"-R ";
    args += GitQuoteArg(patch_file);
    res = FromRun(RunGit(root, args, timeout_ms, true));
    DeleteFileW(patch_file.c_str());
    return res;
}

bool ParseGitBlamePorcelain(std::string_view out, std::vector<GitBlameLine>& lines) {
    lines.clear();
    if (out.empty()) return false;
    struct Meta {
        std::wstring author;
        int64_t ts = 0;
        std::wstring date_text;
    };
    std::unordered_map<std::string, Meta> meta_by_hash;
    std::string cur_hash;
    Meta cur_meta;
    size_t pos = 0;
    while (pos < out.size()) {
        size_t nl = out.find('\n', pos);
        const size_t end = (nl == std::string_view::npos) ? out.size() : nl;
        std::string_view line = out.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        pos = (nl == std::string_view::npos) ? out.size() : nl + 1;

        if (!line.empty() && line[0] == '\t') {
            if (!cur_hash.empty()) {
                auto& saved = meta_by_hash[cur_hash];
                if (!cur_meta.author.empty()) saved.author = cur_meta.author;
                if (cur_meta.ts > 0) {
                    saved.ts = cur_meta.ts;
                    saved.date_text = cur_meta.date_text;
                }
                GitBlameLine bl;
                bl.hash = GitUtf8ToWide(cur_hash.substr(0, (std::min<size_t>)(8, cur_hash.size())));
                bl.uncommitted = (cur_hash == "0000000000000000000000000000000000000000");
                bl.author = saved.author;
                bl.timestamp = saved.ts;
                bl.date_text = saved.date_text;
                lines.push_back(std::move(bl));
                cur_hash.clear();
                cur_meta = Meta{};
            }
            continue;
        }
        if (cur_hash.empty()) {
            const size_t sp = line.find(' ');
            if (sp == 40) {
                cur_hash.assign(line.substr(0, 40));
                if (const auto it = meta_by_hash.find(cur_hash); it != meta_by_hash.end()) {
                    cur_meta = it->second;
                } else {
                    cur_meta = Meta{};
                }
            }
            continue;
        }
        if (line.starts_with("author ")) {
            cur_meta.author = GitUtf8ToWide(line.substr(7));
        } else if (line.starts_with("author-time ")) {
            cur_meta.ts = _strtoi64(std::string(line.substr(12)).c_str(), nullptr, 10);
            cur_meta.date_text = FormatBlameDate(cur_meta.ts);
        }
    }
    return !lines.empty();
}

bool GitReadBlame(const std::wstring& root, const std::wstring& path,
                  std::vector<GitBlameLine>& lines, DWORD timeout_ms) {
    lines.clear();
    const std::wstring args =
        L"-c core.quotepath=false --literal-pathspecs blame --porcelain -- " + GitQuoteArg(path);
    const GitRunResult run = RunGit(root, args, timeout_ms, false);
    if (!run.started || run.timed_out || run.exit_code != 0) return false;
    return ParseGitBlamePorcelain(run.out, lines);
}

GitFileSummary GitReadFileSummary(const std::wstring& root, const std::wstring& rel_path) {
    GitFileSummary s;
    std::wstring path = rel_path;
    std::replace(path.begin(), path.end(), L'\\', L'/');
    if (path.empty()) return s;

    const std::wstring qpath = GitQuoteArg(path);
    const GitRunResult st_run = RunGit(
        root, L"-c core.quotepath=false --literal-pathspecs status --porcelain=v2 --branch -z --untracked-files=all -- " + qpath,
        10000, false);
    if (!st_run.started || st_run.timed_out || st_run.exit_code != 0) return s;

    GitChangeSet cs;
    if (!ParseGitChanges(st_run.out, cs)) return s;
    s.valid = true;
    s.staged = !cs.staged.empty();
    s.unstaged = !cs.unstaged.empty();
    s.untracked = !cs.unstaged.empty() && cs.unstaged[0].kind == GitChangeKind::Untracked;
    s.is_binary = GitIsImagePath(path);

    // Numstat + preview diff against HEAD when HEAD exists, else against index.
    if (!s.untracked && !s.is_binary) {
        const std::wstring base = cs.HasHead() ? L"HEAD " : L"--cached ";
        const GitRunResult ns = RunGit(
            root, L"-c core.quotepath=false --literal-pathspecs diff --numstat " + base + L"-- " + qpath,
            10000, false);
        if (ns.started && !ns.timed_out && ns.exit_code == 0 && !ns.out.empty()) {
            const std::string_view sv(ns.out);
            const size_t tab1 = sv.find('\t');
            if (tab1 != std::string_view::npos) {
                const size_t tab2 = sv.find('\t', tab1 + 1);
                if (tab2 != std::string_view::npos) {
                    const auto a = sv.substr(0, tab1);
                    const auto d = sv.substr(tab1 + 1, tab2 - tab1 - 1);
                    if (a == "-" || d == "-") {
                        s.is_binary = true;
                    } else {
                        s.added = ParseCount(a);
                        s.deleted = ParseCount(d);
                    }
                }
            }
        }
        if (!s.is_binary) {
            const GitRunResult dr = RunGit(
                root, L"-c core.quotepath=false --literal-pathspecs diff -U2 --no-color --no-ext-diff " + base + L"-- " + qpath,
                10000, false);
            if (dr.started && !dr.timed_out && dr.exit_code == 0) {
                const auto lines = SplitByteLines(dr.out);
                int old_no = 0, new_no = 0;
                bool in_first_hunk = false;
                for (const auto& [b, e] : lines) {
                    const std::string_view line = TrimCrLf(std::string_view(dr.out).substr(b, e - b));
                    if (line.starts_with("@@ ")) {
                        if (in_first_hunk) break;
                        int oc = 0, nc = 0;
                        if (ParseHunkRange(line, old_no, oc, new_no, nc)) {
                            in_first_hunk = true;
                            s.first_hunk_line = new_no > 0 ? new_no : old_no;
                        }
                        continue;
                    }
                    if (!in_first_hunk || line.empty()) continue;
                    if (line[0] == '\\') continue;
                    if (s.preview_lines.size() >= 6) break;
                    GitFileMiniDiffLine ml;
                    if (line[0] == '+') {
                        ml.sign = L'+';
                        ml.line_no = new_no++;
                    } else if (line[0] == '-') {
                        ml.sign = L'-';
                        ml.line_no = old_no++;
                    } else {
                        ml.sign = L' ';
                        ml.line_no = new_no++;
                        ++old_no;
                    }
                    ml.text = GitUtf8ToWide(line.substr(1));
                    s.preview_lines.push_back(std::move(ml));
                }
            }
        }
    }

    if (cs.HasHead()) {
        const GitRunResult lr = RunGit(
            root, L"-c core.quotepath=false --literal-pathspecs log -n 3 --format=%h%x1f%an%x1f%at%x1f%s -- " + qpath,
            10000, false);
        if (lr.started && !lr.timed_out && lr.exit_code == 0) {
            const auto lines = SplitByteLines(lr.out);
            for (const auto& [b, e] : lines) {
                const std::string_view line = TrimCrLf(std::string_view(lr.out).substr(b, e - b));
                if (line.empty()) continue;
                std::vector<std::string_view> parts;
                size_t p = 0;
                for (int k = 0; k < 3; ++k) {
                    const size_t sep = line.find('\x1f', p);
                    if (sep == std::string_view::npos) break;
                    parts.push_back(line.substr(p, sep - p));
                    p = sep + 1;
                }
                parts.push_back(line.substr(p));
                if (parts.size() == 4) {
                    GitFileHistoryMini hm;
                    hm.hash = GitUtf8ToWide(parts[0]);
                    hm.author = GitUtf8ToWide(parts[1]);
                    const int64_t ts = _strtoi64(std::string(parts[2]).c_str(), nullptr, 10);
                    hm.time_text = GitFormatShortTime(ts);
                    hm.subject = GitUtf8ToWide(parts[3]);
                    s.history.push_back(std::move(hm));
                }
            }
        }
    }
    return s;
}

}  // namespace pulse::app

#include "git_status.h"

#include <algorithm>
#include <vector>

namespace pulse::app {
namespace {

using ui::GitMark;

int Rank(GitMark mark) {
    switch (mark) {
    case GitMark::Locked: return 1;   // SVN: weaker than any change
    case GitMark::Added: return 2;
    case GitMark::Renamed: return 3;
    case GitMark::Modified:
    case GitMark::Deleted: return 4;
    case GitMark::Conflict: return 5;
    default: return 0;
    }
}

GitMark MarkXY(char x, char y) {
    if (x == 'U' || y == 'U') return GitMark::Conflict;
    if (x == 'D' || y == 'D') return GitMark::Deleted;
    if (x == 'A') return GitMark::Added;
    if (x == 'R' || x == 'C') return GitMark::Renamed;
    if (x == 'M' || y == 'M' || x == 'T' || y == 'T') return GitMark::Modified;
    return GitMark::None;
}

// The text after the first `skip` spaces (the path field of a status record).
std::string_view AfterFields(std::string_view rec, int skip) {
    size_t at = 0;
    for (int i = 0; i < skip; ++i) {
        at = rec.find(' ', at);
        if (at == std::string_view::npos) return {};
        ++at;
    }
    return rec.substr(at);
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

std::wstring FindGitExe() {
    wchar_t found[MAX_PATH]{};
    if (SearchPathW(nullptr, L"git.exe", nullptr, MAX_PATH, found, nullptr)) return found;
    const wchar_t* roots[] = { L"ProgramFiles", L"ProgramW6432", L"LOCALAPPDATA" };
    for (const wchar_t* var : roots) {
        wchar_t base[MAX_PATH]{};
        if (!GetEnvironmentVariableW(var, base, MAX_PATH)) continue;
        std::wstring path = base;
        path += wcscmp(var, L"LOCALAPPDATA") == 0 ? L"\\Programs\\Git\\cmd\\git.exe" : L"\\Git\\cmd\\git.exe";
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return path;
    }
    return {};
}

// The current environment plus the variables that keep git from waiting on a
// console nobody can see.
std::vector<wchar_t> GitEnvironment() {
    std::vector<wchar_t> block;
    if (wchar_t* env = GetEnvironmentStringsW()) {
        const wchar_t* p = env;
        while (*p) {
            const size_t len = wcslen(p);
            const std::wstring_view entry(p, len);
            if (!entry.starts_with(L"GIT_TERMINAL_PROMPT=")) block.insert(block.end(), p, p + len + 1);
            p += len + 1;
        }
        FreeEnvironmentStringsW(env);
    }
    const std::wstring_view add = L"GIT_TERMINAL_PROMPT=0";
    block.insert(block.end(), add.begin(), add.end());
    block.push_back(L'\0');
    block.push_back(L'\0');
    return block;
}

struct Cache {
    std::unordered_map<std::wstring, std::shared_ptr<const GitRepoStatus>> by_root;
    uint64_t revision = 0;
};
Cache& TheCache() {
    static Cache cache;
    return cache;
}

}  // namespace

GitMark StrongerGitMark(GitMark a, GitMark b) { return Rank(b) > Rank(a) ? b : a; }

std::wstring GitLowerPath(std::wstring path) {
    std::replace(path.begin(), path.end(), L'/', L'\\');
    while (!path.empty() && path.back() == L'\\') path.pop_back();
    if (!path.empty()) {
        const int src_len = static_cast<int>(path.size());
        const int dst_len = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, path.data(), src_len,
                                          nullptr, 0, nullptr, nullptr, 0);
        if (dst_len > 0) {
            std::wstring lower(static_cast<size_t>(dst_len), L'\0');
            if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, path.data(), src_len,
                              lower.data(), dst_len, nullptr, nullptr, 0) > 0) {
                return lower;
            }
        }
    }
    return path;
}

std::wstring GitUtf8ToWide(std::string_view text) {
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>((std::max)(n, 0)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}

bool ParseGitStatusV2(std::string_view out, GitRepoStatus& st) {
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
    std::string_view rec;
    while (next(rec)) {
        if (rec.empty()) continue;
        if (rec[0] == '#') {
            header = true;
            auto value = [&](std::string_view key, std::string_view& v) {
                if (!rec.starts_with(key)) return false;
                v = rec.substr(key.size());
                return true;
            };
            std::string_view v;
            if (value("# branch.oid ", v)) {
                if (v != "(initial)") st.head = GitUtf8ToWide(v.substr(0, 7));
            } else if (value("# branch.head ", v)) {
                if (v != "(detached)") st.branch = GitUtf8ToWide(v);
            } else if (value("# branch.upstream ", v)) {
                st.upstream = GitUtf8ToWide(v);
            } else if (value("# branch.ab ", v)) {
                const size_t plus = v.find('+'), minus = v.find('-');
                if (plus != std::string_view::npos) st.ahead = ParseCount(v.substr(plus + 1));
                if (minus != std::string_view::npos) st.behind = ParseCount(v.substr(minus + 1));
            }
            continue;
        }
        GitMark mark = GitMark::None;
        std::string_view path;
        bool untracked_dir = false;
        const char type = rec[0];
        if (type == '1' && rec.size() > 4) {
            mark = MarkXY(rec[2], rec[3]);
            path = AfterFields(rec, 8);
        } else if (type == '2' && rec.size() > 4) {
            mark = rec[2] == 'C' ? GitMark::Added : GitMark::Renamed;
            path = AfterFields(rec, 9);
            std::string_view original;
            next(original);  // -z puts the source path in its own record
        } else if (type == 'u') {
            mark = GitMark::Conflict;
            path = AfterFields(rec, 10);
        } else if (type == '?' && rec.size() > 2) {
            mark = GitMark::Added;
            path = rec.substr(2);
            if (path.ends_with('/')) untracked_dir = true;
        } else {
            continue;  // '!' (ignored) and anything newer
        }
        if (path.empty() || mark == GitMark::None) continue;
        std::wstring rel = GitLowerPath(GitUtf8ToWide(path));
        if (rel.empty()) continue;
        ++st.changed;
        if (untracked_dir) {
            st.untracked_dirs.insert(rel);
            st.dirs[rel] = StrongerGitMark(st.dirs[rel], GitMark::Added);
            ++st.dir_counts[rel];
        } else {
            st.files[rel] = mark;
        }
        // A folder reads as changed when anything under it is; a deleted or
        // renamed child leaves it modified.
        const GitMark up = mark == GitMark::Deleted || mark == GitMark::Renamed ? GitMark::Modified : mark;
        for (size_t slash = rel.rfind(L'\\'); slash != std::wstring::npos; slash = rel.rfind(L'\\')) {
            rel.resize(slash);
            GitMark& dir = st.dirs[rel];
            dir = StrongerGitMark(dir, up);
            ++st.dir_counts[rel];
        }
    }
    return header;
}

GitMark GitMarkFor(const GitRepoStatus& st, const std::wstring& rel, bool is_dir) {
    if (is_dir) {
        if (const auto it = st.dirs.find(rel); it != st.dirs.end()) return it->second;
    }
    if (const auto it = st.files.find(rel); it != st.files.end()) return it->second;  // files, submodules
    if (!st.untracked_dirs.empty()) {
        std::wstring up = rel;
        for (size_t slash = up.rfind(L'\\'); slash != std::wstring::npos; slash = up.rfind(L'\\')) {
            up.resize(slash);
            if (st.untracked_dirs.contains(up)) return GitMark::Added;
        }
    }
    return GitMark::None;
}

int GitDirChangeCount(const GitRepoStatus& st, const std::wstring& rel) {
    if (const auto it = st.dir_counts.find(rel); it != st.dir_counts.end()) return it->second;
    if (!st.untracked_dirs.empty()) {
        std::wstring up = rel;
        if (st.untracked_dirs.contains(up)) return 1;
        for (size_t slash = up.rfind(L'\\'); slash != std::wstring::npos; slash = up.rfind(L'\\')) {
            up.resize(slash);
            if (st.untracked_dirs.contains(up)) return 1;
        }
    }
    return 0;
}

std::wstring GitBranchLabel(const GitRepoStatus& st) {
    std::wstring label = !st.branch.empty() ? st.branch : !st.head.empty() ? st.head : std::wstring(L"HEAD");
    if (st.ahead > 0) label += L" \u2191" + std::to_wstring(st.ahead);
    if (st.behind > 0) label += L" \u2193" + std::to_wstring(st.behind);
    return label;
}

std::wstring GitBadgeText(const GitRepoStatus& st) {
    std::wstring label = !st.branch.empty() ? st.branch : !st.head.empty() ? st.head : std::wstring(L"HEAD");
    if (label.size() > 18) label = label.substr(0, 17) + L"\u2026";
    if (st.ahead > 0) label += L" \u2191" + std::to_wstring(st.ahead);
    if (st.behind > 0) label += L" \u2193" + std::to_wstring(st.behind);
    return label;
}

GitRunResult RunGit(const std::wstring& cwd, const std::wstring& args, DWORD timeout_ms, bool merge_stderr) {
    GitRunResult result;
    static const std::wstring exe = FindGitExe();
    if (exe.empty()) return result;

    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE read_end = nullptr, write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) return result;
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = write_end;
    si.hStdError = merge_stderr ? write_end : nul;
    std::wstring command = L"\"" + exe + L"\" " + args;
    std::vector<wchar_t> env = GitEnvironment();
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, env.data(),
                                   cwd.c_str(), &si, &pi);
    CloseHandle(write_end);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(read_end);
        return result;
    }
    result.started = true;
    const ULONGLONG start = GetTickCount64();
    char buffer[16384];
    auto drain = [&] {
        DWORD avail = 0;
        // A noisy git/helper must yield to the process-exit and timeout checks.
        // Keep all output, but bound the work performed by each polling pass.
        constexpr unsigned kMaxReadsPerPass = 16;
        for (unsigned reads = 0; reads < kMaxReadsPerPass &&
             PeekNamedPipe(read_end, nullptr, 0, nullptr, &avail, nullptr) && avail; ++reads) {
            DWORD got = 0;
            if (!ReadFile(read_end, buffer, (std::min)(avail, static_cast<DWORD>(sizeof(buffer))), &got, nullptr) || !got)
                return;
            result.out.append(buffer, got);
        }
    };
    for (;;) {
        drain();
        // Credential helpers can inherit the pipe and outlive git: stop at
        // git's own exit, not at the pipe's end.
        if (WaitForSingleObject(pi.hProcess, 15) == WAIT_OBJECT_0) {
            drain();
            break;
        }
        if (GetTickCount64() - start > timeout_ms) {
            TerminateProcess(pi.hProcess, 1);
            result.timed_out = true;
            break;
        }
    }
    GetExitCodeProcess(pi.hProcess, &result.exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(read_end);
    return result;
}

std::shared_ptr<const GitRepoStatus> GitStatusFor(const std::wstring& root) {
    if (root.empty()) return nullptr;
    const auto& map = TheCache().by_root;
    const auto it = map.find(GitLowerPath(root));
    return it == map.end() ? nullptr : it->second;
}

void StoreGitStatus(std::shared_ptr<const GitRepoStatus> status) {
    if (!status) return;
    Cache& cache = TheCache();
    cache.by_root[GitLowerPath(status->root)] = std::move(status);
    ++cache.revision;
}

uint64_t GitStatusRevision() { return TheCache().revision; }

}  // namespace pulse::app

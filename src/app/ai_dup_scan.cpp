// find_duplicates: the local three-stage compare and the keep rules.
#include "ai_dup_scan.h"

#include <shlobj.h>
#include <knownfolders.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cwctype>
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace pulse::app::ai {
namespace {

constexpr uint64_t kHead = 64 * 1024;
constexpr uint64_t kMaxFiles = 300000;

uint64_t Ticks(const FILETIME& ft) { return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime; }

uint32_t MsSince(std::chrono::steady_clock::time_point t0) {
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
}

std::wstring Lower(std::wstring s) {
    for (auto& ch : s) ch = static_cast<wchar_t>(std::towlower(ch));
    return s;
}

std::wstring Trim(std::wstring p) {
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    std::replace(p.begin(), p.end(), L'/', L'\\');
    return p;
}

std::wstring Known(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &p)) && p) out = p;
    CoTaskMemFree(p);
    return Trim(out);
}

std::wstring Override() {
    wchar_t over[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"PULSE_AI_SPACE_ROOT", over, MAX_PATH);
    return n && n < MAX_PATH ? Trim(over) : std::wstring();
}

// Known names the model may use, with their folder and fixture subfolder.
struct KnownName {
    const wchar_t* key;
    const wchar_t* alias;
    const wchar_t* alias2;
    const KNOWNFOLDERID* id;
    const wchar_t* sub;
};
const KnownName kKnown[] = {
    {L"downloads", L"\x4E0B\x8F7D", L"download", &FOLDERID_Downloads, L"Downloads"},
    {L"pictures", L"\x56FE\x7247", L"photos", &FOLDERID_Pictures, L"Pictures"},
    {L"desktop", L"\x684C\x9762", L"desk", &FOLDERID_Desktop, L"Desktop"},
    {L"documents", L"\x6587\x6863", L"docs", &FOLDERID_Documents, L"Documents"},
    {L"videos", L"\x89C6\x9891", L"video", &FOLDERID_Videos, L"Videos"},
    {L"music", L"\x97F3\x4E50", L"audio", &FOLDERID_Music, L"Music"},
};

std::wstring KnownPath(const KnownName& k) {
    const std::wstring over = Override();
    return over.empty() ? Known(*k.id) : over + L"\\" + k.sub;
}

bool Under(const std::wstring& path_lower, const std::wstring& root_lower) {
    if (root_lower.empty() || path_lower.size() < root_lower.size()) return false;
    if (path_lower.compare(0, root_lower.size(), root_lower) != 0) return false;
    return path_lower.size() == root_lower.size() || path_lower[root_lower.size()] == L'\\' ||
           root_lower.back() == L'\\';
}

bool SkipDir(const std::wstring& name) {
    static const wchar_t* const kSkip[] = {L".git", L"node_modules", L"__pycache__", L".venv", L"$recycle.bin",
                                           L"system volume information", L"appdata", L"windows", L"program files",
                                           L"program files (x86)", L"programdata", L".svn", L".hg"};
    const std::wstring l = Lower(name);
    for (const wchar_t* k : kSkip)
        if (l == k) return true;
    return false;
}

bool CleanName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L'\\');
    std::wstring stem = Lower(path.substr(slash == std::wstring::npos ? 0 : slash + 1));
    const size_t dot = stem.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) stem.resize(dot);
    while (!stem.empty() && stem.back() == L' ') stem.pop_back();
    // "name (2)" / "name(2)"
    if (!stem.empty() && stem.back() == L')') {
        const size_t open = stem.find_last_of(L'(');
        if (open != std::wstring::npos && open + 2 < stem.size()) {
            bool digits = true;
            for (size_t i = open + 1; i + 1 < stem.size(); ++i)
                if (!std::iswdigit(stem[i])) digits = false;
            if (digits) return false;
        }
    }
    static const wchar_t* const kCopy[] = {L"\x526F\x672C", L" - copy", L"- copy", L"_copy", L"copy of ", L" copy"};
    for (const wchar_t* k : kCopy)
        if (stem.find(k) != std::wstring::npos) return false;
    return true;
}

std::wstring Parent(const std::wstring& path) {
    const size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

HANDLE OpenRead(const std::wstring& path) {
    const std::wstring p = path.size() >= 3 && path[1] == L':' ? L"\\\\?\\" + path : path;
    return CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                       OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
}

uint64_t Fnv(const unsigned char* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

bool HeadHash(const std::wstring& path, uint64_t bytes, uint64_t& out) {
    HANDLE h = OpenRead(path);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::vector<unsigned char> buf(static_cast<size_t>((std::min)(bytes, kHead)));
    DWORD got = 0;
    const bool ok = buf.empty() || (ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) &&
                                    got == buf.size());
    CloseHandle(h);
    if (!ok) return false;
    out = Fnv(buf.data(), buf.size());
    return true;
}

// Byte-for-byte; `read` adds what both reads cost.
bool SameBytes(const std::wstring& a, const std::wstring& b, const std::atomic<bool>& cancel, uint64_t& read) {
    HANDLE ha = OpenRead(a);
    if (ha == INVALID_HANDLE_VALUE) return false;
    HANDLE hb = OpenRead(b);
    if (hb == INVALID_HANDLE_VALUE) {
        CloseHandle(ha);
        return false;
    }
    constexpr DWORD kChunk = 256 * 1024;
    auto ba = std::make_unique<unsigned char[]>(kChunk);
    auto bb = std::make_unique<unsigned char[]>(kChunk);
    bool same = true;
    for (;;) {
        if (cancel.load()) {
            same = false;
            break;
        }
        DWORD ga = 0, gb = 0;
        if (!ReadFile(ha, ba.get(), kChunk, &ga, nullptr) || !ReadFile(hb, bb.get(), kChunk, &gb, nullptr) || ga != gb) {
            same = false;
            break;
        }
        read += static_cast<uint64_t>(ga) * 2;
        if (!ga) break;
        if (memcmp(ba.get(), bb.get(), ga) != 0) {
            same = false;
            break;
        }
    }
    CloseHandle(ha);
    CloseHandle(hb);
    return same;
}

struct Seen {
    std::wstring path;
    uint64_t bytes = 0;
    uint64_t mtime = 0;
    size_t root = 0;
};

void Walk(const std::wstring& dir, size_t root, uint64_t min_bytes, std::vector<Seen>& out,
          std::unordered_set<std::wstring>& seen_dirs, DupProgress& progress, const std::atomic<bool>& cancel,
          bool& truncated, uint64_t& online) {
    std::vector<std::wstring> stack{dir};
    while (!stack.empty()) {
        if (cancel.load() || truncated) return;
        const std::wstring d = std::move(stack.back());
        stack.pop_back();
        if (!seen_dirs.insert(Lower(d)).second) continue;
        WIN32_FIND_DATAW fd{};
        const std::wstring pattern = (d.size() >= 3 && d[1] == L':' ? L"\\\\?\\" + d : d) + L"\\*";
        HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                    FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            const DWORD a = fd.dwFileAttributes;
            if (a & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            if ((a & FILE_ATTRIBUTE_HIDDEN) && (a & FILE_ATTRIBUTE_SYSTEM)) continue;
            const std::wstring full = d + L"\\" + name;
            if (a & FILE_ATTRIBUTE_DIRECTORY) {
                if (!SkipDir(name)) stack.push_back(full);
                continue;
            }
            // Cloud placeholders would download to be read.
            if (a & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN)) {
                ++online;
                continue;
            }
            const uint64_t bytes = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            progress.files.fetch_add(1);
            progress.bytes.fetch_add(bytes);
            if (bytes < min_bytes || bytes == 0) continue;
            out.push_back({full, bytes, Ticks(fd.ftLastWriteTime), root});
            if (out.size() >= kMaxFiles) {
                truncated = true;
                break;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

}  // namespace

std::vector<std::wstring> MessyFolders() {
    std::vector<std::wstring> out;
    for (const KnownName& k : kKnown)
        if (wcscmp(k.key, L"downloads") == 0 || wcscmp(k.key, L"desktop") == 0) {
            const std::wstring p = KnownPath(k);
            if (!p.empty()) out.push_back(p);
        }
    return out;
}

bool ResolveDupRoots(const std::vector<std::wstring>& names, const std::wstring& current,
                     std::vector<DupRoot>& out, std::string& error) {
    out.clear();
    std::vector<std::wstring> asked = names;
    if (asked.empty() && !current.empty()) asked.push_back(current);
    if (asked.empty()) {
        error = "no folder given and no folder is open";
        return false;
    }
    const std::wstring forbidden[] = {Known(FOLDERID_Windows), Known(FOLDERID_ProgramFiles),
                                      Known(FOLDERID_ProgramFilesX86), Known(FOLDERID_ProgramData)};
    for (const std::wstring& raw : asked) {
        std::wstring want = raw;
        while (!want.empty() && std::iswspace(want.back())) want.pop_back();
        while (!want.empty() && std::iswspace(want.front())) want.erase(want.begin());
        DupRoot r;
        const std::wstring l = Lower(want);
        for (const KnownName& k : kKnown)
            if (l == k.key || l == k.alias || l == k.alias2) {
                r.key = k.key;
                r.path = KnownPath(k);
            }
        if (r.key.empty()) r.path = Trim(want);
        const DWORD attrs = r.path.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(r.path.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            error = "folder not found: ";
            for (const wchar_t ch : raw) error += ch >= 0x20 && ch < 0x7F ? static_cast<char>(ch) : '?';
            return false;
        }
        const std::wstring pl = Lower(r.path);
        for (const std::wstring& f : forbidden)
            if (!f.empty() && Under(pl, Lower(f))) {
                error = "system and program folders are never compared";
                return false;
            }
        out.push_back(std::move(r));
    }
    // A folder inside another one is already covered.
    std::vector<DupRoot> kept;
    for (size_t i = 0; i < out.size(); ++i) {
        bool inner = false;
        for (size_t j = 0; j < out.size() && !inner; ++j) {
            if (i == j) continue;
            const std::wstring a = Lower(out[i].path), b = Lower(out[j].path);
            if (a == b ? j < i : Under(a, b)) inner = true;
        }
        if (!inner) kept.push_back(out[i]);
    }
    out = std::move(kept);
    return !out.empty();
}

size_t DupGroup::Kept() const {
    size_t n = 0;
    for (const DupFile& f : files) n += f.keep ? 1 : 0;
    return n;
}

uint64_t DupGroup::ExtraBytes() const {
    uint64_t n = 0;
    for (const DupFile& f : files)
        if (!f.keep) n += f.bytes;
    return n;
}

void ApplyKeepRules(DupGroup& g, bool newest) {
    if (g.picked || g.files.empty()) return;
    const auto better = [newest](const DupFile& a, const DupFile& b) {
        if (a.organized != b.organized) return a.organized;
        if (a.clean != b.clean) return a.clean;
        if (a.mtime != b.mtime) return newest ? a.mtime > b.mtime : a.mtime < b.mtime;
        if (a.path.size() != b.path.size()) return a.path.size() < b.path.size();
        return _wcsicmp(a.path.c_str(), b.path.c_str()) < 0;
    };
    // Organised copies in two or more folders (项目A / 项目B): each folder
    // keeps its best one; only the loose copies go.
    std::map<std::wstring, size_t> best_in;
    for (size_t i = 0; i < g.files.size(); ++i) {
        if (!g.files[i].organized) continue;
        const std::wstring p = Lower(Parent(g.files[i].path));
        const auto it = best_in.find(p);
        if (it == best_in.end() || better(g.files[i], g.files[it->second])) best_in[p] = i;
    }
    for (DupFile& f : g.files) f.keep = false;
    if (best_in.size() >= 2) {
        g.both = true;
        g.why = DupWhy::Both;
        for (const auto& [p, i] : best_in) g.files[i].keep = true;
        return;
    }
    g.both = false;
    size_t best = 0;
    for (size_t i = 1; i < g.files.size(); ++i)
        if (better(g.files[i], g.files[best])) best = i;
    g.files[best].keep = true;
    const DupFile& k = g.files[best];
    bool org = false, clean = false;
    for (const DupFile& f : g.files) {
        if (&f == &k) continue;
        if (f.organized != k.organized) org = true;
        if (f.clean != k.clean) clean = true;
    }
    g.why = org ? DupWhy::Organized : clean ? DupWhy::CleanName : newest ? DupWhy::Newest : DupWhy::Earliest;
}

void PickKeeper(DupGroup& g, size_t file) {
    if (file >= g.files.size()) return;
    for (size_t i = 0; i < g.files.size(); ++i) g.files[i].keep = i == file;
    g.picked = true;
    g.both = false;
    g.why = DupWhy::Picked;
}

void FindDuplicates(DupScan& scan, uint64_t min_bytes, DupProgress& progress, const std::atomic<bool>& cancel) {
    using clock = std::chrono::steady_clock;
    // Stage 1: every file, bucketed by size.
    auto t0 = clock::now();
    std::vector<Seen> all;
    std::unordered_set<std::wstring> seen_dirs;
    for (size_t r = 0; r < scan.roots.size(); ++r)
        Walk(scan.roots[r].path, r, min_bytes, all, seen_dirs, progress, cancel, scan.truncated,
             scan.online_skipped);
    scan.total_files = progress.files.load();
    std::unordered_map<uint64_t, std::vector<size_t>> by_size;
    for (size_t i = 0; i < all.size(); ++i) by_size[all[i].bytes].push_back(i);
    std::vector<std::vector<size_t>> buckets;
    for (auto& [size, list] : by_size)
        if (list.size() >= 2) {
            scan.size_matches += list.size();
            buckets.push_back(std::move(list));
        }
    scan.ms[0] = MsSince(t0);
    if (cancel.load()) return;

    // Stage 2: the first 64 KB.
    progress.stage.store(1);
    progress.done.store(0);
    progress.todo.store(scan.size_matches);
    t0 = clock::now();
    std::vector<std::vector<size_t>> heads;
    for (const auto& bucket : buckets) {
        std::unordered_map<uint64_t, std::vector<size_t>> by_head;
        for (size_t i : bucket) {
            if (cancel.load()) return;
            uint64_t h = 0;
            if (HeadHash(all[i].path, all[i].bytes, h)) by_head[h].push_back(i);
            progress.done.fetch_add(1);
        }
        for (auto& [h, list] : by_head)
            if (list.size() >= 2) {
                scan.head_matches += list.size();
                heads.push_back(std::move(list));
            }
    }
    scan.ms[1] = MsSince(t0);

    // Stage 3: whole files, byte by byte against each cluster's first file.
    progress.stage.store(2);
    progress.done.store(0);
    progress.todo.store(scan.head_matches);
    t0 = clock::now();
    const std::vector<std::wstring> messy = MessyFolders();
    std::vector<std::wstring> messy_l;
    for (const auto& m : messy) messy_l.push_back(Lower(m));
    for (const auto& list : heads) {
        std::vector<std::vector<size_t>> clusters;
        for (size_t i : list) {
            if (cancel.load()) return;
            bool placed = false;
            for (auto& cl : clusters)
                if (SameBytes(all[cl[0]].path, all[i].path, cancel, scan.read_bytes)) {
                    cl.push_back(i);
                    placed = true;
                    break;
                }
            if (!placed) clusters.push_back({i});
            progress.done.fetch_add(1);
        }
        for (const auto& cl : clusters) {
            if (cl.size() < 2) continue;
            DupGroup g;
            for (size_t i : cl) {
                DupFile f;
                f.path = all[i].path;
                f.bytes = all[i].bytes;
                f.mtime = all[i].mtime;
                f.root = all[i].root;
                // Loose: right in Downloads / Desktop; their subfolders (下载\工作\2026 Q3)
                // were made by someone sorting (task 46 rule ①).
                const std::wstring parent_l = Lower(Parent(f.path));
                bool loose = false;
                for (const auto& m : messy_l)
                    if (parent_l == m) loose = true;
                f.organized = !loose && _wcsicmp(Parent(f.path).c_str(), scan.roots[f.root].path.c_str()) != 0;
                f.clean = CleanName(f.path);
                g.files.push_back(std::move(f));
            }
            std::sort(g.files.begin(), g.files.end(),
                      [](const DupFile& a, const DupFile& b) { return _wcsicmp(a.path.c_str(), b.path.c_str()) < 0; });
            ApplyKeepRules(g, false);
            scan.dup_files += g.files.size();
            scan.groups.push_back(std::move(g));
        }
    }
    std::stable_sort(scan.groups.begin(), scan.groups.end(), [](const DupGroup& a, const DupGroup& b) {
        const uint64_t wa = a.files.empty() ? 0 : a.files[0].bytes * (a.files.size() - 1);
        const uint64_t wb = b.files.empty() ? 0 : b.files[0].bytes * (b.files.size() - 1);
        return wa > wb;
    });
    scan.ms[2] = MsSince(t0);
    progress.stage.store(3);
}

bool DupStillSame(const DupFile& keeper, const DupFile& extra, const std::atomic<bool>& cancel) {
    const auto unchanged = [](const DupFile& f) {
        WIN32_FILE_ATTRIBUTE_DATA d{};
        const std::wstring p = f.path.size() >= 3 && f.path[1] == L':' ? L"\\\\?\\" + f.path : f.path;
        if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return false;
        const uint64_t bytes = (static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
        return !(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && bytes == f.bytes && Ticks(d.ftLastWriteTime) == f.mtime;
    };
    if (!unchanged(keeper) || !unchanged(extra)) return false;
    uint64_t read = 0;
    return SameBytes(keeper.path, extra.path, cancel, read);
}

std::wstring DupDirLabel(const DupScan& scan, const DupFile& f, const std::wstring& root_label) {
    std::wstring out = root_label + L" \u203A ";
    if (f.root >= scan.roots.size()) return out;
    const std::wstring& root = scan.roots[f.root].path;
    const std::wstring parent = Parent(f.path);
    if (parent.size() <= root.size()) return out;
    std::wstring rest = parent.substr(root.size());
    while (!rest.empty() && rest.front() == L'\\') rest.erase(rest.begin());
    size_t start = 0;
    while (start < rest.size()) {
        const size_t slash = rest.find(L'\\', start);
        out += rest.substr(start, slash == std::wstring::npos ? std::wstring::npos : slash - start) + L" \u203A ";
        if (slash == std::wstring::npos) break;
        start = slash + 1;
    }
    return out;
}

}  // namespace pulse::app::ai

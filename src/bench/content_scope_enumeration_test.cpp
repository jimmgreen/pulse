#include "../index/content_task_search.h"
#include "../index/content_scope.h"
#include <winioctl.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <set>

using namespace pulse::index;
namespace {
std::mutex listing_mutex;
std::set<std::wstring> denied;
std::vector<std::wstring> opened;
std::set<HANDLE> handles;
std::atomic<bool>* cancel_on_open = nullptr;
int failures = 0;
std::wstring Key(std::wstring path) {
    if (path.starts_with(L"\\\\?\\")) path.erase(0, 4);
    return ContentScopeKey(std::move(path));
}
static bool MakeJunction(const std::filesystem::path& path, const std::filesystem::path& target) {
    if (!CreateDirectoryW(path.c_str(), nullptr)) return false;
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    struct MountPoint {
        DWORD tag;
        WORD length, reserved, substitute_offset, substitute_length, print_offset, print_length;
        wchar_t path[2048];
    } data{};
    const std::wstring target_name = L"\\??\\" + target.wstring();
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    data.substitute_length = static_cast<WORD>(target_name.size() * sizeof(wchar_t));
    data.print_offset = static_cast<WORD>(data.substitute_length + sizeof(wchar_t));
    data.length = static_cast<WORD>(8 + data.print_offset + sizeof(wchar_t));
    memcpy(data.path, target_name.c_str(), data.substitute_length + sizeof(wchar_t));
    DWORD bytes = 0;
    const bool ok = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &data,
        static_cast<DWORD>(data.length) + 8, nullptr, 0, &bytes, nullptr) != FALSE;
    CloseHandle(handle);
    return ok;
}

void Check(bool value, const char* text) {
    std::cout << (value ? "[PASS] " : "[FAIL] ") << text << '\n';
    if (!value) ++failures;
}
}
namespace pulse::index::content_listing {
HANDLE First(const std::wstring& query, WIN32_FIND_DATAW& data, DWORD flags) {
    const auto dir = Key(query.substr(0, query.size() - 2));
    std::lock_guard lock(listing_mutex);
    opened.push_back(dir);
    if (cancel_on_open) cancel_on_open->store(true);
    if (denied.contains(dir)) { SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE; }
    HANDLE handle = FindFirstFileExW(query.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, flags);
    if (handle != INVALID_HANDLE_VALUE) handles.insert(handle);
    return handle;
}
BOOL Next(HANDLE handle, WIN32_FIND_DATAW& data) { return FindNextFileW(handle, &data); }
void Close(HANDLE handle) {
    std::lock_guard lock(listing_mutex);
    handles.erase(handle);
    FindClose(handle);
}
}
int wmain() {
    namespace fs = std::filesystem;
    const auto parent = fs::absolute(fs::current_path() / L"bench_data").lexically_normal();
    const auto base = parent / (L"content-scope-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(parent);
    if (!fs::create_directory(base)) { Check(false, "exclusive fixture creation"); return 1; }
    const auto wanted = base / L"wanted", deep = wanted / L"deep", outside = base / L"outside";
    fs::create_directories(deep); fs::create_directories(outside);
    for (const auto& path : {base / L"root.txt", wanted / L"keep.txt", deep / L"deep.txt", outside / L"outside.txt"})
        std::ofstream(path) << "scope_marker";
    const auto junction = base / L"link", offline = base / L"offline", offline_child = offline / L"deeper";
    Check(MakeJunction(junction, wanted), "create owned junction fixture");
    fs::create_directories(offline_child);
    std::ofstream(offline_child / L"offline.txt") << "scope_marker";
    Check(SetFileAttributesW(offline.c_str(), FILE_ATTRIBUTE_OFFLINE) != FALSE, "mark owned intermediate directory offline");
    // An absent, private endpoint makes accidental global-feed use fail promptly
    // without connecting to the user's index. Unconstrained shared scope must
    // still report the unavailable feed, rather than silently walk the disk.
    const auto private_pipe = L"\\\\.\\pipe\\PulseContentScopeTest." +
        std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
    if (!SetEnvironmentVariableW(L"PULSE_INDEX_FEED_PIPE", private_pipe.c_str())) return 1;
    for (int mode = 0; mode < 2; ++mode) {
    for (int scenario = 0; scenario < 18; ++scenario) {
        std::atomic<bool> cancelled{false};
        ContentIndexConfig config;
        config.shared_scope = mode != 0;
        config.roots = {{base.wstring(), pulse::text::Encoding::Auto}};
        ContentSearchRequest request;
        request.needle = L"scope_marker"; request.root = wanted.wstring(); request.recursive = false;
        denied = {Key(outside.wstring()), Key(deep.wstring())};
        opened.clear(); cancel_on_open = nullptr;
        std::set<std::wstring> expected_open{Key(wanted.wstring())};
        size_t expected_hits = 1;
        DWORD expected_error = ERROR_SUCCESS;
        switch (scenario) {
        case 0: break;
        case 1:
            request.recursive = true; denied.erase(Key(deep.wstring())); expected_open.insert(Key(deep.wstring())); expected_hits = 2; break;
        case 2:
            config.roots = {{wanted.wstring(), pulse::text::Encoding::Auto}}; request.root = base.wstring(); request.recursive = true;
            denied.erase(Key(deep.wstring())); expected_open.insert(Key(deep.wstring())); expected_hits = 2; break;
        case 3:
            config.roots = {{wanted.wstring(), pulse::text::Encoding::Auto}}; request.root = base.wstring(); expected_open.clear(); expected_hits = 0; break;
        case 4:
            config.roots.push_back({wanted.wstring(), pulse::text::Encoding::Auto}); request.recursive = true;
            denied.erase(Key(deep.wstring())); expected_open.insert(Key(deep.wstring())); expected_hits = 2; break;
        case 5:
            request.recursive = true; request.roots = {deep.wstring(), outside.wstring()}; denied.erase(Key(deep.wstring()));
            expected_open = {Key(deep.wstring())}; break;
        case 6:
            request.roots = {deep.wstring()}; expected_open.clear(); expected_hits = 0; break;
        case 7:
            config.roots = {{outside.wstring(), pulse::text::Encoding::Auto}}; expected_open.clear(); expected_hits = 0; break;
        case 8:
            request.recursive = true; expected_open.insert(Key(deep.wstring())); expected_error = ERROR_ACCESS_DENIED; break;
        case 9:
            cancelled = true; expected_open.clear(); expected_hits = 0; expected_error = ERROR_CANCELLED; break;
        case 10:
            config.roots.push_back({wanted.wstring(), pulse::text::Encoding::Auto}); request.root.clear();
            denied.clear(); expected_open.insert(Key(base.wstring())); expected_open.insert(Key(deep.wstring()));
            expected_open.insert(Key(outside.wstring())); expected_hits = 4; break;
        case 11: request.roots = {wanted.wstring()}; break;
        case 12: request.needle = L"no-match"; expected_hits = 0; break;
        case 13: cancel_on_open = &cancelled; expected_hits = 0; expected_error = ERROR_CANCELLED; break;
        case 14: request.root = (junction / L"deep").wstring(); expected_open.clear(); expected_hits = 0; break;
        case 15: request.root = offline_child.wstring(); expected_open.clear(); expected_hits = 0; break;
        case 16: request.root = (base / L"missing").wstring(); expected_open.clear(); expected_hits = 0; expected_error = ERROR_FILE_NOT_FOUND; break;
        case 17: request.candidate_paths = {(wanted / L"keep.txt").wstring()}; expected_open.clear(); break;
        }
        if (config.shared_scope && scenario == 10) {
            expected_open.clear(); expected_hits = 0; expected_error = ERROR_RETRY;
        }
        ContentTaskCache cache;
        cache.version = [](const auto&) { return L"scope-version"; };
        cache.excluded = [](const auto&) { return false; };
        cache.fresh = [](const auto&, uint64_t, uint64_t, const auto&) { return false; };
        ContentSearchProgress final;
        size_t hits = 0, terminals = 0;
        const bool ok = RunContentTaskSupplement(config, request, cancelled, cache, {}, 0,
            [&](const auto& progress, auto batch) { final = progress; hits += batch.size(); terminals += progress.done ? 1 : 0; return true; },
            [](const auto&, uint64_t, std::wstring& body, uint64_t& bytes, DWORD*, pulse::text::Encoding, const auto&) {
                body = L"scope_marker"; bytes = 12; return true;
            });
        const char* names[] = {"nonrecursive subdirectory avoids denied siblings and children", "recursive subdirectory avoids denied sibling",
            "recursive query ancestor starts at configured descendant", "nonrecursive ancestor has no deeper intersection",
            "overlapping configuration roots enumerate once", "root and roots intersect rather than union", "different nonrecursive scopes have empty intersection",
            "disjoint config and request open no directories", "relevant denied subtree remains a partial failure", "pre-cancellation opens no directory",
            "unconstrained request preserves recursive config scope despite recursive flag", "identical nonrecursive constraints retain direct files", "no matches still completes", "cancellation while opening closes handle", "narrowed scope cannot cross intermediate junction",
            "narrowed scope skips intermediate offline directory", "missing directory within scope reports its real error",
            "explicit candidates retain priority over directory and shared feed"};
        const bool valid = ok == (expected_error == ERROR_SUCCESS) && final.done && terminals == 1 && final.error == expected_error && hits == expected_hits &&
            std::set<std::wstring>(opened.begin(), opened.end()) == expected_open && opened.size() == expected_open.size() && handles.empty();
        std::cout << "[INFO] shared_scope=" << config.shared_scope << " scenario=" << scenario << '\n';
        Check(valid, config.shared_scope && scenario == 10 ? "unconstrained shared scope still requires global feed" : names[scenario]);
        if (!valid) std::cout << "[INFO] scenario=" << scenario << " ok=" << ok << " error=" << final.error << " hits=" << hits << " opened=" << opened.size() << '\n';
    }
    }
    for (int shared = 0; shared < 2; ++shared) {
        std::atomic<bool> cancelled{false};
        ContentIndexConfig config;
        config.shared_scope = shared != 0;
        if (!config.shared_scope) config.roots = {{outside.wstring(), pulse::text::Encoding::Auto}};
        ContentSearchRequest request;
        request.needle = L"scope_marker";
        request.root = wanted.wstring();
        request.recursive = true;
        request.allow_uncovered_root = true;
        denied = {Key(outside.wstring())};
        opened.clear();
        cancel_on_open = nullptr;
        ContentTaskCache cache;
        cache.version = [](const auto&) { return L"scope-version"; };
        cache.excluded = [&](const auto& p) { return shared && ContentPathUnder(ContentScopeKey(p), Key(deep.wstring())); };
        cache.fresh = [](const auto&, uint64_t, uint64_t, const auto&) { return false; };
        Check(ContentTaskRoots(config, request) == std::vector<std::wstring>{Key(wanted.wstring())},
              shared ? "ContentTaskRoots allows explicit uncovered scope under shared_scope"
                     : "ContentTaskRoots allows explicit uncovered scope outside config.roots");
        ContentSearchProgress final;
        size_t hits = 0;
        const bool ok = RunContentTaskSupplement(config, request, cancelled, cache, {}, 0,
            [&](const auto& progress, auto batch) { final = progress; hits += batch.size(); return true; },
            [](const auto&, uint64_t, std::wstring& body, uint64_t& bytes, DWORD*, pulse::text::Encoding, const auto&) {
                body = L"scope_marker"; bytes = 12; return true;
            });
        const std::set<std::wstring> expected = shared
            ? std::set<std::wstring>{Key(wanted.wstring())}
            : std::set<std::wstring>{Key(wanted.wstring()), Key(deep.wstring())};
        Check(ok && final.done && final.error == ERROR_SUCCESS && hits == expected.size() &&
                  std::set<std::wstring>(opened.begin(), opened.end()) == expected,
              shared ? "uncovered shared_scope content search walks wanted while respecting cache.excluded"
                     : "uncovered content search walks wanted and deep outside config.roots");
    }
    if (base.parent_path() != parent || !base.filename().wstring().starts_with(L"content-scope-")) return 1;
    SetFileAttributesW(offline.c_str(), FILE_ATTRIBUTE_NORMAL);
    RemoveDirectoryW(junction.c_str());
    fs::remove_all(base);
    return failures ? 1 : 0;
}

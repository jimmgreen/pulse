#include "search_adapter_host.h"
#include "../../app_runtime.h"
#include "../../app_state.h"
#include "../../../common/json_value.h"
#include "../../../fs/fs_snapshot.h"
#include "../../../index/index_query.h"
#include "../../../index/search_kinds.h"
#include <shlobj.h>
#include <algorithm>
#include <atomic>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace pulse::extensions::native {
namespace {

constexpr uint32_t kSearchAdapterIdTag = 0x40000000u;
constexpr uint32_t kSearchAdapterIdMask = 0xC0000000u;
constexpr uint64_t kWindowsToUnixEpoch100ns = 116444736000000000ULL;
constexpr size_t kFallbackMaxEntries = 8192;
constexpr int kFallbackMaxDepth = 4;

std::atomic<uint32_t> g_next_raw_index_id{1};

int64_t FileTimeToUnixMs(uint64_t ft_100ns) noexcept {
    if (ft_100ns <= kWindowsToUnixEpoch100ns) return 0;
    return static_cast<int64_t>((ft_100ns - kWindowsToUnixEpoch100ns) / 10000ULL);
}

uint64_t PackFileTime(const FILETIME& ft) noexcept {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | static_cast<uint64_t>(ft.dwLowDateTime);
}

struct PendingHostSearch {
    HWND owner = nullptr;
    uint64_t request_id = 0;
    uint32_t raw_index_id = 0;
    uint32_t max_results = 20;
    bool files_only = false;
    std::shared_ptr<std::atomic<bool>> cancelled;
    std::function<void(SearchBackendReply)> complete;
};

std::mutex g_search_mutex;
auto& ActiveByOwnerRequest() {
    static auto* map = new std::map<std::pair<HWND, uint64_t>, std::shared_ptr<PendingHostSearch>>();
    return *map;
}
auto& ActiveByRawIndexId() {
    static auto* map = new std::map<uint32_t, std::shared_ptr<PendingHostSearch>>();
    return *map;
}

uint32_t AllocateRawIndexId() noexcept {
    for (;;) {
        const uint32_t seq = g_next_raw_index_id.fetch_add(1, std::memory_order_relaxed) & 0x3FFFFFFFu;
        if (seq != 0) return kSearchAdapterIdTag | seq;
    }
}

bool ExpandSearchKindFilter(const std::wstring& kind_token,
                            std::vector<std::wstring>& exts,
                            bool& folders_only) {
    exts.clear();
    folders_only = false;
    if (kind_token.empty()) return true;
    const std::wstring folded = index::Fold(kind_token);
    if (folded.empty()) return true;
    if (folded == L"installer" || folded == L"installers" || folded == L"安装包") {
        exts = {L"exe", L"msi", L"msix", L"msixbundle", L"appx", L"appxbundle"};
        return true;
    }
    return index::ExpandKindToken(folded, exts, folders_only);
}

bool MatchesCompiledQuery(const std::wstring& full_path,
                          const std::wstring& name,
                          bool is_dir,
                          uint64_t size_bytes,
                          uint64_t modified_100ns,
                          const index::CompiledQuery& compiled) {
    if (compiled.groups.empty()) return true;
    for (const auto& group : compiled.groups) {
        bool all_ok = true;
        for (const auto& term : group) {
            if (!index::MatchTerm(full_path, name, is_dir, size_bytes, modified_100ns, term)) {
                all_ok = false;
                break;
            }
        }
        if (all_ok) return true;
    }
    return false;
}

std::wstring DefaultFallbackRoot() {
    PWSTR desktop = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, KF_FLAG_DEFAULT, nullptr, &desktop)) && desktop) {
        result = fs::NormalizePath(desktop);
    }
    if (desktop) CoTaskMemFree(desktop);
    return result;
}

void RunFallbackDirectoryScan(std::wstring root_dir,
                              std::wstring compiled_needle,
                              bool folders_only,
                              bool files_only,
                              uint32_t max_results,
                              std::shared_ptr<PendingHostSearch> pending) {
    if (!pending || pending->cancelled->load(std::memory_order_acquire)) return;
    if (root_dir.empty()) {
        root_dir = DefaultFallbackRoot();
    }

    SearchBackendReply reply;
    reply.status = PULSE_OK;
    reply.index_connected = false;
    if (root_dir.empty()) {
        std::function<void(SearchBackendReply)> cb;
        {
            std::lock_guard lock(g_search_mutex);
            ActiveByOwnerRequest().erase({pending->owner, pending->request_id});
            if (!pending->cancelled->load(std::memory_order_acquire)) {
                cb = std::move(pending->complete);
            }
        }
        if (cb) cb(std::move(reply));
        return;
    }

    const index::CompiledQuery compiled = index::ParseQuery(compiled_needle);

    struct Candidate {
        int rank = 100;
        SearchHitData hit;
    };
    std::vector<Candidate> matched;

    struct DirNode {
        std::wstring path;
        int depth = 0;
    };
    std::deque<DirNode> queue;
    queue.push_back(DirNode{root_dir, 0});
    size_t visited = 0;

    while (!queue.empty() && visited < kFallbackMaxEntries) {
        if (pending->cancelled->load(std::memory_order_acquire)) return;
        DirNode current = std::move(queue.front());
        queue.pop_front();

        std::wstring pattern = current.path;
        if (!pattern.empty() && pattern.back() != L'\\' && pattern.back() != L'/') {
            pattern.push_back(L'\\');
        }
        const std::wstring prefix = pattern;
        pattern.push_back(L'*');

        WIN32_FIND_DATAW fd{};
        HANDLE find = FindFirstFileExW(
            pattern.c_str(),
            FindExInfoBasic,
            &fd,
            FindExSearchNameMatch,
            nullptr,
            FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) continue;

        do {
            if (pending->cancelled->load(std::memory_order_acquire)) {
                FindClose(find);
                return;
            }
            const wchar_t* raw_name = fd.cFileName;
            if (raw_name[0] == L'.' &&
                (raw_name[1] == L'\0' || (raw_name[1] == L'.' && raw_name[2] == L'\0'))) {
                continue;
            }
            ++visited;
            if (visited > kFallbackMaxEntries) break;

            const bool is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            const bool is_reparse = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
            std::wstring name(raw_name);
            std::wstring full_path = prefix + name;

            if (is_dir && !is_reparse && current.depth < kFallbackMaxDepth) {
                queue.push_back(DirNode{full_path, current.depth + 1});
            }

            if (folders_only && !is_dir) continue;
            if (files_only && is_dir) continue;

            const uint64_t size_bytes = is_dir
                ? 0ULL
                : ((static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | static_cast<uint64_t>(fd.nFileSizeLow));
            const uint64_t modified_100ns = PackFileTime(fd.ftLastWriteTime);

            if (!MatchesCompiledQuery(full_path, name, is_dir, size_bytes, modified_100ns, compiled)) {
                continue;
            }

            Candidate cand;
            cand.rank = index::RankName(name.c_str(), static_cast<uint32_t>(name.size()), is_dir, compiled);
            cand.hit.path = json::Utf8(full_path);
            cand.hit.name = json::Utf8(name);
            cand.hit.size_bytes = size_bytes;
            cand.hit.modified_unix_ms = FileTimeToUnixMs(modified_100ns);
            cand.hit.flags = is_dir ? PULSE_SEARCH_HIT_FOLDER : PULSE_SEARCH_HIT_NONE;
            matched.push_back(std::move(cand));
        } while (FindNextFileW(find, &fd));

        FindClose(find);
    }

    if (pending->cancelled->load(std::memory_order_acquire)) return;

    std::sort(matched.begin(), matched.end(), [](const Candidate& a, const Candidate& b) {
        if (a.rank != b.rank) return a.rank < b.rank;
        return a.hit.name < b.hit.name;
    });

    reply.total_matches = matched.size();
    const size_t keep = (std::min)(matched.size(), static_cast<size_t>(max_results));
    reply.hits.reserve(keep);
    for (size_t i = 0; i < keep; ++i) {
        reply.hits.push_back(std::move(matched[i].hit));
    }

    std::function<void(SearchBackendReply)> cb;
    {
        std::lock_guard lock(g_search_mutex);
        ActiveByOwnerRequest().erase({pending->owner, pending->request_id});
        if (!pending->cancelled->load(std::memory_order_acquire)) {
            cb = std::move(pending->complete);
        }
    }
    if (cb) cb(std::move(reply));
}

} // namespace

bool SearchAdapterHostDeliver(uint32_t raw_id, index::SearchResult&& result) {
    if ((raw_id & kSearchAdapterIdMask) != kSearchAdapterIdTag) return false;
    std::shared_ptr<PendingHostSearch> pending;
    std::function<void(SearchBackendReply)> cb;
    {
        std::lock_guard lock(g_search_mutex);
        auto& by_raw = ActiveByRawIndexId();
        const auto it = by_raw.find(raw_id);
        if (it == by_raw.end()) return true;
        pending = it->second;
        by_raw.erase(it);
        if (pending) {
            ActiveByOwnerRequest().erase({pending->owner, pending->request_id});
            if (!pending->cancelled->load(std::memory_order_acquire)) {
                cb = std::move(pending->complete);
            }
        }
    }
    if (!cb || !pending) return true;

    SearchBackendReply reply;
    reply.status = result.error == ERROR_SUCCESS ? PULSE_OK : PULSE_ERROR_UNAVAILABLE;
    reply.index_connected = true;
    reply.total_matches = result.total;
    reply.hits.reserve((std::min)(result.hits.size(), static_cast<size_t>(pending->max_results)));
    for (const auto& h : result.hits) {
        if (pending->files_only && h.is_dir) continue;
        if (reply.hits.size() >= pending->max_results) break;
        SearchHitData item;
        item.path = json::Utf8(h.path);
        const std::filesystem::path p(h.path);
        const std::wstring filename = p.filename().wstring();
        item.name = json::Utf8(filename.empty() ? h.path : filename);
        item.size_bytes = h.is_dir ? 0ULL : h.size;
        item.modified_unix_ms = FileTimeToUnixMs(h.mtime);
        item.flags = h.is_dir ? PULSE_SEARCH_HIT_FOLDER : PULSE_SEARCH_HIT_NONE;
        reply.hits.push_back(std::move(item));
    }
    if (reply.total_matches < reply.hits.size()) {
        reply.total_matches = reply.hits.size();
    }
    cb(std::move(reply));
    return true;
}

bool ClaimSearchAdapterIndexResult(AppState& s, uint32_t raw_id) {
    if ((raw_id & kSearchAdapterIdMask) != kSearchAdapterIdTag) return false;
    index::SearchResult result;
    if (!s.core.index.TakeResult(raw_id, result)) return true;
    return SearchAdapterHostDeliver(raw_id, std::move(result));
}

SearchBackend MakeSearchBackend(HWND owner) {
    SearchBackend backend;
    backend.valid_owner = [owner] { return IsWindow(owner) != FALSE; };
    backend.start = [owner](SearchQuerySpec spec, std::function<void(SearchBackendReply)> complete) -> PulseStatus {
        auto* app = GetAppState(owner);
        if (!app || !IsWindow(owner)) return PULSE_ERROR_STATE;

        std::vector<std::wstring> kind_exts;
        bool kind_folders_only = false;
        ExpandSearchKindFilter(spec.kind_token, kind_exts, kind_folders_only);

        const bool files_only = (spec.flags & PULSE_SEARCH_FLAG_FILES_ONLY) != 0;
        const bool folders_only = ((spec.flags & PULSE_SEARCH_FLAG_FOLDERS_ONLY) != 0) || kind_folders_only;
        if (kind_folders_only && files_only) {
            SearchBackendReply empty;
            empty.status = PULSE_OK;
            empty.index_connected = app->core.index.Connected();
            complete(std::move(empty));
            return PULSE_OK;
        }

        std::wstring needle;
        if ((spec.flags & PULSE_SEARCH_FLAG_PINYIN) == 0) {
            needle += L"nopinyin: ";
        }
        needle += spec.query;
        if (!kind_exts.empty()) {
            needle += L" ext:";
            for (size_t i = 0; i < kind_exts.size(); ++i) {
                if (i > 0) needle.push_back(L';');
                needle += kind_exts[i];
            }
        }
        if (files_only) {
            needle += L" file:";
        }

        const std::wstring norm_scope = spec.scope_directory.empty()
            ? std::wstring{}
            : fs::NormalizePath(spec.scope_directory);
        if (!spec.scope_directory.empty() && norm_scope.empty()) {
            return PULSE_ERROR_ARGUMENT;
        }

        auto pending = std::make_shared<PendingHostSearch>();
        pending->owner = owner;
        pending->request_id = spec.request_id;
        pending->max_results = spec.max_results;
        pending->files_only = files_only;
        pending->cancelled = std::make_shared<std::atomic<bool>>(false);
        pending->complete = std::move(complete);

        if (app->core.index.Connected()) {
            index::Query q;
            q.needle = needle;
            q.path_prefix = norm_scope;
            q.folders_only = folders_only;
            q.limit = spec.max_results;
            q.rank = true;

            const uint32_t raw_id = AllocateRawIndexId();
            pending->raw_index_id = raw_id;
            {
                std::lock_guard lock(g_search_mutex);
                ActiveByOwnerRequest()[{owner, spec.request_id}] = pending;
                ActiveByRawIndexId()[raw_id] = pending;
            }
            app->core.index.SearchAsync(q, raw_id);
            return PULSE_OK;
        } else {
            std::lock_guard lock(g_search_mutex);
            ActiveByOwnerRequest()[{owner, spec.request_id}] = pending;
        }

        std::thread([norm_scope, needle = std::move(needle), folders_only, files_only,
                     max_results = spec.max_results, pending = std::move(pending)]() mutable {
            RunFallbackDirectoryScan(std::move(norm_scope), std::move(needle), folders_only,
                                     files_only, max_results, std::move(pending));
        }).detach();
        return PULSE_OK;
    };
    backend.cancel = [owner](uint64_t request_id) {
        std::shared_ptr<PendingHostSearch> pending;
        {
            std::lock_guard lock(g_search_mutex);
            auto& by_req = ActiveByOwnerRequest();
            const auto it = by_req.find({owner, request_id});
            if (it != by_req.end()) {
                pending = it->second;
                by_req.erase(it);
                if (pending && pending->raw_index_id != 0) {
                    ActiveByRawIndexId().erase(pending->raw_index_id);
                }
            }
        }
        if (pending && pending->cancelled) {
            pending->cancelled->store(true, std::memory_order_release);
        }
    };
    return backend;
}

SearchAdapter::SearchAdapter(HWND owner, std::function<bool()> alive,
                             std::shared_ptr<DiagnosticsSink> diagnostics,
                             std::function<void()> wake)
    : SearchAdapter(MakeSearchBackend(owner), std::move(alive), std::move(diagnostics), std::move(wake)) {}

} // namespace pulse::extensions::native

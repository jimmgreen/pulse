#include "../index/index_engine.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <new>
#include <string_view>

namespace {
thread_local bool count_allocations = false;
thread_local size_t allocation_calls = 0, allocation_bytes = 0;
int failures = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    failures += !ok;
}
}
namespace pulse::index {
struct EngineTestAccess {
    static bool Fixture(Engine& engine, const std::wstring& path, int files) {
        Engine::Store store;
        engine.AddNodeLocked(store, -1, L"C:", Engine::kFlagDir);
        int ordinal = 0;
        for (const auto* directory : {L"Allowed", L"Hidden"}) {
            const int parent = static_cast<int>(store.nodes.size());
            engine.AddNodeLocked(store, 0, directory, Engine::kFlagDir);
            for (int i = 0; i < files / 2; ++i, ++ordinal) {
                wchar_t name[64]{};
                swprintf_s(name, L"match_%06d_%ls.txt", ordinal, ordinal % 3 ? L"file" : L"中文");
                const int id = engine.AddNodeLocked(store, parent, name, 0);
                store.nodes[id].unused = static_cast<uint32_t>(id + 1);
                store.attrs[id].size = static_cast<uint64_t>((ordinal * 31) % 9991);
                store.attrs[id].mtime = 1700000000u + static_cast<uint32_t>((ordinal * 17) % 7651);
            }
            store.nodes[parent].unused = static_cast<uint32_t>(store.nodes.size());
        }
        store.nodes[0].unused = static_cast<uint32_t>(store.nodes.size());
        std::vector<Engine::VolState> volumes(1);
        volumes[0].letter = L'C'; volumes[0].volume_id = L"match-cache-owned-fixture";
        volumes[0].root_idx = 0; volumes[0].first_idx = 0;
        volumes[0].item_count = static_cast<uint32_t>(store.nodes.size());
        if (!engine.WriteIndexFile(path, store, volumes, 123456) ||
            !MoveFileExW((path + L".tmp").c_str(), path.c_str(), 0)) return false;
        std::unique_ptr<Engine::MappedFile> mapped;
        if (!engine.MapIndexFile(path, mapped)) return false;
        engine.AdoptMappedLocked(std::move(mapped));
        // Adoption starts the real pinyin builder. Wait for its publication so
        // benchmark modes do not race background CPU work / cache invalidation.
        const auto deadline = GetTickCount64() + 30000;
        while (GetTickCount64() < deadline) {
            {
                std::shared_lock lock(engine.mutex_);
                if (engine.pinyin_ready_) return true;
            }
            Sleep(1);
        }
        return false;
    }
    static void Cold(Engine& engine) { engine.InvalidateFilterLocked(); }
    static size_t CachedBytes(const Engine& engine) {
        return engine.cache_set_.ids.size() * sizeof(int32_t) + engine.cache_set_.bits.size() * sizeof(uint64_t);
    }
    static bool Dense(const Engine& engine) { return engine.cache_set_.dense; }
    static void MoveFirstToHidden(Engine& engine, int hidden) {
        const auto node = engine.NodeAt(2);
        const std::wstring name(engine.NameOf(2));
        engine.ChildMapRemove(node.parent, name, 2);
        auto& patch = engine.patches_[2];
        patch.parent = hidden; patch.flags = node.flags; patch.has_meta = true;
        engine.ChildMapAdd(hidden, name, 2);
        engine.InvalidateFilterLocked();
    }
};
}
namespace {
using namespace pulse::index;
bool Same(const SearchResult& a, const SearchResult& b) {
    return a.total == b.total && a.partial == b.partial && a.error == b.error &&
        a.unchecked_dirs == b.unchecked_dirs &&
        std::equal(a.hits.begin(), a.hits.end(), b.hits.begin(), b.hits.end(), [](const Hit& x, const Hit& y) {
            return x.path == y.path && x.name == y.name && x.size == y.size && x.mtime == y.mtime && x.is_dir == y.is_dir;
        });
}
struct Visibility : DirVisibility {
    int hidden = -1;
    int state = 0;
    bool top = false;
    std::atomic<uint32_t>* cancel = nullptr;
    int State(int32_t dir) const override {
        if (cancel) cancel->store(2);
        return dir == hidden ? state : 1;
    }
    bool TopFirst() const override { return top; }
};
bool CachedEqualsCold(Engine& engine, const Query& q, const DirVisibility* visibility = nullptr) {
    const auto cached = engine.Search(q, nullptr, 0, visibility);
    EngineTestAccess::Cold(engine);
    const auto cold = engine.Search(q, nullptr, 0, visibility);
    return Same(cached, cold);
}
void Regression(Engine& engine, int files) {
    Query q; q.needle = L"match"; q.rank = false; q.limit = 17;
    Query full_query = q; full_query.limit = static_cast<size_t>(files);
    const auto full_result = engine.Search(full_query);
    bool slices = full_result.hits.size() == static_cast<size_t>(files);
    for (bool descending : {false, true}) {
        auto ordered = full_result.hits;
        if (descending) std::reverse(ordered.begin(), ordered.end());
        for (size_t offset : {size_t{0}, size_t{13}, static_cast<size_t>(files - 3), static_cast<size_t>(files + 5), SIZE_MAX}) {
            for (size_t limit : {size_t{0}, size_t{1}, size_t{48}, static_cast<size_t>(files + 5)}) {
                Query page = q; page.sort_desc = descending; page.offset = offset; page.limit = limit;
                SearchResult expected; expected.total = full_result.total;
                const size_t begin = (std::min)(offset, ordered.size());
                const size_t end = begin + (std::min)(limit, ordered.size() - begin);
                expected.hits.assign(ordered.begin() + begin, ordered.begin() + end);
                slices &= Same(engine.Search(page), expected);
            }
        }
    }
    Check(slices, "Index ascending/descending pages equal independent full-order slices including SIZE_MAX offset");
    bool pages = true;
    for (auto sort : {ResultSort::Index, ResultSort::Name, ResultSort::Size, ResultSort::Mtime}) {
        for (bool descending : {false, true}) {
            q.sort = sort; q.sort_desc = descending;
            for (size_t offset : {size_t{0}, size_t{13}, static_cast<size_t>(files - 3), static_cast<size_t>(files + 5)}) {
                q.offset = offset;
                pages &= CachedEqualsCold(engine, q);
            }
        }
    }
    Check(pages, "cached page matches cold across all sort directions first middle tail beyond end");
    q.offset = 0; q.limit = 0;
    Check(CachedEqualsCold(engine, q) && engine.Search(q).total == static_cast<size_t>(files), "count-only cached query preserves exact total");
    q.limit = 17; q.rank = true; q.offset = 11;
    bool ranking = CachedEqualsCold(engine, q);
    q.needle = L"m"; ranking &= CachedEqualsCold(engine, q);
    q.needle = L"match_000"; ranking &= CachedEqualsCold(engine, q);
    Check(ranking, "ranked single-character and narrowed cached pages equal cold");
    q.needle = L"match"; q.rank = false; q.sort = ResultSort::Index; q.sort_desc = false; q.offset = 0;
    Visibility restricted; restricted.hidden = files / 2 + 2;
    Check(CachedEqualsCold(engine, q, &restricted) && engine.Search(q, nullptr, 0, &restricted).total == static_cast<size_t>(files / 2), "caller filtering excludes hidden folder");
    Check(engine.Search(q).total == static_cast<size_t>(files), "restricted caller never contaminates shared match cache");
    restricted.state = -1;
    const auto unknown = engine.Search(q, nullptr, 0, &restricted);
    Check(unknown.hits.empty() && !unknown.unchecked_dirs.empty() && CachedEqualsCold(engine, q, &restricted), "unknown permission discovery preserved on exact cache");
    q.rank = true; restricted.top = true;
    Check(CachedEqualsCold(engine, q, &restricted), "ranked TopFirst permission discovery equals cold");
    std::atomic<uint32_t> latest{2};
    Check(engine.Search(q, &latest, 1).hits.empty(), "already cancelled exact query returns no hits");
    latest.store(1); restricted.state = 1; restricted.cancel = &latest;
    Check(engine.Search(q, &latest, 1, &restricted).hits.empty(), "cancellation during caller filtering/ranking returns no hits");
    restricted.cancel = nullptr;
    Check(CachedEqualsCold(engine, q), "cancelled query does not corrupt subsequent cache");
    q.rank = false; q.path_prefix = L"C:\\Allowed";
    const auto before_move = engine.Search(q);
    EngineTestAccess::MoveFirstToHidden(engine, files / 2 + 2);
    const auto after_move = engine.Search(q);
    Check(before_move.total == after_move.total + 1 && CachedEqualsCold(engine, q), "same universe parent move invalidates cached scope");
    engine.AddForTest(L"C:\\Allowed\\match_new.txt", L"match_new.txt", false, 4321);
    Check(engine.Search(q).total == before_move.total && CachedEqualsCold(engine, q), "live appended file invalidates exact cached page");
    q.path_prefix.clear(); q.needle = L"no_such_match_xyz";
    Check(CachedEqualsCold(engine, q) && engine.Search(q).total == 0, "empty match cache");
    q.needle.clear(); q.folders_only = true;
    Check(CachedEqualsCold(engine, q), "folder-only cache scope independent from files");
}
uint64_t Hash(const SearchResult& result) {
    uint64_t hash = result.total;
    for (const auto& hit : result.hits) for (wchar_t ch : hit.path) hash = (hash ^ ch) * 1099511628211ull;
    return hash;
}
void Performance(Engine& engine, int files) {
    for (const auto* mode : {"count", "index", "index_desc", "name", "rank"}) {
        Query q; q.needle = L"match"; q.rank = std::string_view(mode) == "rank";
        q.sort = std::string_view(mode) == "name" ? ResultSort::Name : ResultSort::Index;
        q.sort_desc = std::string_view(mode) == "index_desc";
        q.limit = std::string_view(mode) == "count" ? 0 : 48;
        const auto warm = engine.Search(q);
        Check(warm.total == static_cast<size_t>(files), "performance corpus total");
        const size_t cached_bytes = EngineTestAccess::CachedBytes(engine);
        allocation_calls = allocation_bytes = 0;
        count_allocations = true;
        const auto start = std::chrono::steady_clock::now();
        uint64_t checksum = 0;
        constexpr int iterations = 40;
        for (int i = 0; i < iterations; ++i) {
            q.offset = static_cast<size_t>((i % 7) * 48);
            const auto result = engine.Search(q);
            checksum ^= Hash(result) + static_cast<uint64_t>(i);
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        count_allocations = false;
        std::printf("files=%d dense=%d cache_bytes=%zu mode=%s pages=%d ms=%.3f allocs=%zu bytes=%zu checksum=%llu\n",
            files, EngineTestAccess::Dense(engine), cached_bytes, mode, iterations, ms,
            allocation_calls, allocation_bytes, static_cast<unsigned long long>(checksum));
        Check(CachedEqualsCold(engine, q), "performance last page equals cold result");
    }
}
}
void* operator new(size_t bytes) {
    if (count_allocations) { ++allocation_calls; allocation_bytes += bytes; }
    if (void* p = std::malloc(bytes ? bytes : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](size_t bytes) { return ::operator new(bytes); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }
int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    const bool perf = argc > 1 && std::string_view(argv[1]) == "--perf";
    const auto root = fs::absolute(fs::path(L"bench_data") /
        (L"match_cache_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64())));
    if (!fs::create_directory(root)) return 2;
    for (int files : perf ? std::vector<int>{50000, 200000} : std::vector<int>{4096, 70000}) {
        pulse::index::Engine engine;
        const bool ready = pulse::index::EngineTestAccess::Fixture(engine, (root / (std::to_wstring(files) + L".bin")).wstring(), files);
        Check(ready, "owned mapped index fixture ready with pinyin settled, no service");
        if (ready) {
            if (perf) Performance(engine, files);
            else Regression(engine, files);
        }
    }
    std::error_code error;
    fs::remove_all(root, error);
    Check(!error && !fs::exists(root), "owned mapped fixtures cleaned up");
    std::printf("Failures: %d\n", failures);
    return failures ? 1 : 0;
}

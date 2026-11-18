#include "../index/content_index.h"
#include "../index/network_index.h"
#include "../app/global_search_handoff.h"
#include "../app/search_query.h"
#include "../app/live_network_search.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>

using namespace pulse;
namespace {
int failed = 0;
void Check(bool ok, const char* label) {
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failed;
}
using Names = std::set<std::wstring>;
void FilenamePredicates(const std::filesystem::path& base) {
    const auto root = base / L"files";
    std::filesystem::create_directories(root);
    std::ofstream(root / L"report.log") << "marker";
    std::ofstream(root / L"draft.txt") << "marker and extra text";
    const auto set_date = [&](const wchar_t* name, WORD year) {
        HANDLE file = CreateFileW((root / name).c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        SYSTEMTIME time{}; time.wYear = year; time.wMonth = 6; time.wDay = 15;
        FILETIME stamp{};
        const bool ok = SystemTimeToFileTime(&time, &stamp) && file != INVALID_HANDLE_VALUE &&
                        SetFileTime(file, nullptr, nullptr, &stamp);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        Check(ok, "isolated fixture timestamps");
    };
    set_date(L"report.log", 2020); set_date(L"draft.txt", 2025);
    index::ContentIndex cache((base / L"cache.sqlite").wstring());
    index::ContentIndexConfig config;
    config.roots = {{root.wstring(), text::Encoding::Auto}};
    // This target can live in build/unify_build; its isolated fixtures therefore
    // have a build ancestor, which the production default intentionally excludes.
    config.excluded_directories.clear();
    const bool configured = cache.Configure(config);
    const bool idle = configured && cache.WaitUntilIdle(10000);
    const auto status = cache.Status();
    printf("cache configured=%d idle=%d indexed=%llu skipped=%llu errors=%llu error=%lu pending=%llu\n",
           configured, idle, status.indexed_files, status.skipped_files, status.errors,
           status.error, status.pending_files);
    Check(configured && idle && status.indexed_files == 2,
          "both content fixtures indexed");
    struct Case { const wchar_t* query; const wchar_t* expected; };
    for (const auto& test : {Case{L"draft", L"draft.txt"}, {L"!draft", L"report.log"},
             {L"ext:log", L"report.log"}, {L"!ext:log", L"draft.txt"},
             {L"size:>10", L"draft.txt"}, {L"!size:>10", L"report.log"},
             {L"dm:2020", L"report.log"}, {L"!dm:2020", L"draft.txt"}}) {
        index::ContentSearchRequest request;
        request.root = root.wstring(); request.needle = L"marker";
        request.filename_query = test.query;
        std::atomic<bool> cancel{false};
        Names cached, scanned, ordinary;
        index::ContentSearchProgress cached_final{}, scanned_final{};
        cache.Search(request, cancel, [&](const auto& progress, auto hits) {
            cached_final = progress;
            for (const auto& hit : hits) cached.insert(hit.name);
            return true;
        });
        request.indexed = false;
        index::RunContentSearch(request, cancel, [&](const auto& progress, auto hits) {
            scanned_final = progress;
            for (const auto& hit : hits) scanned.insert(hit.name);
            return true;
        });
        index::LiveNetworkMatches matches; std::mutex mutex;
        index::LiveNetworkWalk(root.wstring(), test.query, false, matches, mutex, [] { return false; }, [] {});
        for (const auto& hit : matches.hits) ordinary.insert(hit.name);
        if (!cached_final.done || cached_final.error || cached != Names{test.expected})
            printf("cached predicate=%ls done=%d error=%lu hits=%zu expected=%ls\n",
                   test.query, cached_final.done, cached_final.error, cached.size(), test.expected);
        Check(cached_final.done && !cached_final.error && cached == Names{test.expected}, "cached positive/negative filename predicate");
        Check(scanned_final.done && !scanned_final.error && scanned == Names{test.expected}, "direct positive/negative filename predicate");
        Check(matches.complete && !matches.error && ordinary == cached && ordinary == scanned, "ordinary filename search agrees with both content providers");
    }
}
void Handoff() {
    for (const std::wstring query : {L"ext:pdf", L"size:>1mb", L"a|b", L"\"exact name\"", L"folder:", L"path:\"C:\\original folder\" ext:pdf"}) {
        const auto original = app::SplitSearchQueryText(query);
        for (const std::wstring folder : {L"", L"pulse:recycle", L"\\\\server\\share with spaces"}) {
            const auto path = GlobalSearchHandoffPath({query, false, folder});
            const auto transferred = app::SplitSearchQueryText(path.substr(std::wstring(L"pulse:search:").size()));
            Check(original.filename_needle == transferred.filename_needle &&
                  transferred.path_prefix == (folder.empty() ? original.path_prefix : folder),
                  "handoff retains raw filename operators, OR, quotes, and scope");
        }
    }
    const auto content = GlobalSearchHandoffPath({L"marker", true, L"C:\\fixture"});
    Check(app::SplitSearchQueryText(content.substr(13)).content.present(), "content handoff retains content mode");
}
void Merge() {
    index::SearchResult local, network;
    for (int i = 0; i < 200; ++i) {
        index::Hit hit; hit.name = L"copy-report-" + std::to_wstring(i) + L".txt";
        hit.path = L"C:\\fixture\\" + hit.name; local.hits.push_back(hit);
    }
    index::Hit exact; exact.name = L"report"; exact.path = L"\\\\server\\share\\report";
    network.hits.push_back(exact);
    index::Hit duplicate = local.hits.front(); duplicate.path[0] = L'c';
    network.hits.push_back(duplicate);
    local.total = local.hits.size(); network.total = network.hits.size();
    index::Query query; query.needle = L"report"; query.limit = 200;
    const auto a = index::MergeSearchResults(query, local, network, true);
    const auto b = index::MergeSearchResults(query, network, local, true);
    Check(a.hits.size() == 200 && a.hits.front().name == L"report" && a.total == 201,
          "global top-N keeps exact network hit and removes case-insensitive duplicate");
    Check(a.hits.size() == b.hits.size() && std::equal(a.hits.begin(), a.hits.end(), b.hits.begin(), [](const auto& x, const auto& y) {
        return index::Fold(x.path) == index::Fold(y.path);
    }), "provider order leaves global top-N unchanged");
}
void Sessions() {
    auto a = std::make_shared<LiveNetworkSearch>(); a->session_id = 1; a->key = L"same"; a->latest_id = 11;
    auto b = std::make_shared<LiveNetworkSearch>(); b->session_id = 2; b->key = L"same"; b->latest_id = 22;
    {
        LiveNetworkSearchSessions sessions;
        sessions.Set(a); sessions.Drop(3);
        Check(sessions.Owns(a) && !a->cancel, "unrelated local refresh cannot cancel UNC pane");
        sessions.Set(b);
        Check(sessions.Owns(a) && sessions.Owns(b) && a->latest_id == 11, "same query in two panes retains independent result owners");
        auto replacement = std::make_shared<LiveNetworkSearch>(); replacement->session_id = 2; replacement->key = L"different";
        sessions.Set(replacement);
        Check(b->cancel && !sessions.Owns(b) && sessions.Owns(a), "different UNC query cancels only originating pane and rejects stale progress");
        a->latest_id = 12;
        Check(sessions.Find(1) == a && sessions.Find(2) == replacement, "paging and focus changes retain session ownership");
        sessions.Drop(2);
        Check(replacement->cancel && sessions.Owns(a), "closing or cancelling one pane leaves other pane running");
    }
    Check(a->cancel, "window teardown cancels remaining worker");
}
}
int wmain() {
    wchar_t module[32768]{}; GetModuleFileNameW(nullptr, module, ARRAYSIZE(module));
    const auto base = std::filesystem::path(module).parent_path().parent_path() / L"bench_data" /
        (L"search-audit-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(base);
    FilenamePredicates(base); Handoff(); Merge(); Sessions();
    std::filesystem::remove_all(base);
    return failed ? 1 : 0;
}

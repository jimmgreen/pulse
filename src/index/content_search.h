#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "index_query.h"

namespace pulse::index {
class ContentResultStore;

enum class ContentSearchMode : uint32_t { Content = 0, Duplicates = 1 };
enum class ContentResultSort : uint32_t { Index = 0, Name = 1, Size = 2, Mtime = 3, Path = 4, Type = 5 };
enum class ContentSearchPhase : uint32_t { Enumerating = 0, Hashing = 1 };


struct ContentSearchRequest {
    uint64_t generation = 0;
    uint64_t session_id = 0;
    uint64_t after_revision = 0;
    bool incremental = false; // Server-local subscription state; revision zero is a valid cursor.
    bool subscribe = false;
    ContentSearchMode mode = ContentSearchMode::Content;
    std::wstring root;
    std::vector<std::wstring> roots;
    std::vector<std::wstring> candidate_paths;
    bool indexed = true;
    bool allow_uncovered_root = false;
    bool task_scan = false; // Interactive query also examines uncached files.
    ContentResultSort sort = ContentResultSort::Index;
    bool sort_desc = false;
    std::wstring filename_query;
    std::wstring needle;
    std::vector<std::wstring> needles;
    std::vector<std::wstring> excluded_needles;
    ContentMatchMode match_mode = ContentMatchMode::AllWords;
    bool recursive = true;
    bool case_sensitive = false;
    bool whole_word = false;
    bool skip_system_locations = false;
    uint64_t minimum_file_bytes = 0;
    uint64_t maximum_file_bytes = 64ull * 1024ull * 1024ull;
    uint64_t maximum_document_bytes = 512ull * 1024ull * 1024ull;
    size_t maximum_hits = 0; // 0 means complete results.
    bool paged_results = false; // Client-side disk spool; not an IPC payload flag.
    std::shared_ptr<ContentResultStore> previous_results; // Same-query background refresh only.
};

struct ContentHit {
    std::wstring path;
    std::wstring name;
    std::wstring snippet;
    uint64_t size = 0;
    uint64_t modified = 0;
    uint32_t line = 0;
    uint32_t group = 0;
    bool removed = false;
    uint64_t file_id = 0;
};

enum class ContentSubscriptionFailure : uint32_t {
    None, QueueOverflow, FeedConnect, FeedNotReady, FeedGap, WatchOverflow, WatchNotReady, Transport,
    Metadata, DirectoryEnumeration, Reconcile
};

struct ContentSearchProgress {
    uint64_t generation = 0;
    uint64_t index_revision = 0;
    bool live = false;
    bool delta = false;
    uint64_t scanned_files = 0;
    uint64_t scanned_bytes = 0;
    uint64_t total_files = 0;
    ContentSearchPhase phase = ContentSearchPhase::Enumerating;
    std::wstring current_root;
    bool done = false;
    bool truncated = false;
    DWORD error = ERROR_SUCCESS;
    DWORD subscription_error = ERROR_SUCCESS;
    ContentSubscriptionFailure subscription_failure = ContentSubscriptionFailure::None;
};

using ContentBatchCallback = std::function<bool(const ContentSearchProgress&,
                                                 std::vector<ContentHit>)>;

bool MatchContentFilename(const std::wstring& path, uint64_t size, uint64_t modified, const CompiledQuery& query);

bool RunContentSearch(const ContentSearchRequest& request, const std::atomic<bool>& cancelled,
                      ContentBatchCallback callback);

} // namespace pulse::index

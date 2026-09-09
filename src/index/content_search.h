#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "index_query.h"

namespace pulse::index {

enum class ContentSearchMode : uint32_t { Content = 0, Duplicates = 1 };
enum class ContentSearchPhase : uint32_t { Enumerating = 0, Hashing = 1 };

inline constexpr size_t kContentCandidateCap = 10000;

struct ContentSearchRequest {
    uint64_t generation = 0;
    ContentSearchMode mode = ContentSearchMode::Content;
    std::wstring root;
    std::vector<std::wstring> roots;
    std::vector<std::wstring> candidate_paths;
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
    size_t maximum_hits = 10000;
};

struct ContentHit {
    std::wstring path;
    std::wstring name;
    std::wstring snippet;
    uint64_t size = 0;
    uint64_t modified = 0;
    uint32_t line = 0;
    uint32_t group = 0;
};

struct ContentSearchProgress {
    uint64_t generation = 0;
    uint64_t scanned_files = 0;
    uint64_t scanned_bytes = 0;
    uint64_t total_files = 0;
    ContentSearchPhase phase = ContentSearchPhase::Enumerating;
    std::wstring current_root;
    bool done = false;
    bool truncated = false;
    DWORD error = ERROR_SUCCESS;
};

using ContentBatchCallback = std::function<bool(const ContentSearchProgress&,
                                                 std::vector<ContentHit>)>;

bool RunContentSearch(const ContentSearchRequest& request, const std::atomic<bool>& cancelled,
                      ContentBatchCallback callback);

} // namespace pulse::index

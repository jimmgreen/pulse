// index_types.h — Search request/result types shared by the index engine,
// its pipe protocol and pulse.exe. Kept apart from index_engine.h so callers
// do not compile the engine class and its storage headers.
#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace pulse::index {

inline constexpr size_t kSearchPageCap = 100000;
// Consecutive base-merge failures back off 5 s, 10 s, 20 s ... capped at ten
// minutes, so a deterministic failure cannot rewrite the full base every 5 s.
constexpr ULONGLONG MergeRetryDelayMs(uint32_t consecutive_failures) noexcept {
    constexpr ULONGLONG base = 5000, cap = 10ull * 60ull * 1000ull;
    if (consecutive_failures == 0) return 0;
    const uint32_t shift = consecutive_failures - 1 < 7 ? consecutive_failures - 1 : 7;
    const ULONGLONG delay = base << shift;
    return delay < cap ? delay : cap;
}
inline constexpr size_t kSearchUiPageSize = 2048;

struct Hit {
    std::wstring path;
    std::wstring name;
    bool is_dir = false;
    uint64_t size = 0;
    uint64_t mtime = 0; // FILETIME as u64
    // Client-side recycle source only; never inferred from a physical name.
    std::wstring recycle_path;
};

enum class ResultSort : uint8_t { Index, Name, Size, Mtime };

struct Query {
    uint64_t session_id = 0;
    bool subscribe = false;
    std::wstring needle;
    std::wstring path_prefix;
    bool folders_only = false;
    size_t offset = 0;
    size_t limit = 48;
    bool rank = true;
    ResultSort sort = ResultSort::Index;
    bool sort_desc = false;
};

struct SearchResult {
    uint64_t revision = 0;
    std::vector<Hit> hits;
    size_t total = 0;
    DWORD error = 0;
    // Filtered searches: parent folders DirVisibility could not answer yet.
    // No hits are produced until every one of them has been checked.
    std::vector<std::pair<int32_t, std::wstring>> unchecked_dirs;
    // Filtered ranked searches (DirVisibility::TopFirst): the hits are final
    // but folders of lower-ranked matches are still unchecked, so `total`
    // counts confirmed names only. The host follows up with the full count.
    bool partial = false;
    uint64_t layout = 0;  // Engine::LayoutEpoch() the node ids above belong to
};

// Optional per-caller filter for Engine::Search. A match is returned only
// when the caller may list its parent folder, since that listing is what
// would show the name to the caller in Explorer. Node ids passed to State()
// are stable for one Engine::LayoutEpoch().
class DirVisibility {
public:
    virtual ~DirVisibility() = default;
    virtual int State(int32_t dir) const = 0;  // 1 listable, 0 not, -1 unknown
    // Ranked pages: ask only for the folders that decide the page.
    virtual bool TopFirst() const { return false; }
};

} // namespace pulse::index

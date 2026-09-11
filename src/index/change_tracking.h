#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
namespace pulse::index {
std::wstring NormalizeChangePath(std::wstring path);
bool IsChangeJournalName(std::wstring_view name);
enum class ChangeKind : uint32_t { Created, Modified, Deleted, Renamed, MovedIn, MovedOut };
enum class ChangeState : uint32_t { Available, Gap, Unavailable, Scanning, Offline, NotCovered };
enum class ChangeSource : uint32_t { Event, InitialMtime };
struct ChangeRecord {
    uint64_t id = 0, time = 0;
    uint64_t file_id = 0;
    ChangeKind kind = ChangeKind::Modified;
    bool is_dir = false;
    ChangeSource source = ChangeSource::Event;
    std::wstring path, old_path;
};
struct ChangeSummary {
    std::wstring path;
    uint64_t last_change = 0;
    uint32_t count = 0;
    uint32_t counts[6]{};
    uint32_t initial_count = 0;
    bool has_deleted = false, incomplete = true;
    ChangeState state = ChangeState::Unavailable;
};
struct ChangeResponse {
    ChangeState state = ChangeState::Unavailable;
    std::vector<ChangeSummary> summaries;
    std::vector<ChangeRecord> records;
    uint64_t next_cursor = 0;
};
class ChangeTracker {
public:
    static uint64_t Now();
    void Open(const std::wstring& directory);
    bool Lease(const std::wstring& owner, bool enabled);
    void Record(ChangeRecord record);
    void Seed(const std::wstring& owner, std::vector<ChangeRecord> records);
    void Gap();
    void Flush();
    ChangeResponse Summaries(const std::wstring& owner, const std::vector<std::wstring>& paths, uint64_t since = 0);
    ChangeResponse Details(const std::wstring& owner, const std::wstring& path,
                           uint64_t since, uint64_t before, uint32_t limit, uint32_t kind_filter = UINT32_MAX);
private:
    friend struct ChangeTrackerTestAccess;
    struct Journal {
        uint64_t start = 0, expiry = 0, tracking_since = 0, paused_at = 0, gap_end = 0, gap_until = 0;
        bool gap = false, loaded = false, seeded = false, dirty = false, active = false;
        uint64_t saved = 0, revision = 0, cache_revision = UINT64_MAX, cache_since = 0, cache_expires = 0, cache_oldest = UINT64_MAX;
        std::unordered_map<std::wstring, ChangeSummary> summaries;
        std::vector<ChangeRecord> records;
    };
    void Load(const std::wstring& owner, Journal& journal);
    bool Save(const std::wstring& owner, const Journal& journal);
    void Prune(Journal& journal);
    std::mutex mutex_;
    std::wstring directory_;
    std::unordered_map<std::wstring, Journal> journals_;
    uint64_t next_id_ = 1;
};
}

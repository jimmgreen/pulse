#pragma once
#include <string>
#include <vector>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>

namespace pulse::app {

struct SearchHistoryEntry {
    std::wstring query;
    std::wstring path;
};

struct SearchHistory {
    bool persist = true;
    std::vector<SearchHistoryEntry> entries;

    bool Load();
    bool Save() const;
    // Mutations only touch memory. Save snapshots on a serial background worker.
    bool Record(const std::wstring& query, const std::wstring& path);
    bool Remove(const std::wstring& path);
    bool Clear();
    std::wstring ToJson() const;
    bool FromJson(const std::wstring& json);
    bool LoadFromFile(const std::wstring& file);
    bool SaveToFile(const std::wstring& file) const;
};

class SearchHistoryWriter {
public:
    explicit SearchHistoryWriter(std::wstring file = {});
    ~SearchHistoryWriter();
    SearchHistoryWriter(const SearchHistoryWriter&) = delete;
    SearchHistoryWriter& operator=(const SearchHistoryWriter&) = delete;
    void Submit(SearchHistory snapshot);
    void Flush();
    void Stop();

private:
    void Run();
    std::wstring file_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<SearchHistory> pending_;
    uint64_t submitted_ = 0;
    uint64_t completed_ = 0;
    bool stopping_ = false;
    std::thread thread_;
};

} // namespace pulse::app

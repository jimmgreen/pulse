#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace pulse::app {
struct FolderStatistics {
    uint64_t bytes = 0, files = 0, directories = 0;
    uint64_t version = 0;
    bool partial = false;
    bool verified = false;
};

class FolderStatisticsService {
public:
    using Scan = std::function<FolderStatistics(const std::wstring&, const std::atomic_bool&)>;
    explicit FolderStatisticsService(Scan scan = {});
    ~FolderStatisticsService();
    FolderStatisticsService(const FolderStatisticsService&) = delete;
    FolderStatisticsService& operator=(const FolderStatisticsService&) = delete;
    // Called on a coordinator, never the UI. after_version subscribes to the
    // next changed result; cancellation detaches only this caller.
    FolderStatistics Read(const std::wstring& path, const std::function<bool()>& cancelled,
                          uint64_t after_version = 0);
    uint64_t ScansStarted() const;
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
FolderStatisticsService& SharedFolderStatistics();
}

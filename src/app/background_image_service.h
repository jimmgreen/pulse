// background_image_service.h — the single source of the current background
// image path, shared by every window.
#pragma once

#include "background_image_fetch.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace pulse::app {

enum class BackgroundSource : uint8_t { Image = 0, Folder = 1, Url = 2, Api = 3 };

enum class BackgroundSwitchMode : uint8_t {
    Manual = 0,
    OnStartup = 1,
    Timed = 2,
    OnNavigate = 3,
};

struct BackgroundImageConfig {
    BackgroundSource source = BackgroundSource::Image;
    std::wstring image;
    std::wstring folder;
    std::wstring url;
    std::wstring api_pointer;
    BackgroundSwitchMode switch_mode = BackgroundSwitchMode::Manual;
    int interval_minutes = 30;
    bool operator==(const BackgroundImageConfig&) const = default;
};

struct BackgroundImageSnapshot {
    std::wstring path;     // empty means there is nothing to draw
    uint64_t version = 0;  // incremented for every publish, even the same path
    bool network = false;
    bool busy = false;  // a scan or request is in flight
    std::wstring error; // the most recent failure; path is kept
};

// A shuffled deck of [0, count). Entries inside one deck never repeat, and a
// refilled deck never starts with the entry that ended the previous one.
class BackgroundShuffleBag {
public:
    void Reset(size_t count, std::mt19937& rng);
    void Clear() noexcept;
    bool empty() const noexcept { return order_.empty(); }
    size_t Remaining() const noexcept { return order_.size() - cursor_; }
    size_t Take(std::mt19937& rng);  // refills the deck when exhausted

private:
    std::vector<size_t> order_;
    size_t cursor_ = 0;
    bool has_last_ = false;
    size_t last_ = 0;
};

// A worker result is publishable only when its captured generation is current.
bool IsCurrentBackgroundGeneration(uint64_t captured, uint64_t current) noexcept;

class BackgroundImageService {
public:
    BackgroundImageService();
    ~BackgroundImageService();

    BackgroundImageService(const BackgroundImageService&) = delete;
    BackgroundImageService& operator=(const BackgroundImageService&) = delete;

    bool SetConfig(BackgroundImageConfig config);
    // UI thread only: writes happen from SetConfig/Clear on that thread.
    const BackgroundImageConfig& config() const noexcept { return config_; }
    BackgroundImageSnapshot Snapshot() const;
    void Tick(uint64_t now_ms, bool navigated);
    bool Next();
    void Clear();
    void Shutdown();
    uint64_t NetworkRequestsForTesting() const;

    // Test seams; set before SetConfig. They do not affect production callers.
    void SetFetchForTesting(BackgroundFetchFunction fetch);
    void SetCacheRootForTesting(std::filesystem::path root);

private:
    enum class JobKind : uint8_t { Folder, Network };

    struct Job {
        JobKind kind = JobKind::Folder;
        BackgroundSource source = BackgroundSource::Image;
        uint64_t generation = 0;
        std::wstring folder;
        std::wstring url;
        std::wstring pointer;
    };

    void WorkerMain();
    void ExecuteFolder(const Job& job);
    void ExecuteNetwork(const Job& job, const BackgroundFetchFunction& fetch);
    bool TriggerLocked();
    bool PostLocked(JobKind kind);
    void PublishLocked(std::wstring path, bool network);
    void ClearPathLocked();
    void FailLocked(const std::wstring& error);
    void ResetScheduleLocked();

    // True when both configurations would fetch the same picture; the switch
    // schedule is deliberately excluded from the comparison.
    static bool SameContent(const BackgroundImageConfig& left,
                            const BackgroundImageConfig& right);

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    bool stopping_ = false;
    std::optional<Job> pending_;
    BackgroundImageConfig config_;
    BackgroundImageSnapshot snapshot_;
    BackgroundFetchFunction fetch_ = FetchBackgroundHttp;
    std::filesystem::path cache_root_;
    std::atomic<uint64_t> generation_{0};
    std::atomic<uint64_t> network_requests_{0};
    std::mt19937 rng_;
    BackgroundShuffleBag bag_;
    std::vector<std::wstring> folder_files_;
    std::wstring folder_scanned_;
    bool folder_ready_ = false;
    bool startup_consumed_ = false;
    bool timed_armed_ = false;
    uint64_t timed_due_ms_ = 0;
};

}  // namespace pulse::app

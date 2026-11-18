// app_worker.h — Async enumeration/sort worker with generation tracking.
#pragma once
#include "../fs/fs_enum.h"
#include "io_task_queue.h"
#include "../fs/fs_recycle.h"
#include "../fs/fs_snapshot.h"
#include "../fs/fs_net_cache.h"
#include "../ui/ui_view_model.h"
#include "entry_sort.h"
#include <memory>
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <deque>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace pulse::app {

struct RawListingBatch {
    std::mutex mutex;
    std::condition_variable ready;
    std::atomic_bool started{false};
    bool done = false;
    std::exception_ptr error;
    std::vector<fs::DirEntry> entries;
    fs::DirectoryIdentity identity;
    std::wstring git_root;
    uint64_t listed_at = 0;
    std::vector<std::pair<std::wstring, uint64_t>> subscribers;
};

struct WorkItem {
    std::wstring path;
    std::wstring request_key;
    uint64_t generation;
    ui::SortColumn sort_column;
    ui::SortDirection sort_direction;
    bool load_paths = false;
    bool preserve_order = false;
    std::vector<std::wstring> paths;
    int group_by = 0;
    // The folder's own folders-first mode (Tab::EffectiveFolderSort).
    FolderSortMode folder_sort = FolderSortMode::FoldersFirst;
    std::vector<uint64_t> display_times;
    // Known folder totals for a Size sort (#58); null sorts folders as 0 bytes.
    std::shared_ptr<const FolderSizeLookup> folder_sizes;
    fs::NetSnapshotWrite cache_write;
    std::shared_ptr<RawListingBatch> raw_listing;
    // Classified before taking the queue lock; mapped/cloud drives share UNC quotas.
    bool network_job = false;
};

struct WorkResult {
    std::wstring path;
    uint64_t generation;
    fs::SnapshotPtr snapshot;
    fs::DirectoryIdentity identity;
    std::wstring git_root;
    bool cancelled = false;
    bool error = false;
    bool bitlocker_locked = false; // error: the drive is a locked BitLocker volume
    uint8_t device_error = 0;      // error: fs::DeviceError of a pulse:device: listing
    double enum_ms = 0.0;
    double sort_ms = 0.0;
    uint64_t listed_at = 0;  // fs::WatchClock when a folder listing began
    fs::RecycleBinInfo recycle_info;
};

using ResultCallback = std::function<void(WorkResult)>;

class WorkerPool {
public:
    WorkerPool();
    ~WorkerPool();

    void Start(ResultCallback cb);
    void Stop();
    void CancelGeneration(uint64_t generation);

    // Enqueue a refresh for path. Returns the generation assigned.
    // `folders` defaults to Settings > Folders first; a tab passes its own.
    uint64_t Refresh(const std::wstring& path, ui::SortColumn col, ui::SortDirection dir,
                     int group_by = 0,
                     std::shared_ptr<const FolderSizeLookup> folder_sizes = nullptr,
                     FolderSortMode folders = DefaultFolderSortMode());

    // Independent consumer request; cancel an obsolete request by generation.
    uint64_t LoadPaths(const std::wstring& view_path, std::vector<std::wstring> paths,
                       ui::SortColumn col, ui::SortDirection dir,
                       bool preserve_order = false,
                       std::vector<uint64_t> display_times = {},
                       int group_by = 0,
                       FolderSortMode folders = DefaultFolderSortMode());

    void EnqueueIo(std::function<void()> task);
    void EnqueueSerialIo(std::function<void()> task);

private:
    void WorkerThread();
    WorkResult Process(const WorkItem& item);

    ResultCallback callback_;
    std::vector<std::thread> threads_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<WorkItem> queue_;
    size_t active_network_jobs_ = 0;
    size_t max_network_jobs_ = 1;
    IoTaskQueue io_queue_;
    std::atomic<bool> running_{false};
    bool stopped_ = false;
    uint64_t global_gen_ = 0;
    std::unordered_map<std::wstring, uint64_t> current_gen_;
};

} // namespace pulse::app

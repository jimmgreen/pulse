// app_worker.h — Async enumeration/sort worker with generation tracking.
#pragma once
#include "../fs/fs_enum.h"
#include "io_task_queue.h"
#include "../fs/fs_recycle.h"
#include "../fs/fs_snapshot.h"
#include "../ui/ui_renderer.h"
#include "entry_sort.h"
#include <memory>
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace pulse::app {

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
    std::vector<uint64_t> display_times;
    // Known folder totals for a Size sort (#58); null sorts folders as 0 bytes.
    std::shared_ptr<const FolderSizeLookup> folder_sizes;
};

struct WorkResult {
    std::wstring path;
    uint64_t generation;
    fs::SnapshotPtr snapshot;
    fs::DirectoryIdentity identity;
    std::wstring git_root;
    bool cancelled = false;
    bool error = false;
    // Rows gathered so far by a still-running scan (a big folder or bin). A partial
    // result only refreshes the rows; the final one owns ordering, totals and the store.
    bool partial = false;
    double enum_ms = 0.0;
    double sort_ms = 0.0;
    fs::RecycleBinInfo recycle_info;
};

using ResultCallback = std::function<void(WorkResult)>;

class WorkerPool {
public:
    WorkerPool();
    ~WorkerPool();

    void Start(ResultCallback cb);
    void Stop();

    // Enqueue a refresh for path. Returns the generation assigned.
    uint64_t Refresh(const std::wstring& path, ui::SortColumn col, ui::SortDirection dir,
                     int group_by = 0,
                     std::shared_ptr<const FolderSizeLookup> folder_sizes = nullptr);

    uint64_t LoadPaths(const std::wstring& view_path, std::vector<std::wstring> paths,
                       ui::SortColumn col, ui::SortDirection dir,
                       bool preserve_order = false,
                       std::vector<uint64_t> display_times = {},
                       int group_by = 0);

    void EnqueueIo(std::function<void()> task);
    void EnqueueSerialIo(std::function<void()> task);

private:
    void WorkerThread();
    // emit receives the progressive rows of a long scan; it may be called several
    // times before Process returns the final result (and never for fast scans).
    WorkResult Process(const WorkItem& item, const ResultCallback& emit);

    ResultCallback callback_;
    std::vector<std::thread> threads_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<WorkItem> queue_;
    IoTaskQueue io_queue_;
    std::atomic<bool> running_{false};
    bool stopped_ = false;
    uint64_t global_gen_ = 0;
    std::unordered_map<std::wstring, uint64_t> current_gen_;
};

} // namespace pulse::app

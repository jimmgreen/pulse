// app_worker.h — Async enumeration/sort worker with generation tracking.
#pragma once
#include "../fs/fs_enum.h"
#include "../fs/fs_recycle.h"
#include "../fs/fs_snapshot.h"
#include "../ui/ui_renderer.h"
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
    std::vector<uint64_t> display_times;
};

struct WorkResult {
    std::wstring path;
    uint64_t generation;
    fs::SnapshotPtr snapshot;
    fs::DirectoryIdentity identity;
    std::wstring git_root;
    bool cancelled = false;
    bool error = false;
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
    uint64_t Refresh(const std::wstring& path, ui::SortColumn col, ui::SortDirection dir);

    uint64_t LoadPaths(const std::wstring& view_path, std::vector<std::wstring> paths,
                       ui::SortColumn col, ui::SortDirection dir,
                       bool preserve_order = false,
                       std::vector<uint64_t> display_times = {});

    void EnqueueIo(std::function<void()> task);

private:
    void WorkerThread();
    WorkResult Process(const WorkItem& item);

    ResultCallback callback_;
    std::vector<std::thread> threads_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<WorkItem> queue_;
    std::queue<std::function<void()>> io_queue_;
    std::atomic<bool> running_{false};
    bool stopped_ = false;
    uint64_t global_gen_ = 0;
    std::unordered_map<std::wstring, uint64_t> current_gen_;
};

} // namespace pulse::app

// app_worker.cpp
#include "app_worker.h"
#include "app_model.h"
#include "link_resolve.h"
#include "../fs/fs_enum.h"
#include "../fs/fs_net_cache.h"
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cwctype>

namespace pulse::app {

namespace {

std::wstring WorkKey(const std::wstring& path, ui::SortColumn col,
                     ui::SortDirection dir) {
    std::wstring key = path;
    key.push_back(L'\x1f');
    key += std::to_wstring(static_cast<int>(col));
    key.push_back(L':');
    key += std::to_wstring(static_cast<int>(dir));
    return key;
}

} // namespace

WorkerPool::WorkerPool() = default;

WorkerPool::~WorkerPool() {
    Stop();
}

void WorkerPool::Start(ResultCallback cb) {
    Stop();
    callback_ = std::move(cb);
    running_ = true;
    stopped_ = false;
    const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
    const unsigned count = std::min(4u, hw);
    threads_.reserve(count);
    for (unsigned i = 0; i < count; ++i)
        threads_.emplace_back(&WorkerPool::WorkerThread, this);
}

void WorkerPool::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        stopped_ = true;
    }
    cv_.notify_all();
    for (auto& thread : threads_) if (thread.joinable()) thread.join();
    threads_.clear();
}

uint64_t WorkerPool::Refresh(const std::wstring& path, ui::SortColumn col, ui::SortDirection dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t gen = ++global_gen_;
    const std::wstring key = WorkKey(path, col, dir);
    current_gen_[key] = gen;
    // Only supersede the same path+sort request. Separate panes may show the
    // same directory with different sort orders.
    std::queue<WorkItem> filtered;
    while (!queue_.empty()) {
        if (queue_.front().request_key != key) filtered.push(std::move(queue_.front()));
        queue_.pop();
    }
    queue_ = std::move(filtered);
    queue_.push({ path, key, gen, col, dir });
    cv_.notify_one();
    return gen;
}

uint64_t WorkerPool::LoadPaths(const std::wstring& view_path,
                               std::vector<std::wstring> paths,
                               ui::SortColumn col, ui::SortDirection dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t gen = ++global_gen_;
    const std::wstring key = WorkKey(view_path, col, dir);
    current_gen_[key] = gen;
    std::queue<WorkItem> filtered;
    while (!queue_.empty()) {
        if (queue_.front().request_key != key) filtered.push(std::move(queue_.front()));
        queue_.pop();
    }
    queue_ = std::move(filtered);
    WorkItem item{ view_path, key, gen, col, dir };
    item.load_paths = true;
    item.paths = std::move(paths);
    queue_.push(std::move(item));
    cv_.notify_one();
    return gen;
}

void WorkerPool::EnqueueIo(std::function<void()> task) {
    if (!task) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || stopped_) return;
    io_queue_.push(std::move(task));
    cv_.notify_one();
}

static std::wstring Extension(const std::wstring& name) {
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0 || dot + 1 >= name.size()) return L"";
    std::wstring ext = name.substr(dot + 1);
    for (auto& c : ext) c = std::towlower(c);
    return ext;
}

static int NameCompare(const std::wstring& a, const std::wstring& b) {
    // StrCmpLogicalW gives Explorer-like natural sorting (img2 < img10).
    int cmp = StrCmpLogicalW(a.c_str(), b.c_str());
    if (cmp == 0) cmp = wcscmp(a.c_str(), b.c_str());
    return cmp;
}

static bool CompareEntries(const fs::DirEntry& a, const fs::DirEntry& b,
                           ui::SortColumn col, ui::SortDirection dir) {
    bool aDir = a.is_dir;
    bool bDir = b.is_dir;
    if (aDir != bDir) {
        // Directories always on top.
        return aDir;
    }
    int cmp = 0;
    switch (col) {
    case ui::SortColumn::Name:
        cmp = NameCompare(a.name, b.name);
        break;
    case ui::SortColumn::Size:
        if (a.size < b.size) cmp = -1;
        else if (a.size > b.size) cmp = 1;
        else cmp = NameCompare(a.name, b.name);
        break;
    case ui::SortColumn::Mtime:
        cmp = CompareFileTime(&a.mtime, &b.mtime);
        if (cmp == 0) cmp = NameCompare(a.name, b.name);
        break;
    case ui::SortColumn::Type: {
        std::wstring ea = Extension(a.name);
        std::wstring eb = Extension(b.name);
        cmp = _wcsicmp(ea.c_str(), eb.c_str());
        if (cmp == 0) cmp = NameCompare(a.name, b.name);
        break;
    }
    }
    if (dir == ui::SortDirection::Desc) cmp = -cmp;
    return cmp < 0;
}

WorkResult WorkerPool::Process(const WorkItem& item) {
    WorkResult res;
    res.path = item.path;
    res.generation = item.generation;
    if (!fs::IsVirtualPath(item.path) && !fs::IsUncPath(item.path))
        res.git_root = FindGitRoot(item.path);

    auto t0 = std::chrono::steady_clock::now();
    auto entries = std::make_shared<std::vector<fs::DirEntry>>();
    if (item.load_paths) {
        entries->reserve(item.paths.size());
        for (size_t i = 0; i < item.paths.size(); ++i) {
            if ((i & 127u) == 0) {
                std::lock_guard<std::mutex> lock(mutex_);
                const auto it = current_gen_.find(item.request_key);
                if (!running_ || it == current_gen_.end() || it->second != item.generation) {
                    res.cancelled = true;
                    return res;
                }
            }
            const std::wstring& full = item.paths[i];
            if (full.empty() || fs::IsVirtualPath(full)) continue;
            fs::DirEntry entry;
            entry.full_path = full;
            std::wstring leaf = full;
            if (leaf.starts_with(L"\\\\?\\UNC\\")) leaf = L"\\\\" + leaf.substr(8);
            else if (leaf.starts_with(L"\\\\?\\")) leaf = leaf.substr(4);
            while (leaf.size() > 1 && (leaf.back() == L'\\' || leaf.back() == L'/')) leaf.pop_back();
            const auto slash = leaf.find_last_of(L"\\/");
            entry.name = slash == std::wstring::npos ? leaf : leaf.substr(slash + 1);
            WIN32_FILE_ATTRIBUTE_DATA data{};
            bool ok = GetFileAttributesExW(full.c_str(), GetFileExInfoStandard, &data) != 0;
            if (!ok && leaf != full)
                ok = GetFileAttributesExW(leaf.c_str(), GetFileExInfoStandard, &data) != 0;
            if (ok) {
                entry.attrs = data.dwFileAttributes;
                entry.is_dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                entry.is_reparse = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
                entry.cloud_recall =
                    (data.dwFileAttributes & FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS) != 0;
                entry.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) |
                             data.nFileSizeLow;
                entry.mtime = data.ftLastWriteTime;
            }
            entries->push_back(std::move(entry));
        }
    } else {
        try {
            fs::EnumerateDirectory(item.path, *entries);
        } catch (...) {
            res.error = true;
            res.snapshot = nullptr;
            return res;
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    res.enum_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // Check cancellation before sort.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = current_gen_.find(item.request_key);
        if (it == current_gen_.end() || it->second != item.generation) {
            res.cancelled = true;
            return res;
        }
    }

    // Resolve .lnk targets (Recent folder, desktop shortcuts) before display.
    ResolveLinksInPlace(item.path, *entries, [&] {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = current_gen_.find(item.request_key);
        return !running_ || it == current_gen_.end() || it->second != item.generation;
    });

    auto t2 = std::chrono::steady_clock::now();
    // Interruptible sort: check every 8192 comparisons roughly via chunking.
    // For simplicity do full sort here; generation check after.
    size_t comparisons = 0;
    struct SortCancelled {};
    try {
        std::sort(entries->begin(), entries->end(),
            [&](const fs::DirEntry& a, const fs::DirEntry& b) {
                if ((++comparisons & 8191u) == 0) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    const auto it = current_gen_.find(item.request_key);
                    if (!running_ || it == current_gen_.end() || it->second != item.generation)
                        throw SortCancelled{};
                }
                return CompareEntries(a, b, item.sort_column, item.sort_direction);
            });
    } catch (const SortCancelled&) {
        res.cancelled = true;
        return res;
    }
    auto t3 = std::chrono::steady_clock::now();
    res.sort_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = current_gen_.find(item.request_key);
        if (it == current_gen_.end() || it->second != item.generation) {
            res.cancelled = true;
            return res;
        }
    }

    res.snapshot = std::move(entries);

    // Timing output for large directories (visible in a debugger or ETW).
    if (res.snapshot && res.snapshot->size() >= 10000) {
        wchar_t msg[256];
        swprintf_s(msg, L"[Pulse] %s: %zu items, enum=%.2f ms sort=%.2f ms\n",
            item.path.c_str(), res.snapshot->size(), res.enum_ms, res.sort_ms);
        OutputDebugStringW(msg);
    }

    return res;
}

void WorkerPool::WorkerThread() {
    while (running_) {
        WorkItem item;
        std::function<void()> io_task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&] {
                return stopped_ || !queue_.empty() || !io_queue_.empty() || !running_;
            });
            if (!running_ || stopped_) return;
            if (!queue_.empty()) {
                item = std::move(queue_.front());
                queue_.pop();
            } else if (!io_queue_.empty()) {
                io_task = std::move(io_queue_.front());
                io_queue_.pop();
            } else {
                continue;
            }
        }
        if (io_task) {
            try { io_task(); } catch (...) {}
            continue;
        }
        WorkResult res = Process(item);
        if (!res.cancelled && callback_) {
            const std::wstring cache_path = res.path;
            fs::SnapshotPtr cache_snapshot = res.snapshot;
            try {
                callback_(std::move(res));
            } catch (...) {
                // Ignore.
            }
            if (cache_snapshot && fs::IsUncPath(cache_path))
                fs::SaveNetSnapshot(cache_path, cache_snapshot);
        }
    }
}

} // namespace pulse::app

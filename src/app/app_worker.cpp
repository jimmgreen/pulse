#include "drive_verbs.h"
// app_worker.cpp
#include "app_worker.h"
#include "app_model.h"
#include "entry_sort.h"
#include "entry_group.h"
#include "link_resolve.h"
#include "cloud_roots.h"
#include "device_registry.h"
#include "this_pc_devices.h"
#include "../common/localization.h"
#include "../fs/fs_enum.h"
#include "../fs/bitlocker_volume.h"
#include "../fs/bounded_enumeration.h"
#include "../fs/drive_kind.h"
#include "../fs/fs_recycle.h"
#include "../fs/fs_net_cache.h"
#include "../fs/fs_watch.h"
#include "../common/runtime_log.h"
#include "../common/delete_diagnostics.h"
#include <cstdlib>
#include <exception>
#include <string>
#include "startup_trace.h"
#include <algorithm>
#include <chrono>
#include <cstdio>

namespace pulse::app {

namespace {
#ifdef PULSE_WORKER_BATCH_TEST
std::atomic<unsigned> raw_listing_batches_created{0};
#endif

std::wstring WorkKey(const std::wstring& path, ui::SortColumn col,
                     ui::SortDirection dir, int group_by, FolderSortMode folders) {
    std::wstring key = path;
    key.push_back(L'\x1f');
    key += std::to_wstring(static_cast<int>(col));
    key.push_back(L':');
    key += std::to_wstring(static_cast<int>(dir));
    if (group_by) {
        key.push_back(L'g');
        key += std::to_wstring(group_by);
    }
    key.push_back(L'f');
    key += std::to_wstring(static_cast<int>(folders));
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
    active_network_jobs_ = 0;
    max_network_jobs_ = count - 1;
    threads_.reserve(count);
    for (unsigned i = 0; i < count; ++i)
        threads_.emplace_back(&WorkerPool::WorkerThread, this);
}

void WorkerPool::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        stopped_ = true;
        queue_ = {};
        current_gen_.clear();
    }
    cv_.notify_all();
    for (auto& thread : threads_) if (thread.joinable()) thread.join();
    threads_.clear();
}

void WorkerPool::CancelGeneration(uint64_t generation) {
    std::lock_guard lock(mutex_);
    std::erase_if(current_gen_, [=](const auto& item) { return item.second == generation; });
    std::deque<WorkItem> remaining;
    while (!queue_.empty()) {
        if (queue_.front().generation != generation) remaining.push_back(std::move(queue_.front()));
        queue_.pop_front();
    }
    queue_ = std::move(remaining);
}

uint64_t WorkerPool::Refresh(const std::wstring& path, ui::SortColumn col,
                             ui::SortDirection dir, int group_by,
                             std::shared_ptr<const FolderSizeLookup> folder_sizes,
                             FolderSortMode folders) {
    const bool network_job = fs::IsUncPath(path) || fs::IsRemoteLikeDrive(path);
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t gen = ++global_gen_;
    const std::wstring key = WorkKey(path, col, dir, group_by, folders);
    const bool superseding = current_gen_.contains(key);
    current_gen_[key] = gen;
    // Only supersede the same path+sort request. Separate panes may show the
    // same directory with different sort orders.
    std::deque<WorkItem> filtered;
    while (!queue_.empty()) {
        if (queue_.front().request_key != key) filtered.push_back(std::move(queue_.front()));
        else diagnostics::runtime::Event("navigation_superseded", {{"generation", queue_.front().generation}});
        queue_.pop_front();
    }
    queue_ = std::move(filtered);
    WorkItem work{ path, key, gen, col, dir };
    work.network_job = network_job;
    // Only requests already queued together share physical work. A refresh
    // superseding the same presentation always starts a fresh batch.
    if (!fs::IsVirtualPath(path) && !path.empty()) {
        for (auto& pending : queue_) {
            if (superseding) break;
            if (pending.path == path && pending.request_key != key && !pending.load_paths &&
                (!pending.raw_listing || !pending.raw_listing->started.load())) {
                if (!pending.raw_listing) {
                    pending.raw_listing = std::make_shared<RawListingBatch>();
                    pending.raw_listing->subscribers.emplace_back(pending.request_key, pending.generation);
#ifdef PULSE_WORKER_BATCH_TEST
                    ++raw_listing_batches_created;
#endif
                }
                work.raw_listing = pending.raw_listing;
                break;
            }
        }
        if (work.raw_listing) work.raw_listing->subscribers.emplace_back(key, gen);
    }
    work.cache_write = fs::BeginNetSnapshotWrite(path);
    work.group_by = group_by;
    work.folder_sort = folders;
    work.folder_sizes = std::move(folder_sizes);
    queue_.push_back(std::move(work));
    diagnostics::runtime::Event("navigation_request", {{"generation", gen}, {"load_paths", 0}});
    cv_.notify_one();
    return gen;
}

uint64_t WorkerPool::LoadPaths(const std::wstring& view_path,
                               std::vector<std::wstring> paths,
                               ui::SortColumn col, ui::SortDirection dir,
                               bool preserve_order,
                               std::vector<uint64_t> display_times,
                               int group_by, FolderSortMode folders) {
    const bool network_job = fs::IsUncPath(view_path) || fs::IsRemoteLikeDrive(view_path);
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t gen = ++global_gen_;
    // Virtual consumers can share a path/sort but have different Recent
    // filters and payloads. Each request owns its cancellation identity.
    const std::wstring key = WorkKey(view_path, col, dir, group_by, folders) +
        L"\x1fpaths:" + std::to_wstring(gen);
    current_gen_[key] = gen;
    WorkItem item{ view_path, key, gen, col, dir };
    item.network_job = network_job;
    item.load_paths = true;
    item.preserve_order = preserve_order;
    item.group_by = group_by;
    item.folder_sort = folders;
    item.paths = std::move(paths);
    item.display_times = std::move(display_times);
    queue_.push_back(std::move(item));
    diagnostics::runtime::Event("navigation_request", {{"generation", gen}, {"load_paths", 1}});
    cv_.notify_one();
    return gen;
}

void WorkerPool::EnqueueIo(std::function<void()> task) {
    if (!task) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || stopped_) return;
    io_queue_.Push(std::move(task), false);
    cv_.notify_one();
}

void WorkerPool::EnqueueSerialIo(std::function<void()> task) {
    if (!task) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || stopped_) return;
    io_queue_.Push(std::move(task), true);
    cv_.notify_one();
}

namespace {

// Why a listing failed, for navigation_error (#226: error=1 alone could not
// tell a refusing server from Pulse's own listing quota). fs_enum reports
// "...error=<win32>" or "...status=0x<ntstatus>"; bounded_enumeration.h
// reports a timeout or a full quota.
void LogListingFailure(uint64_t generation, const std::exception_ptr& failure) {
    std::string what;
    try {
        if (failure) std::rethrow_exception(failure);
    } catch (const std::exception& e) {
        what = e.what();
    } catch (...) {
    }
    uint64_t reason = 0;  // 0 other, 1 win32 error, 2 NTSTATUS, 3 timed out, 4 listing quota full
    uint64_t code = 0;
    if (const size_t at = what.find("error="); at != std::string::npos) {
        reason = 1;
        code = std::strtoull(what.c_str() + at + 6, nullptr, 10);
    } else if (const size_t nt = what.find("status=0x"); nt != std::string::npos) {
        reason = 2;
        code = std::strtoull(what.c_str() + nt + 9, nullptr, 16);
    } else if (what.find("timed out") != std::string::npos) {
        reason = 3;
    } else if (what.find("capacity exhausted") != std::string::npos) {
        reason = 4;
    }
    diagnostics::runtime::Event("navigation_error", {{"generation", generation}, {"reason", reason}, {"code", code}});
}

} // namespace

WorkResult WorkerPool::Process(const WorkItem& item) {
    WorkResult res;
    res.path = item.path;
    res.generation = item.generation;
    const auto cancelled = [&] {
        std::lock_guard lock(mutex_);
        auto it = current_gen_.find(item.request_key);
        return !running_ || it == current_gen_.end() || it->second != item.generation;
    };

    if (item.load_paths && !fs::IsVirtualPath(item.path) && !fs::IsUncPath(item.path))
        res.git_root = FindGitRoot(item.path);
    auto t0 = std::chrono::steady_clock::now();
    pulse::app::startup_trace::Mark("worker_listing");
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
                entry.reparse_tag = entry.is_reparse ? fs::ReadReparseTag(leaf) : 0;
                entry.cloud_recall =
                    (data.dwFileAttributes & FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS) != 0;
                entry.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) |
                             data.nFileSizeLow;
                entry.mtime = data.ftLastWriteTime;
                entry.ctime = data.ftCreationTime;
                entry.atime = data.ftLastAccessTime;
            }
            if (i < item.display_times.size() && item.display_times[i] != 0) {
                entry.mtime.dwLowDateTime = static_cast<DWORD>(item.display_times[i]);
                entry.mtime.dwHighDateTime = static_cast<DWORD>(item.display_times[i] >> 32);
            }
            entries->push_back(std::move(entry));
        }
    } else if (fs::IsDevicePath(item.path)) {
        // A phone folder (device_registry.h): listed on the device thread.
        const fs::DeviceError error = pulse::app::DeviceRegistry::Instance().ListFolder(
            item.path, *entries, cancelled);
        if (error == fs::DeviceError::Cancelled) {
            res.cancelled = true;
            return res;
        }
        if (error != fs::DeviceError::None) {
            res.cancelled = cancelled();
            res.error = !res.cancelled;
            res.device_error = static_cast<uint8_t>(error);
            res.snapshot = nullptr;
            return res;
        }
    } else if (fs::IsRecycleViewPath(item.path)) {
        try {
            fs::EnumerateRecycleBin(*entries, &res.recycle_info);
        } catch (...) {
            res.error = true;
            res.snapshot = nullptr;
            return res;
        }
    } else {
        // A watch starting on this folder arms first (briefly bounded): its
        // arm report then needs no second listing (fs::ArmedAt).
        fs::WaitForStartingWatch(item.path, 2, fs::IsUncPath(item.path) ? 150 : 10);
        res.listed_at = fs::WatchClock();
        try {
            struct Listing {
                std::vector<fs::DirEntry> entries;
                fs::DirectoryIdentity identity;
                std::wstring git_root;
            };
            const auto batch_cancelled = [&] {
                if (!item.raw_listing) return cancelled();
                std::lock_guard lock(mutex_);
                if (!running_) return true;
                for (const auto& [key, generation] : item.raw_listing->subscribers) {
                    const auto it = current_gen_.find(key);
                    if (it != current_gen_.end() && it->second == generation) return false;
                }
                return true;
            };
            const auto enumerate = [&] {
            return fs::RunBoundedEnumeration<Listing>(item.network_job,
                [path = item.path](const std::atomic_bool& cancel) {
                    Listing result;
                    fs::EnumerateDirectory(path, result.entries, &cancel);
                    if (cancel) throw fs::EnumerationCancelled();
                    if (path.empty()) {
                        for (const auto& drive : result.entries)
                            if (drive.drive_type) RefreshDriveEjectable(drive.full_path, drive.drive_type);
                        // This PC: the program icons Windows shows there (网盘程序入口).
                        for (auto& launcher : CurrentThisPcLaunchers()) {
                            fs::DirEntry e;
                            e.name = std::move(launcher.label);
                            e.full_path = kThisPcLauncherPrefix + launcher.clsid;
                            e.attrs = FILE_ATTRIBUTE_NORMAL;
                            e.pc_launcher = launcher.brand;
                            result.entries.push_back(std::move(e));
                        }
                        // Phones (the device thread's last listing, device_registry.h);
                        const auto devices = pulse::app::DeviceRegistry::Instance().Devices();
                        app::AppendThisPcDevices(result.entries, devices);
                    }
                    fs::QueryDirectoryIdentity(path, result.identity);
                    if (cancel) throw fs::EnumerationCancelled();
                    if (!fs::IsVirtualPath(path) && !fs::IsUncPath(path))
                        result.git_root = FindGitRoot(path);
                    return result;
                }, batch_cancelled);
            };
            Listing listing;
            if (const auto batch = item.raw_listing) {
                std::unique_lock lock(batch->mutex);
                if (!batch->started) {
                    batch->started = true;
                    batch->listed_at = res.listed_at;
                    lock.unlock();
                    try {
                        auto raw = enumerate();
                        lock.lock();
                        batch->entries = std::move(raw.entries);
                        batch->identity = raw.identity;
                        batch->git_root = std::move(raw.git_root);
                    } catch (...) {
                        lock.lock();
                        batch->error = std::current_exception();
                    }
                    batch->done = true;
                    batch->ready.notify_all();
                } else {
                    while (!batch->done) {
                        if (cancelled()) throw fs::EnumerationCancelled();
                        batch->ready.wait_for(lock, std::chrono::milliseconds(20));
                    }
                }
                if (batch->error) std::rethrow_exception(batch->error);
                listing.entries = batch->entries;
                listing.identity = batch->identity;
                listing.git_root = batch->git_root;
                res.listed_at = batch->listed_at;
            } else listing = enumerate();
            *entries = std::move(listing.entries);
            res.identity = listing.identity;
            res.git_root = std::move(listing.git_root);
        } catch (const fs::EnumerationCancelled&) {
            res.cancelled = true;
            return res;
        } catch (...) {
            {
                std::lock_guard lock(mutex_);
                const auto current = current_gen_.find(item.request_key);
                res.cancelled = !running_ || current == current_gen_.end() || current->second != item.generation;
            }
            if (!res.cancelled) LogListingFailure(item.generation, std::current_exception());
            res.error = !res.cancelled;
            res.bitlocker_locked = res.error && fs::IsBitLockerLocked(item.path);
            res.snapshot = nullptr;
            return res;
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    res.enum_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    diagnostics::runtime::Event("navigation_enumerated", {{"generation", item.generation},
        {"entries", entries->size()}, {"elapsed_us", static_cast<uint64_t>(res.enum_ms * 1000.0)},
        {"directory_id", diagnostics::deletion::Id(diagnostics::deletion::Key(item.path))}});
    if (!item.load_paths)
        diagnostics::deletion::Snapshot(item.path, *entries, item.generation, 0);

    // Check cancellation before sort.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = current_gen_.find(item.request_key);
        if (!running_ || it == current_gen_.end() || it->second != item.generation) {
            res.cancelled = true;
            return res;
        }
    }

    // Read link destinations on this worker before display (including .lnk targets).
    // Not on a phone: reading a .lnk there means copying it off the device.
    if (!fs::IsRecycleViewPath(item.path) && !fs::IsDevicePath(item.path)) {
        ResolveLinksInPlace(item.path, *entries, [&] {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = current_gen_.find(item.request_key);
            return !running_ || it == current_gen_.end() || it->second != item.generation;
        });
    }

    auto t2 = std::chrono::steady_clock::now();
    // Interruptible sort: check every 8192 comparisons roughly via chunking.
    // For simplicity do full sort here; generation check after.
    size_t comparisons = 0;
    struct SortCancelled {};
    if (!item.preserve_order) {
        const ScopedEntryGrouping grouping(item.group_by, item.path);
        const ScopedFolderSortMode folder_mode(item.folder_sort);
        const ScopedMediaSortFolder media_folder(item.path);
        const auto tick = [&] {
            if ((++comparisons & 8191u) == 0) {
                std::lock_guard<std::mutex> lock(mutex_);
                const auto it = current_gen_.find(item.request_key);
                if (!running_ || it == current_gen_.end() || it->second != item.generation)
                    throw SortCancelled{};
            }
        };
        try {
            if (item.folder_sizes && item.sort_column == ui::SortColumn::Size) {
                SortEntriesBySize(*entries, item.sort_direction, *item.folder_sizes, tick);
            } else {
                SortEntries(*entries, item.sort_column, item.sort_direction, tick);
            }
        } catch (const SortCancelled&) {
            res.cancelled = true;
            return res;
        }
    }
    auto t3 = std::chrono::steady_clock::now();
    res.sort_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();
    diagnostics::runtime::Event("navigation_sorted", {{"generation", item.generation},
        {"entries", entries->size()}, {"elapsed_us", static_cast<uint64_t>(res.sort_ms * 1000.0)},
        {"prepare_us", static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count())}});

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = current_gen_.find(item.request_key);
        if (!running_ || it == current_gen_.end() || it->second != item.generation) {
            res.cancelled = true;
            return res;
        }
    }

    pulse::app::startup_trace::Mark("worker_sorted");
    res.snapshot = std::move(entries);
    if (item.load_paths || fs::IsRecycleViewPath(item.path))
        fs::QueryDirectoryIdentity(item.path, res.identity);

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
        IoTaskQueue::Job io_job;
        bool network_job = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            const auto eligible = [&](const WorkItem& candidate) {
                return !candidate.network_job || active_network_jobs_ < max_network_jobs_;
            };
            cv_.wait(lock, [&] {
                return stopped_ || !running_ || io_queue_.Ready() ||
                    std::any_of(queue_.begin(), queue_.end(), eligible);
            });
            if (!running_ || stopped_) return;
            const auto next = std::find_if(queue_.begin(), queue_.end(), eligible);
            if (next != queue_.end()) {
                item = std::move(*next);
                queue_.erase(next);
                network_job = item.network_job;
                if (network_job) ++active_network_jobs_;
            } else if (io_queue_.Ready()) {
                io_job = io_queue_.Pop();
            } else {
                continue;
            }
        }
        if (io_job.task) {
            try { io_job.task(); } catch (...) {}
            { std::lock_guard lock(mutex_); io_queue_.Complete(io_job); }
            cv_.notify_all();
            continue;
        }
        const auto started = GetTickCount64();
        diagnostics::runtime::Event("navigation_start", {{"generation", item.generation}, {"load_paths", item.load_paths}});
        WorkResult res = Process(item);
        {
            std::lock_guard lock(mutex_);
            const auto it = current_gen_.find(item.request_key);
            if (!running_ || it == current_gen_.end() || it->second != item.generation)
                res.cancelled = true;
        }
        diagnostics::runtime::Event("navigation_end", {{"generation", item.generation},
            {"cancelled", res.cancelled}, {"error", res.error},
            {"entries", res.snapshot ? res.snapshot->size() : 0},
            {"has_snapshot", res.snapshot != nullptr}, {"elapsed_ms", GetTickCount64() - started}});
        if (!res.cancelled && callback_) {
            fs::SnapshotPtr cache_snapshot = res.snapshot;
            try {
                callback_(std::move(res));
            } catch (...) {
                // Ignore.
            }
            if (cache_snapshot)
                fs::SaveNetSnapshot(item.cache_write, cache_snapshot);
        }
        // Completed generations no longer participate in cancellation checks.
        // Remove only when no newer request replaced this key while we were
        // processing, so a concurrent refresh remains authoritative.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (network_job) --active_network_jobs_;
            const auto it = current_gen_.find(item.request_key);
            if (it != current_gen_.end() && it->second == item.generation)
                current_gen_.erase(it);
        }
        cv_.notify_all();
    }
}

} // namespace pulse::app

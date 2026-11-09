// transfer_engine.cpp — See transfer_engine.h.
#include "transfer_engine.h"
#include "../common/worker_thread.h"
#include "owned_transfer_output.h"
#include "../fs/drive_kind.h"
#include "../common/runtime_log.h"
#include <winioctl.h>
#include <algorithm>
#include <chrono>

namespace pulse::ops {
namespace {

enum class Storage { Solid, Spinning, Remote, Removable, Unknown };

std::wstring VolumeRoot(const std::wstring& path) {
    if (path.empty()) return {};
    std::vector<wchar_t> root(32768);
    if (!GetVolumePathNameW(path.c_str(), root.data(), static_cast<DWORD>(root.size()))) return {};
    return root.data();
}

Storage StorageOf(const std::wstring& path, uint64_t endpoint = 0) {
    const bool diagnostic = diagnostics::runtime::Enabled();
    const auto started = GetTickCount64();
    uint64_t drive_type = 0, bus = 0, seek_bytes = 0;
    DWORD query_error = 0, bus_error = 0;
    const auto finish = [&](Storage storage, DWORD error = ERROR_SUCCESS) {
        if (diagnostic) diagnostics::runtime::Event("copy_storage_probe", {
            {"storage", static_cast<uint64_t>(storage)}, {"endpoint", endpoint}, {"drive_type", drive_type},
            {"bus_type", bus}, {"error", error}, {"seek_query_error", query_error},
            {"seek_descriptor_bytes", seek_bytes}, {"bus_query_error", bus_error},
            {"elapsed_ms", GetTickCount64() - started}});
        return storage;
    };
    const std::wstring root = VolumeRoot(path);
    if (root.empty()) return finish(Storage::Unknown, GetLastError());
    if (::pulse::fs::IsRemoteLikeDrive(root)) return finish(Storage::Remote);  // includes cloud mounts
    drive_type = GetDriveTypeW(root.c_str());
    switch (drive_type) {
    case DRIVE_REMOTE: return finish(Storage::Remote);
    case DRIVE_REMOVABLE:
    case DRIVE_CDROM: return finish(Storage::Removable);
    case DRIVE_FIXED: break;
    default: return finish(Storage::Unknown);
    }
    // \\?\Volume{...}\ works for drive letters and mount points alike; the
    // device is opened without the trailing backslash and without access.
    wchar_t volume[MAX_PATH]{};
    if (!GetVolumeNameForVolumeMountPointW(root.c_str(), volume, MAX_PATH)) return finish(Storage::Unknown, GetLastError());
    std::wstring device = volume;
    if (!device.empty() && device.back() == L'\\') device.pop_back();
    HANDLE handle = CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return finish(Storage::Unknown, GetLastError());
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    query.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR seek{};
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                                    &seek, sizeof(seek), &returned, nullptr);
    query_error = ok ? ERROR_SUCCESS : GetLastError();
    seek_bytes = returned;
    if (diagnostic) {
        query.PropertyId = StorageDeviceProperty;
        STORAGE_DEVICE_DESCRIPTOR descriptor{};
        DWORD descriptor_bytes = 0;
        const BOOL bus_ok = DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                &descriptor, sizeof(descriptor), &descriptor_bytes, nullptr);
        if (bus_ok && descriptor_bytes >= sizeof(descriptor)) bus = descriptor.BusType;
        else bus_error = bus_ok ? ERROR_INVALID_DATA : GetLastError();
    }
    CloseHandle(handle);
    // A volume over several disks has no single answer.
    if (!ok || returned < sizeof(seek)) return finish(Storage::Unknown, query_error ? query_error : ERROR_INVALID_DATA);
    return finish(seek.IncursSeekPenalty ? Storage::Spinning : Storage::Solid);
}

unsigned ThreadsFor(Storage storage) {
    switch (storage) {
    case Storage::Solid: {
        const unsigned cores = (std::max)(1u, std::thread::hardware_concurrency());
        return (std::min)(8u, (std::max)(2u, 2 * cores));
    }
    case Storage::Removable: return 1;
    // A small file over SMB is a handful of round trips that a single
    // stream spends waiting; four in flight hide most of that latency
    // (twice as fast as two on a loopback share) without turning a NAS
    // disk's reads into seeks.
    case Storage::Remote: return 4;
    default: return 2;
    }
}

constexpr auto kTakeWait = std::chrono::milliseconds(100);

}  // namespace

unsigned TransferThreadCount(const std::wstring& source, const std::wstring& dest_dir) {
    return (std::min)(ThreadsFor(StorageOf(source, 1)), ThreadsFor(StorageOf(dest_dir, 2)));
}

bool IsSolidToSpinningTransfer(const std::wstring& source, const std::wstring& dest_dir) {
    return StorageOf(source, 1) == Storage::Solid && StorageOf(dest_dir, 2) == Storage::Spinning;
}

unsigned MediumTransferThreadCount(const std::wstring& source, const std::wstring& dest_dir) {
    return StorageOf(source, 1) == Storage::Solid && StorageOf(dest_dir, 2) == Storage::Solid ? 4u : 1u;
}

TransferTarget DescribeTransferTarget(const std::wstring& source, const std::wstring& dest_dir) {
    TransferTarget target;
    const Storage storage = StorageOf(dest_dir, 2);
    target.slow = storage == Storage::Remote || storage == Storage::Removable;
    target.spinning = storage == Storage::Spinning;
    const std::wstring source_root = VolumeRoot(source);
    const std::wstring dest_root = VolumeRoot(dest_dir);
    DWORD flags = 0;
    if (!dest_root.empty() && _wcsicmp(source_root.c_str(), dest_root.c_str()) == 0 &&
        GetVolumeInformationW(dest_root.c_str(), nullptr, 0, nullptr, nullptr, &flags, nullptr, 0)) {
        constexpr DWORD kSupportsBlockRefcounting = 0x08000000;  // FILE_SUPPORTS_BLOCK_REFCOUNTING
        target.block_clone = (flags & kSupportsBlockRefcounting) != 0;
    }
    return target;
}

SmallCopyPool::SmallCopyPool(unsigned threads, std::atomic<bool>& cancel, std::atomic<bool>& pause,
                             std::mutex& control_mutex, std::condition_variable& control_cv, bool bounded_medium,
                             std::shared_ptr<SmallCopyPhaseProfile> phase_profile,
                             TransferDeviceBudget::TaskToken budget_task)
    : cancel_(cancel), pause_(pause), control_mutex_(control_mutex), control_cv_(control_cv),
      bounded_medium_(bounded_medium), phase_profile_(std::move(phase_profile)),
      budget_task_(budget_task ? std::move(budget_task) : TransferDeviceBudget::CreateTask()) {
    threads = (std::max)(1u, threads);
    // Bound queued metadata independently of the byte budget.
    capacity_ = bounded_medium_ ? 128 : (std::max<size_t>)(threads * 4, 1024);
    threads_.reserve(threads);
    try {
        for (unsigned i = 0; i < threads; ++i) threads_.push_back(StartWorkerThread([this] { Worker(); }));
    } catch (...) {
        // A failed constructor does not run our destructor. No jobs have been
        // submitted yet, so wake and retire the partially constructed pool.
        {
            std::lock_guard lock(mutex_);
            closing_ = true;
        }
        work_cv_.notify_all();
        for (auto& thread : threads_) thread.join();
        throw;
    }
}

SmallCopyPool::~SmallCopyPool() {
    Stop();
    {
        std::lock_guard lock(mutex_);
        closing_ = true;
    }
    work_cv_.notify_all();
    for (auto& thread : threads_) thread.join();
}

bool SmallCopyPool::HasRoom(uint64_t bytes) const {
    constexpr uint64_t budget = 256ull << 20;
    std::lock_guard lock(mutex_);
    return !stopping_.load() && outstanding_ < capacity_ &&
        (!bounded_medium_ || outstanding_ == 0 || (bytes <= budget && reserved_bytes_ <= budget - bytes));
}

std::vector<SmallCopyProgress> SmallCopyPool::TakeProgress() {
    std::lock_guard lock(mutex_);
    std::vector<SmallCopyProgress> snapshots;
    snapshots.reserve(progress_.size());
    for (const auto& item : progress_) snapshots.push_back(item.second);
    progress_.clear();
    return snapshots;
}

void SmallCopyPool::Submit(SmallCopyJob job) {
    {
        std::lock_guard lock(mutex_);
        if (stopping_) {
            done_.push_back({job.index, false});
        } else {
            queue_.push_back(std::move(job));
        }
        if (bounded_medium_) {
            reservations_[job.index] = job.AdmissionBytes();
            reserved_bytes_ += job.AdmissionBytes();
        }
        ++outstanding_;
    }
    work_cv_.notify_one();
    done_cv_.notify_all();
}

std::vector<SmallCopyResult> SmallCopyPool::Take(bool wait) {
    std::unique_lock lock(mutex_);
    if (wait) done_cv_.wait_for(lock, kTakeWait, [&] { return !done_.empty() || outstanding_ == 0; });
    std::vector<SmallCopyResult> taken;
    taken.swap(done_);
    outstanding_ -= taken.size();
    for (const auto& result : taken) {
        if (bounded_medium_) {
            const auto reservation = reservations_.find(result.index);
            if (reservation != reservations_.end()) {
                reserved_bytes_ -= reservation->second;
                reservations_.erase(reservation);
            }
        }
        progress_.erase(result.index);
    }
    return taken;
}

size_t SmallCopyPool::Outstanding() const {
    std::lock_guard lock(mutex_);
    return outstanding_;
}

SmallCopyQueueSnapshot SmallCopyPool::QueueSnapshot() const {
    std::lock_guard lock(mutex_);
    return {queue_.size(), outstanding_ - queue_.size() - done_.size(), done_.size()};
}

void SmallCopyPool::Stop() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        for (const auto& job : queue_) done_.push_back({job.index, false});
        queue_.clear();
    }
    done_cv_.notify_all();
    control_cv_.notify_all();
}

void SmallCopyPool::Worker() {
    std::unique_ptr<SHA256VerificationWorker> verifier;
    std::unique_ptr<SmallCopyWorkerProfile> profile;
    if (phase_profile_) profile = std::make_unique<SmallCopyWorkerProfile>();
    struct MergeProfile {
        SmallCopyPhaseProfile* shared;
        SmallCopyWorkerProfile* local;
        ~MergeProfile() {
            if (!shared) return;
            std::lock_guard lock(shared->mutex);
            shared->workers.Add(*local);
        }
    } merge_profile{phase_profile_.get(), profile.get()};
    bool received_job = false;
    for (;;) {
        SmallCopyJob job;
        {
            std::unique_lock lock(mutex_);
            const bool measure_empty_wait = profile && queue_.empty();
            std::chrono::steady_clock::time_point wait_started{};
            if (measure_empty_wait) wait_started = std::chrono::steady_clock::now();
            work_cv_.wait(lock, [&] { return closing_ || !queue_.empty(); });
            if (measure_empty_wait) {
                const auto elapsed = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - wait_started).count());
                if (queue_.empty()) profile->queue_close_ns += elapsed;
                else if (received_job) profile->queue_supply_ns += elapsed;
                else profile->queue_start_ns += elapsed;
            }
            if (queue_.empty()) return;
            job = std::move(queue_.front());
            queue_.pop_front();
            received_job = true;
        }
        TransferDeviceBudget::Lease device_lease;
        TransferDeviceBudget::Waiter budget_waiter;
        bool dropped = false;
        bool waited_for_budget = false;
        std::chrono::steady_clock::time_point budget_started{};
        for (;;) {
            while (pause_.load() && !cancel_.load() && !stopping_.load()) {
                budget_waiter.Reset();
                std::unique_lock control(control_mutex_);
                control_cv_.wait_for(control, kTakeWait, [&] {
                    return cancel_.load() || stopping_.load() || !pause_.load();
                });
            }
            if (cancel_.load() || stopping_.load()) { dropped = true; break; }
            if (!job.devices) break;
            device_lease = budget_waiter ? job.devices->budget.TryAcquire(budget_waiter)
                : job.devices->budget.TryAcquire(job.devices->source, job.devices->target, job.AdmissionBytes());
            if (!device_lease && !budget_waiter)
                budget_waiter = job.devices->budget.Wait(job.devices->source, job.devices->target, job.AdmissionBytes(), budget_task_);
            if (device_lease) break;
            if (profile && !waited_for_budget) {
                budget_started = std::chrono::steady_clock::now();
                waited_for_budget = true;
            }
            // Another pool or serial transfer may own the shared permit. The
            // bounded wait observes cancellation; the persistent ticket retains task order.
            if (budget_waiter) budget_waiter.WaitForChange();
            else {
                std::unique_lock control(control_mutex_);
                control_cv_.wait_for(control, std::chrono::milliseconds(2), [&] {
                    return cancel_.load() || stopping_.load() || pause_.load();
                });
            }
        }
        budget_waiter.Reset();
        if (waited_for_budget)
            profile->budget_wait_ns += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - budget_started).count());
        SmallCopyResult result = dropped ? SmallCopyResult{job.index, false} : Run(job, verifier, profile.get());
        // Copy, verification and per-file move flush are finished. Do not make
        // workers depend on the coordinator consuming done_ before they continue.
        device_lease.Reset();
        {
            std::lock_guard lock(mutex_);
            done_.push_back(std::move(result));
        }
        done_cv_.notify_all();
    }
}

namespace {
// NtFlushBuffersFileEx with FLUSH_FLAGS_NO_SYNC (Windows 10 1709+): write the
// file's data and metadata to the device without flushing the device cache.
using FlushBuffersFileEx = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PVOID);
constexpr ULONG kFlushNoSync = 0x2;
FlushBuffersFileEx NoSyncFlush() {
    static const FlushBuffersFileEx function = [] {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll ? reinterpret_cast<FlushBuffersFileEx>(GetProcAddress(ntdll, "NtFlushBuffersFileEx")) : nullptr;
    }();
    return function;
}
std::atomic<bool> g_no_sync_unsupported{false};
}  // namespace

bool FlushCopyToDisk(const std::wstring& path, DWORD attributes, DWORD& error, bool sync) {
    error = ERROR_SUCCESS;
    const bool read_only = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY);
    if (read_only) SetFileAttributesW(path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    bool flushed = false;
    if (file == INVALID_HANDLE_VALUE) {
        error = GetLastError();
    } else {
        const FlushBuffersFileEx no_sync = sync || g_no_sync_unsupported.load() ? nullptr : NoSyncFlush();
        if (no_sync) {
            struct { LONG_PTR status; ULONG_PTR information; } io{};
            const LONG status = no_sync(file, kFlushNoSync, nullptr, 0, &io);
            flushed = status >= 0;
            // Not understood (older Windows, some file systems): a full flush
            // from now on; slower, never less safe.
            if (!flushed) g_no_sync_unsupported.store(true);
        }
        if (!flushed) {
            flushed = FlushFileBuffers(file) != FALSE;
            if (!flushed) error = GetLastError();
        }
        CloseHandle(file);
    }
    if (read_only) SetFileAttributesW(path.c_str(), attributes);
    if (!flushed && (error == ERROR_INVALID_FUNCTION || error == ERROR_NOT_SUPPORTED)) {
        error = ERROR_SUCCESS;  // nothing more this file system can do
        return true;
    }
    return flushed;
}

COPYFILE2_MESSAGE_ACTION CALLBACK SmallCopyPool::Progress(const COPYFILE2_MESSAGE* message, PVOID opaque) {
    const auto& context = *static_cast<ProgressContext*>(opaque);
    auto& pool = *context.pool;
    if (context.owned_output && !*context.owned_output) {
        HANDLE target = INVALID_HANDLE_VALUE;
        switch (message->Type) {
        case COPYFILE2_CALLBACK_STREAM_STARTED: target = message->Info.StreamStarted.hDestinationFile; break;
        case COPYFILE2_CALLBACK_CHUNK_FINISHED: target = message->Info.ChunkFinished.hDestinationFile; break;
        case COPYFILE2_CALLBACK_STREAM_FINISHED: target = message->Info.StreamFinished.hDestinationFile; break;
        default: break;
        }
        if (target != INVALID_HANDLE_VALUE) {
            VerificationStamp stamp;
            if (CaptureOwnedOutputStamp(target, stamp)) *context.owned_output = std::make_shared<VerificationStamp>(stamp);
        }
    }
    while (pool.pause_.load() && !pool.cancel_.load() && !pool.stopping_.load()) {
        std::unique_lock control(pool.control_mutex_);
        pool.control_cv_.wait_for(control, kTakeWait, [&] {
            return !pool.pause_.load() || pool.cancel_.load() || pool.stopping_.load();
        });
    }
    if (pool.cancel_.load() || pool.stopping_.load()) return COPYFILE2_PROGRESS_CANCEL;
    if (message->Type == COPYFILE2_CALLBACK_CHUNK_FINISHED) {
        const auto& chunk = message->Info.ChunkFinished;
        std::lock_guard lock(pool.mutex_);
        pool.progress_[context.index] = {context.index, chunk.uliTotalBytesTransferred.QuadPart,
                                        chunk.uliTotalFileSize.QuadPart};
    }
    return COPYFILE2_PROGRESS_CONTINUE;
}

SmallCopyResult SmallCopyPool::Run(const SmallCopyJob& job, std::unique_ptr<SHA256VerificationWorker>& verifier,
                                  SmallCopyWorkerProfile* profile) {
    SmallCopyResult result;
    result.index = job.index;
    result.ran = true;
    VerificationStamp before_copy;
    if (job.verify) {
        result.verification = std::make_shared<VerificationResult>();
        DWORD error = ERROR_SUCCESS;
        if (!CaptureVerificationStamp(job.source, before_copy, error)) {
            result.verification->error = VerificationError::SourceStat;
            result.verification->win32_error = error;
            result.hr = HRESULT_FROM_WIN32(error ? error : ERROR_READ_FAULT);
            return result;
        }
        if (job.expected_source) {
            const auto& expected = *job.expected_source;
            if (expected.volume != before_copy.volume || expected.index_high != before_copy.index_high ||
                expected.index_low != before_copy.index_low || expected.bytes != before_copy.bytes ||
                CompareFileTime(&expected.last_write, &before_copy.last_write) != 0) {
                result.verification->error = VerificationError::SourceChanged;
                result.hr = HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
                return result;
            }
        }
    }
    ProgressContext context{this, job.index, (job.verify || job.staged || job.unbuffered) ? &result.owned_output : nullptr};
    // Link semantics survive an unsupported no-buffering retry unchanged.
    COPYFILE2_EXTENDED_PARAMETERS parameters{};
    parameters.dwSize = sizeof(parameters);
    parameters.dwCopyFlags = COPY_FILE_COPY_SYMLINK | COPY_FILE_FAIL_IF_EXISTS |
        (job.unbuffered ? COPY_FILE_NO_BUFFERING : 0);
    if (job.verify || job.staged || job.unbuffered || job.bytes >= (1ull << 20)) {
        parameters.pProgressRoutine = Progress;
        parameters.pvCallbackContext = &context;
    }
    const auto copy = [&] {
        if (!profile) return CopyFile2(job.source.c_str(), job.destination.c_str(), &parameters);
        const auto started = std::chrono::steady_clock::now();
        const HRESULT hr = CopyFile2(job.source.c_str(), job.destination.c_str(), &parameters);
        const DWORD error = GetLastError();
        profile->copy_work_ns += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started).count());
        ++profile->copy_calls;
        SetLastError(error);
        return hr;
    };
    result.hr = copy();
    if (result.hr == HRESULT_FROM_WIN32(ERROR_INVALID_PARAMETER) && job.unbuffered) {
        result.buffering_fallback_error = result.hr;
        if (result.owned_output) {
            if (!DeleteVerifiedOutput(job.destination, *result.owned_output, result.buffering_cleanup_error))
                return result; // Preserve the first failure; never retry over an unowned/unclean output.
            result.owned_output.reset();
            std::lock_guard lock(mutex_);
            progress_.erase(job.index);
        }
        parameters.dwCopyFlags &= ~COPY_FILE_NO_BUFFERING;
        result.buffered_retry = true;
        result.hr = copy();
    }
    if (FAILED(result.hr)) {
        // A name lost to another writer is that writer's file.
        // Callback CANCEL already removes the partial destination. A second
        // delete could remove another writer's replacement of that name.
        if (result.hr != HRESULT_FROM_WIN32(ERROR_FILE_EXISTS) &&
            result.hr != HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS) &&
            !(parameters.pProgressRoutine && result.hr == HRESULT_FROM_WIN32(ERROR_REQUEST_ABORTED))) {
            if (job.verify || job.staged || job.unbuffered) {
                DWORD cleanup_error = 0;
                if (result.owned_output) DeleteVerifiedOutput(job.destination, *result.owned_output, cleanup_error);
            } else DeleteFileW(job.destination.c_str());
        }
        return result;
    }
    result.copy_completed = true;
    result.bytes = job.bytes;
    if (job.bytes >= (1ull << 20)) {
        WIN32_FILE_ATTRIBUTE_DATA copied{};
        if (GetFileAttributesExW(job.destination.c_str(), GetFileExInfoStandard, &copied))
            result.bytes = (static_cast<uint64_t>(copied.nFileSizeHigh) << 32) | copied.nFileSizeLow;
    }
    if (job.verify) {
        if (!verifier) verifier = std::make_unique<SHA256VerificationWorker>();
        std::atomic<bool> verification_cancel{cancel_.load() || stopping_.load()};
        const auto wait = [&] {
            while (pause_.load() && !cancel_.load() && !stopping_.load()) {
                std::unique_lock control(control_mutex_);
                control_cv_.wait_for(control, kTakeWait, [&] {
                    return !pause_.load() || cancel_.load() || stopping_.load();
                });
            }
            verification_cancel.store(cancel_.load() || stopping_.load());
        };
        {
            std::lock_guard lock(mutex_);
            progress_[job.index] = {job.index, result.bytes, result.bytes, true};
        }
        *result.verification = verifier->Verify(job.source, job.destination, verification_cancel, wait, &before_copy, result.owned_output.get());
        if (!result.verification->Success()) {
            const auto& verification = *result.verification;
            const DWORD error = verification.error == VerificationError::Cancelled ? ERROR_REQUEST_ABORTED :
                verification.win32_error ? verification.win32_error : ERROR_CRC;
            result.hr = HRESULT_FROM_WIN32(error);
            return result;
        }
        result.bytes = result.verification->source_bytes;
    }
    if (job.move) {
        // Written out here, in parallel; the source is deleted by RunTransfer
        // once a whole batch is durable.
        if (DWORD error = 0; !FlushCopyToDisk(job.destination, job.attributes, error, false)) {
            result.delete_error = error ? error : ERROR_WRITE_FAULT;
            result.not_flushed = true;
            return result;
        }
        result.move_pending = true;
    }
    return result;
}

}  // namespace pulse::ops

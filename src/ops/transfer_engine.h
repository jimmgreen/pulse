// transfer_engine.h — Parallel direct copying for OpsManager::RunTransfer.
//
// RunTransfer keeps everything that has to happen in order on its own
// thread: folders (always created before what they contain), conflicts and
// their prompts, links, large files with progress and pause, verification.
// Eligible direct copies with no conflict are handed to this pool; their results come
// back to the RunTransfer thread, which applies them, so the completed
// lists, undo records and rollback stay in the original order.
#pragma once
#include <windows.h>
#include "verification_worker.h"
#include "transfer_device_budget.h"
#include <memory>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unordered_map>

namespace pulse::ops {

// Copy threads for a transfer from `source` into `dest_dir`: several on
// solid-state storage, 4 on a share, 2 on a spinning disk, 1 on a removable
// drive. When the storage cannot be identified, 2.
unsigned TransferThreadCount(const std::wstring& source, const std::wstring& dest_dir);

bool IsSolidToSpinningTransfer(const std::wstring& source, const std::wstring& dest_dir);

unsigned MediumTransferThreadCount(const std::wstring& source, const std::wstring& dest_dir);

struct TransferTarget {
    bool slow = false;         // a share or a removable drive
    bool spinning = false;     // a fixed disk with a seek penalty (HDD)
    bool block_clone = false;  // same volume, and it clones blocks (ReFS)
};
TransferTarget DescribeTransferTarget(const std::wstring& source, const std::wstring& dest_dir);

// A move deletes its source only once the copy is on the disk itself, not
// just in the cache: after a power loss the file is then in one place or
// the other, never in neither. A disk-cache flush costs a spinning disk
// tens of milliseconds, so it is paid once per batch: each copy is written
// out with `sync` false (data and metadata to the device, no cache flush),
// then one copy per volume with `sync` true makes the whole batch durable,
// and only then are the batch's sources deleted.
// `attributes`: the copy's (a read-only copy is made writable for the flush
// and restored). File systems that cannot flush count as done. False: keep
// the source; `error` says why.
bool FlushCopyToDisk(const std::wstring& path, DWORD attributes, DWORD& error, bool sync = true);

struct SmallCopyJob {
    size_t index = 0;           // entry index in RunTransfer
    std::wstring source;
    std::wstring destination;   // known not to exist; its folder exists
    DWORD attributes = 0;       // as scanned
    bool move = false;          // write the copy out for a move (FlushCopyToDisk, no sync)
    uint64_t bytes = 0;
    bool verify = false;
    std::shared_ptr<const VerificationStamp> expected_source;
    bool staged = false;
    bool unbuffered = false;
    // Null preserves the legacy admission path. Queued jobs do not hold permits.
    std::shared_ptr<const TransferDevicePair> devices;
    // Scheduling weight, not allocated buffer memory or reported file size.
    // Zero keeps the original whole-file admission policy.
    uint64_t admission_bytes = 0;
    uint64_t AdmissionBytes() const noexcept { return admission_bytes ? admission_bytes : bytes; }
};

struct SmallCopyProgress {
    size_t index = 0;
    uint64_t bytes = 0; // absolute snapshot, not a delta
    uint64_t total = 0;
    bool verifying = false;
};

struct SmallCopyResult {
    size_t index = 0;
    bool ran = false;           // false: dropped after Stop() or cancel
    HRESULT hr = S_OK;          // the copy
    DWORD delete_error = ERROR_SUCCESS;  // the source delete of a move
    bool not_flushed = false;   // a move kept its source: the copy could not be written out
    bool move_pending = false;  // a move's copy is written out; RunTransfer deletes the source
    bool source_removed = false;
    uint64_t bytes = 0; // actual logical size when available, otherwise scanned size
    // Allocated only for verification jobs. Failure also sets hr; target cleanup
    // and staging commit belong to the coordinator, never to the verifier.
    std::shared_ptr<VerificationResult> verification;
    bool copy_completed = false;
    HRESULT buffering_fallback_error = S_OK;
    DWORD buffering_cleanup_error = ERROR_SUCCESS;
    bool buffered_retry = false;
    std::shared_ptr<VerificationStamp> owned_output;
};

// Optional diagnostic state. Read worker totals only after the pool has joined.
// Durations are summed worker nanoseconds, not task wall-clock time; waits/calls
// may include user pauses. No profile object is allocated on the default path.
struct SmallCopyWorkerProfile {
    uint64_t queue_start_ns = 0, queue_supply_ns = 0, queue_close_ns = 0;
    uint64_t budget_wait_ns = 0, copy_work_ns = 0, copy_calls = 0;
    void Add(const SmallCopyWorkerProfile& other) {
        queue_start_ns += other.queue_start_ns;
        queue_supply_ns += other.queue_supply_ns;
        queue_close_ns += other.queue_close_ns;
        budget_wait_ns += other.budget_wait_ns;
        copy_work_ns += other.copy_work_ns;
        copy_calls += other.copy_calls;
    }
};
struct SmallCopyPhaseProfile {
    std::mutex mutex;
    SmallCopyWorkerProfile workers;
    uint64_t apply_ns = 0, apply_calls = 0, apply_empty_calls = 0, apply_results = 0;
};
struct SmallCopyQueueSnapshot {
    size_t queued = 0, in_flight = 0, done = 0;
};

class SmallCopyPool {
public:
    // `pause`/`cancel` are RunTransfer's flags; medium copies also check in callbacks.
    // Small files check between files. A paused worker waits on `control_cv` under `control_mutex`.
    SmallCopyPool(unsigned threads, std::atomic<bool>& cancel, std::atomic<bool>& pause,
                  std::mutex& control_mutex, std::condition_variable& control_cv, bool bounded_medium = true,
                  std::shared_ptr<SmallCopyPhaseProfile> phase_profile = {},
                  TransferDeviceBudget::TaskToken budget_task = {});
    ~SmallCopyPool();
    SmallCopyPool(const SmallCopyPool&) = delete;
    SmallCopyPool& operator=(const SmallCopyPool&) = delete;

    // Single coordinator producer: check HasRoom(job.AdmissionBytes()) before Submit.
    // An oversized file may run alone; indices must be unique until Take.
    bool HasRoom(uint64_t bytes = 0) const;
    // Coalesced absolute snapshots; consume before Take, which retires them.
    std::vector<SmallCopyProgress> TakeProgress();
    void Submit(SmallCopyJob job);
    // Finished results so far; with `wait`, blocks until at least one is
    // available or nothing is outstanding.
    std::vector<SmallCopyResult> Take(bool wait);
    size_t Outstanding() const;
    SmallCopyQueueSnapshot QueueSnapshot() const;
    // Queued jobs are dropped; active callback copies are cancelled.
    void Stop();

private:
    void Worker();
    SmallCopyResult Run(const SmallCopyJob& job, std::unique_ptr<SHA256VerificationWorker>& verifier,
                        SmallCopyWorkerProfile* profile);

    std::atomic<bool>& cancel_;
    std::atomic<bool>& pause_;
    std::mutex& control_mutex_;
    std::condition_variable& control_cv_;
    mutable std::mutex mutex_;
    std::condition_variable work_cv_;
    std::condition_variable done_cv_;
    std::deque<SmallCopyJob> queue_;
    std::vector<SmallCopyResult> done_;
    size_t outstanding_ = 0;    // queued + running + done but not taken
    size_t capacity_ = 0;
    std::atomic<bool> stopping_{false};
    const bool bounded_medium_;
    const std::shared_ptr<SmallCopyPhaseProfile> phase_profile_;
    const TransferDeviceBudget::TaskToken budget_task_;
    uint64_t reserved_bytes_ = 0;
    std::unordered_map<size_t, uint64_t> reservations_;
    std::unordered_map<size_t, SmallCopyProgress> progress_;
    struct ProgressContext { SmallCopyPool* pool; size_t index; std::shared_ptr<VerificationStamp>* owned_output; };
    static COPYFILE2_MESSAGE_ACTION CALLBACK Progress(const COPYFILE2_MESSAGE*, PVOID);
    bool closing_ = false;
    std::vector<std::thread> threads_;
};

}  // namespace pulse::ops
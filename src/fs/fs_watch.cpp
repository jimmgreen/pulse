// fs_watch.cpp
#include "fs_watch.h"
#include "fs_enum.h"
#include <winioctl.h>
#include <cfgmgr32.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
// Also for the tests that compile this file directly.
#pragma comment(lib, "cfgmgr32.lib")

namespace pulse::fs {

namespace {

std::vector<DirNotifyEvent> ParseNotifyBuffer(const BYTE* data, DWORD bytes,
                                              std::wstring& pending_old, bool emit_old) {
    std::vector<DirNotifyEvent> out;
    if (!data || bytes < sizeof(FILE_NOTIFY_INFORMATION)) return out;
    const BYTE* p = data;
    const BYTE* end = data + bytes;
    while (p + sizeof(FILE_NOTIFY_INFORMATION) <= end) {
        auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(p);
        const size_t name_chars = info->FileNameLength / sizeof(WCHAR);
        const BYTE* name_end = reinterpret_cast<const BYTE*>(info->FileName) + info->FileNameLength;
        if (name_end > end) break;
        std::wstring name(info->FileName, name_chars);
        if (info->Action == FILE_ACTION_RENAMED_OLD_NAME) {
            pending_old = std::move(name);
            if (emit_old) out.push_back({FILE_ACTION_RENAMED_OLD_NAME, pending_old, {}});
        } else if (info->Action == FILE_ACTION_RENAMED_NEW_NAME) {
            DirNotifyEvent ev;
            ev.action = FILE_ACTION_RENAMED_NEW_NAME;
            ev.name = std::move(name);
            ev.old_name = std::move(pending_old);
            pending_old.clear();
            out.push_back(std::move(ev));
        } else {
            DirNotifyEvent ev;
            ev.action = info->Action;
            ev.name = std::move(name);
            out.push_back(std::move(ev));
        }
        if (info->NextEntryOffset == 0) break;
        p += info->NextEntryOffset;
    }
    return out;
}

} // namespace

namespace { std::atomic<unsigned> active_watchers{0}; constexpr DWORD kOverflowPauseMs = 100; }

namespace {
// Watches that have neither armed nor given up yet.
struct StartingWatch {
    const void* id;
    std::wstring path;
    bool running;  // its thread is opening the folder
};
std::mutex starting_mutex;
std::condition_variable starting_cv;
std::vector<StartingWatch> starting_watches;
} // namespace

uint64_t WatchClock() noexcept {
    return static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
}

void WaitForStartingWatch(const std::wstring& path, DWORD start_ms, DWORD arm_ms) {
    const std::wstring key = NormalizePath(path);
    if (key.empty()) return;
    const auto starting = [&](bool include_queued) {
        return std::any_of(starting_watches.begin(), starting_watches.end(), [&](const StartingWatch& w) {
            return w.path == key && (include_queued || w.running);
        });
    };
    const auto start_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(start_ms);
    std::unique_lock lock(starting_mutex);
    // Polled, not a timed wait: that can oversleep to the next timer tick
    // (~16 ms), which a startup whose watch thread runs late would pay.
    while (!starting(false)) {
        if (!starting(true) || std::chrono::steady_clock::now() >= start_deadline) return;
        lock.unlock();
        std::this_thread::yield();
        lock.lock();
    }
    starting_cv.wait_for(lock, std::chrono::milliseconds(arm_ms), [&] { return !starting(false); });
}

struct DirWatch::State {
    std::wstring path_;
    bool subtree_ = false;
    ChangeCallback callback_;
    std::mutex callback_mutex_;
    std::atomic<HANDLE> hDir_{INVALID_HANDLE_VALUE};
    HANDLE hStop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<bool> running_{true};
    std::promise<void> stopped_;
    std::shared_future<void> stopped_future_ = stopped_.get_future().share();
    std::atomic<bool> armed_{false};
    bool stamp_arm_ = false;
    std::atomic<bool> starting_{false};  // listed in starting_watches
    void BeginStarting() {
        std::lock_guard lock(starting_mutex);
        starting_watches.push_back({this, path_, false});
        starting_ = true;
    }
    void StartingRuns() {
        if (!starting_) return;
        {
            std::lock_guard lock(starting_mutex);
            for (auto& w : starting_watches) if (w.id == this) w.running = true;
        }
        starting_cv.notify_all();
    }
    // Armed, failed or stopped: a listing waiting in WaitForStartingWatch goes on.
    void EndStarting() {
        if (!starting_.exchange(false)) return;
        {
            std::lock_guard lock(starting_mutex);
            std::erase_if(starting_watches, [this](const StartingWatch& w) { return w.id == this; });
        }
        starting_cv.notify_all();
    }
    OVERLAPPED overlapped_{};
    BY_HANDLE_FILE_INFORMATION identity_{};
    bool identity_valid_ = false;
    std::wstring pending_rename_old_;
    DWORD buffer_[64 * 1024 / sizeof(DWORD)]{};
    // Handle lease (an RH oplock) on hDir_. Without it the open handle made
    // renaming or moving this folder's parents fail with ERROR_ACCESS_DENIED,
    // for Pulse's own operations and for every other program. Whoever needs
    // the handle gone breaks the lease and waits; the watch closes it, steps
    // aside and looks again. Volumes that grant no lease are watched as before.
    HANDLE lease_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    OVERLAPPED lease_overlapped_{};
    REQUEST_OPLOCK_OUTPUT_BUFFER lease_output_{};
    bool lease_pending_ = false;
    // Device removal (#251). An open handle on a volume vetoes "Safely Remove"
    // and eject unless it is closed on query-remove, as Explorer does. The
    // callback asks the worker to close the handle and waits until it has;
    // the worker reopens when the removal fails, or after it completes.
    HCMNOTIFICATION removal_ = nullptr;
    HANDLE removal_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);   // query-remove arrived
    HANDLE released_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // handle closed for it
    HANDLE resume_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);    // removal failed or done
    ~State() {
        EndStarting();
        UnwatchRemoval();
        const HANDLE directory = hDir_.load();
        if (directory != INVALID_HANDLE_VALUE) CloseHandle(directory);
        if (hStop_) CloseHandle(hStop_);
        if (lease_event_) CloseHandle(lease_event_);
        for (HANDLE event : {removal_event_, released_event_, resume_event_})
            if (event) CloseHandle(event);
        --active_watchers;
        stopped_.set_value();
    }
    static HANDLE OpenDirectory(const std::wstring& path);
    static bool ReadIdentity(HANDLE handle, BY_HANDLE_FILE_INFORMATION& identity);
    bool WatchedPathReplaced() const;
    bool ReopenDirectory();
    void RequestLease(HANDLE directory, DWORD level, DWORD flags);
    void DropLease(HANDLE directory);
    bool LeaseBreakWantsHandle(HANDLE directory);
    void WatchRemoval(HANDLE directory);
    void UnwatchRemoval();
    static DWORD CALLBACK RemovalCallback(HCMNOTIFICATION, PVOID context, CM_NOTIFY_ACTION action,
                                          PCM_NOTIFY_EVENT_DATA, DWORD);
    void Notify(bool overflow, std::vector<DirNotifyEvent> events);
    void WorkerThread();
};

DirWatch::DirWatch() = default;
DirWatch::~DirWatch() { Stop(); }

bool DirWatch::Start(const std::wstring& path, ChangeCallback cb, bool subtree, bool stamp_arm) {
    Stop();
    if (path.empty()) return false;
    // Bound retired workers even when a network provider ignores cancellation.
    if (active_watchers.fetch_add(1) >= 128) { --active_watchers; return false; }
    std::shared_ptr<State> state;
    try { state = std::make_shared<State>(); }
    catch (...) { --active_watchers; return false; }
    if (!state->hStop_ || !state->removal_event_ || !state->released_event_ || !state->resume_event_)
        return false;
    path_ = NormalizePath(path);
    state->path_ = path_;
    state->subtree_ = subtree;
    state->callback_ = std::move(cb);
    state->stamp_arm_ = stamp_arm;
    if (stamp_arm) state->BeginStarting();
    try { thread_ = std::thread([state] { state->WorkerThread(); }); }
    catch (...) { state->EndStarting(); return false; }
    state_ = std::move(state);
    return true;
}

bool DirWatch::Armed() const { return state_ && state_->armed_.load(); }

void DirWatch::Stop() {
    (void)StopAsync();
}

std::shared_future<void> DirWatch::StopAsync() {
    if (!state_) return {};
    auto completion = state_->stopped_future_;
    state_->EndStarting();
    state_->running_ = false;
    SetEvent(state_->hStop_);
    {
        // Callbacks only dispatch notifications. Quiesce them before owners die.
        std::lock_guard lock(state_->callback_mutex_);
        state_->callback_ = {};
    }
    if (thread_.joinable()) {
        CancelSynchronousIo(thread_.native_handle());
        thread_.detach();
    }
    state_.reset(); // The worker owns handles and pending OVERLAPPED until drained.
    return completion;
}

HANDLE DirWatch::State::OpenDirectory(const std::wstring& path) {
    return CreateFileW(
        path.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);
}

bool DirWatch::State::ReadIdentity(HANDLE handle, BY_HANDLE_FILE_INFORMATION& identity) {
    return handle != INVALID_HANDLE_VALUE &&
           GetFileInformationByHandle(handle, &identity) != FALSE;
}

bool DirWatch::State::WatchedPathReplaced() const {
    if (!identity_valid_) return false;
    HANDLE current = OpenDirectory(path_);
    if (current == INVALID_HANDLE_VALUE) return true;
    BY_HANDLE_FILE_INFORMATION identity{};
    const bool readable = ReadIdentity(current, identity);
    CloseHandle(current);
    if (!readable) return true;
    return identity.dwVolumeSerialNumber != identity_.dwVolumeSerialNumber ||
           identity.nFileIndexHigh != identity_.nFileIndexHigh ||
           identity.nFileIndexLow != identity_.nFileIndexLow;
}

bool DirWatch::State::ReopenDirectory() {
    armed_ = false;
    const HANDLE old = hDir_.exchange(INVALID_HANDLE_VALUE, std::memory_order_acq_rel);
    DropLease(old);
    if (old != INVALID_HANDLE_VALUE) CloseHandle(old);
    UnwatchRemoval();
    identity_valid_ = false;

    while (running_) {
        HANDLE directory = OpenDirectory(path_);
        if (directory != INVALID_HANDLE_VALUE) {
            BY_HANDLE_FILE_INFORMATION identity{};
            if (ReadIdentity(directory, identity)) {
                identity_ = identity;
                identity_valid_ = true;
                hDir_.store(directory, std::memory_order_release);
                RequestLease(directory, OPLOCK_LEVEL_CACHE_READ | OPLOCK_LEVEL_CACHE_HANDLE,
                             REQUEST_OPLOCK_INPUT_FLAG_REQUEST);
                WatchRemoval(directory);
                return true;
            }
            CloseHandle(directory);
        }
        if (WaitForSingleObject(hStop_, 1000) == WAIT_OBJECT_0) break;
    }
    return false;
}

void DirWatch::State::RequestLease(HANDLE directory, DWORD level, DWORD flags) {
    lease_pending_ = false;
    // A share keeps its handle semantics on the server.
    if (!lease_event_ || directory == INVALID_HANDLE_VALUE || path_.starts_with(L"\\\\?\\UNC\\") ||
        !path_.starts_with(L"\\\\?\\"))
        return;
    REQUEST_OPLOCK_INPUT_BUFFER input{};
    input.StructureVersion = REQUEST_OPLOCK_CURRENT_VERSION;
    input.StructureLength = sizeof(input);
    input.RequestedOplockLevel = level;
    input.Flags = flags;
    lease_output_ = {};
    ZeroMemory(&lease_overlapped_, sizeof(lease_overlapped_));
    lease_overlapped_.hEvent = lease_event_;
    ResetEvent(lease_event_);
    const BOOL done = DeviceIoControl(directory, FSCTL_REQUEST_OPLOCK, &input, sizeof(input), &lease_output_,
                                      sizeof(lease_output_), nullptr, &lease_overlapped_);
    // Pending is the granted state: the request completes when the lease breaks.
    lease_pending_ = !done && GetLastError() == ERROR_IO_PENDING;
}

void DirWatch::State::DropLease(HANDLE directory) {
    if (!lease_pending_) return;
    lease_pending_ = false;
    CancelIoEx(directory, &lease_overlapped_);
    WaitForSingleObject(lease_event_, INFINITE);  // the kernel is done with the OVERLAPPED
}

// The lease broke. A break that keeps read caching and takes the handle away
// means a rename or move of this folder or a folder above it, or an open that
// conflicts with ours, is waiting for the handle. A folder's contents changing
// breaks the whole lease instead: keep the handle and its pending read, renew.
bool DirWatch::State::LeaseBreakWantsHandle(HANDLE directory) {
    lease_pending_ = false;
    if (lease_overlapped_.Internal != 0) return false;  // refused or cancelled: watch without one
    const DWORD level = lease_output_.NewOplockLevel;
    if ((level & OPLOCK_LEVEL_CACHE_READ) && !(level & OPLOCK_LEVEL_CACHE_HANDLE)) return true;
    if (lease_output_.Flags & REQUEST_OPLOCK_OUTPUT_FLAG_ACK_REQUIRED) {
        // An acknowledgment to a level above none stays pending for the next break.
        RequestLease(directory, level, REQUEST_OPLOCK_INPUT_FLAG_ACK);
        if (lease_pending_) return false;
    }
    RequestLease(directory, OPLOCK_LEVEL_CACHE_READ | OPLOCK_LEVEL_CACHE_HANDLE, REQUEST_OPLOCK_INPUT_FLAG_REQUEST);
    return false;
}

void DirWatch::State::WatchRemoval(HANDLE directory) {
    // Shares have no local device to remove.
    if (removal_ || !removal_event_ || !released_event_ || !resume_event_ ||
        path_.starts_with(L"\\\\?\\UNC\\") || !path_.starts_with(L"\\\\?\\"))
        return;
    ResetEvent(removal_event_);
    ResetEvent(resume_event_);
    CM_NOTIFY_FILTER filter{};
    filter.cbSize = sizeof(filter);
    filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEHANDLE;
    filter.u.DeviceHandle.hTarget = directory;
    if (CM_Register_Notification(&filter, this, &State::RemovalCallback, &removal_) != CR_SUCCESS)
        removal_ = nullptr;
}

void DirWatch::State::UnwatchRemoval() {
    if (!removal_) return;
    // Waits for a callback in flight; never called from the callback itself.
    if (released_event_) SetEvent(released_event_);
    CM_Unregister_Notification(removal_);
    removal_ = nullptr;
}

DWORD CALLBACK DirWatch::State::RemovalCallback(HCMNOTIFICATION, PVOID context, CM_NOTIFY_ACTION action,
                                                PCM_NOTIFY_EVENT_DATA, DWORD) {
    auto* state = static_cast<State*>(context);
    switch (action) {
    case CM_NOTIFY_ACTION_DEVICEQUERYREMOVE:
        ResetEvent(state->released_event_);
        SetEvent(state->removal_event_);
        // The worker's wait wakes at once; give up rather than stall the removal.
        WaitForSingleObject(state->released_event_, 5000);
        break;
    case CM_NOTIFY_ACTION_DEVICEQUERYREMOVEFAILED:
    case CM_NOTIFY_ACTION_DEVICEREMOVEPENDING:
    case CM_NOTIFY_ACTION_DEVICEREMOVECOMPLETE:
        SetEvent(state->resume_event_);
        break;
    default:
        break;
    }
    return ERROR_SUCCESS;
}

void DirWatch::State::Notify(bool overflow, std::vector<DirNotifyEvent> events) {
    std::lock_guard lock(callback_mutex_);
    if (!running_ || !callback_) return;
    try {
        callback_(overflow, std::move(events));
    } catch (...) {
    }
}

void DirWatch::State::WorkerThread() {
    struct StartingEnd {
        State* state;
        ~StartingEnd() { state->EndStarting(); }
    } starting_end{this};
    StartingRuns();
    if (!ReopenDirectory()) return;
    bool newly_armed = true;
    DWORD retry_delay = 250;
    bool failure_notified = false;
    const auto recovered = [&] {
        if (failure_notified) Notify(true, {});
        failure_notified = false;
        retry_delay = 250;
        // Publish readiness only after the missed-change reconciliation has
        // been delivered. A queued read alone can still fail immediately.
        armed_ = true;
    };
    const auto retry_failure = [&](DWORD error) {
        EndStarting();
        armed_ = false;
        pending_rename_old_.clear();
        if (!failure_notified) Notify(true, {});
        failure_notified = true;
        const bool unsupported = error == ERROR_INVALID_FUNCTION || error == ERROR_NOT_SUPPORTED;
        const DWORD delay = unsupported ? 30000 : retry_delay;
        if (WaitForSingleObject(hStop_, delay) != WAIT_TIMEOUT || !running_) return false;
        if (unsupported) Notify(true, {}); // Bounded fallback for local providers as well as UNC.
        else retry_delay = (std::min)(retry_delay * 2, DWORD{8000});
        return ReopenDirectory();
    };
    // More changes than the buffer holds (#58-12): a big folder taking a bulk
    // copy. The handle is fine and the system keeps recording; only the
    // listing has to be read again. Reopening with the failure backoff here
    // left the view up to eight seconds behind. The short pause bounds a
    // provider that reports this on every read.
    const auto overflowed = [&] {
        pending_rename_old_.clear();
        Notify(true, {});
        return WaitForSingleObject(hStop_, kOverflowPauseMs) == WAIT_TIMEOUT && running_;
    };
    while (running_) {
        ZeroMemory(&overlapped_, sizeof(overlapped_));
        overlapped_.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped_.hEvent) break;
        HANDLE events[4] = { hStop_, overlapped_.hEvent, removal_event_, lease_event_ };
        const HANDLE directory = hDir_.load(std::memory_order_acquire);
        if (directory == INVALID_HANDLE_VALUE) {
            CloseHandle(overlapped_.hEvent);
            break;
        }

        BOOL ok = ReadDirectoryChangesW(
            directory,
            buffer_,
            static_cast<DWORD>(sizeof(buffer_)),
            subtree_ ? TRUE : FALSE,
            FILE_NOTIFY_CHANGE_FILE_NAME |
                FILE_NOTIFY_CHANGE_DIR_NAME |
                FILE_NOTIFY_CHANGE_ATTRIBUTES |
                FILE_NOTIFY_CHANGE_SIZE |
                FILE_NOTIFY_CHANGE_LAST_WRITE |
                FILE_NOTIFY_CHANGE_CREATION |
                FILE_NOTIFY_CHANGE_SECURITY,
            nullptr,
            &overlapped_,
            nullptr);

        const DWORD read_error = ok ? ERROR_SUCCESS : GetLastError();
        if (!ok && read_error != ERROR_IO_PENDING) {
            CloseHandle(overlapped_.hEvent);
            overlapped_.hEvent = nullptr;
            if (!running_) break;
            if (read_error == ERROR_NOTIFY_ENUM_DIR) {
                if (!overflowed()) break;
                continue;
            }
            if (!retry_failure(read_error)) break;
            newly_armed = true;
            continue;
        }

        // An initial/reopened watch may have missed changes while
        // the handle was opening. Reconcile only after the read is armed.
        if (!failure_notified) armed_ = true;
        if (newly_armed && !failure_notified) {
            std::vector<DirNotifyEvent> arm;
            if (stamp_arm_) {
                // Stamped after the read is queued: any later change is reported.
                arm.emplace_back().armed_at = WatchClock();
                EndStarting();
            }
            Notify(true, std::move(arm));
        }
        newly_armed = false;

        bool replaced = false;
        bool stopped = false;
        bool stepped_aside = false;
        bool removing = false;
        for (;;) {
            // A held handle lease breaks as soon as the directory is renamed or
            // deleted, so the identity check is only a slow backstop then;
            // without one (shares, some file systems) it stays at 500 ms.
            const DWORD wait = WaitForMultipleObjects(lease_pending_ ? 4 : 3, events, FALSE,
                                                      lease_pending_ ? 5000 : 500);
            if (wait == WAIT_OBJECT_0 + 2) {
                CancelIoEx(directory, &overlapped_);
                WaitForSingleObject(overlapped_.hEvent, INFINITE);
                removing = true;
                break;
            }
            if (wait == WAIT_OBJECT_0 + 3) {
                if (!LeaseBreakWantsHandle(directory)) continue;
                CancelIoEx(directory, &overlapped_);
                WaitForSingleObject(overlapped_.hEvent, INFINITE);
                stepped_aside = true;
                break;
            }
            if (wait == WAIT_TIMEOUT) {
                if (!WatchedPathReplaced()) {
                    recovered(); // A pending read survived its first identity check.
                    continue;
                }
                CancelIoEx(directory, &overlapped_);
                WaitForSingleObject(overlapped_.hEvent, INFINITE);
                replaced = true;
                break;
            }
            if (wait == WAIT_OBJECT_0) {
                CancelIoEx(directory, &overlapped_);
                WaitForSingleObject(overlapped_.hEvent, INFINITE);
                stopped = true;
                break;
            }
            if (wait != WAIT_OBJECT_0 + 1) {
                CancelIoEx(directory, &overlapped_);
                WaitForSingleObject(overlapped_.hEvent, INFINITE);
            }
            break;
        }

        DWORD transferred = 0;
        const BOOL got = GetOverlappedResult(directory, &overlapped_, &transferred, FALSE);
        const DWORD result_error = got ? ERROR_SUCCESS : GetLastError();
        CloseHandle(overlapped_.hEvent);
        overlapped_.hEvent = nullptr;

        if (stopped || !running_) break;

        if (removing) {
            // Close for the removal, then stay closed until it fails (reopen
            // now) or completes (reopen once the volume is back).
            armed_ = false;
            pending_rename_old_.clear();
            const HANDLE old = hDir_.exchange(INVALID_HANDLE_VALUE, std::memory_order_acq_rel);
            DropLease(old);
            if (old != INVALID_HANDLE_VALUE) CloseHandle(old);
            SetEvent(released_event_);
            HANDLE resume[2] = { hStop_, resume_event_ };
            if (WaitForMultipleObjects(2, resume, FALSE, 60000) == WAIT_OBJECT_0 || !running_) break;
            if (!ReopenDirectory()) break;
            newly_armed = true;
            continue;
        }

        if (stepped_aside) {
            // Close now so the waiting rename or move goes ahead, and let it
            // finish before opening the path again.
            armed_ = false;
            pending_rename_old_.clear();
            const HANDLE old = hDir_.exchange(INVALID_HANDLE_VALUE, std::memory_order_acq_rel);
            if (old != INVALID_HANDLE_VALUE) CloseHandle(old);
            if (WaitForSingleObject(hStop_, 300) != WAIT_TIMEOUT || !running_) break;
            Notify(true, {});
            if (!ReopenDirectory()) break;
            newly_armed = true;
            continue;
        }

        if (replaced) {
            pending_rename_old_.clear();
            Notify(true, {});
            if (!ReopenDirectory()) break;
            newly_armed = true;
            continue;
        }

        if (!got && result_error == ERROR_NOTIFY_ENUM_DIR) {
            recovered();
            if (!overflowed()) break;
            continue;
        }
        if (!got) {
            if (!retry_failure(result_error)) break;
            newly_armed = true;
            continue;
        }

        recovered();
        if (transferred == 0) {
            pending_rename_old_.clear();
            Notify(true, {});
            continue;
        }
        Notify(false, ParseNotifyBuffer(reinterpret_cast<const BYTE*>(buffer_), transferred,
                                        pending_rename_old_, subtree_));
    }
    armed_ = false;
    DropLease(hDir_.load(std::memory_order_acquire));
    UnwatchRemoval();
}

void DirWatchSet::Sync(const std::vector<std::wstring>& paths, Callback cb) {
    std::erase_if(retiring_, [](const auto& item) {
        return !item.second.valid() || item.second.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
    });
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback_ = std::move(cb);
    }
    std::vector<std::wstring> wanted;
    wanted.reserve(paths.size());
    for (const auto& path : paths) {
        if (path.empty() || IsVirtualPath(path)) continue;
        auto key = NormalizePath(path);
        if (std::any_of(suspended_roots_.begin(), suspended_roots_.end(), [&](const auto& root) {
            return _wcsicmp(key.c_str(), root.c_str()) == 0 || (key.size() > root.size() && _wcsnicmp(key.c_str(), root.c_str(), root.size()) == 0 &&
                (root.back() == L'\\' || key[root.size()] == L'\\'));
        })) continue;
        wanted.push_back(std::move(key));
    }
    std::sort(wanted.begin(), wanted.end());
    wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());

    for (auto it = watches_.begin(); it != watches_.end();) {
        if (std::binary_search(wanted.begin(), wanted.end(), it->first)) ++it;
        else {
            retiring_.emplace_back(it->first, it->second->StopAsync());
            it = watches_.erase(it);
        }
    }
    for (const auto& path : wanted) {
        if (watches_.contains(path)) continue;
        auto watch = std::make_unique<DirWatch>();
        const std::wstring key = path;
        if (!watch->Start(path, [this, key](bool overflow, std::vector<DirNotifyEvent> events) {
            Callback invoke;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                invoke = callback_;
            }
            if (invoke) invoke(key, overflow, std::move(events));
        }, false, true)) {
            continue;
        }
        watches_[path] = std::move(watch);
    }
}

void DirWatchSet::Stop() {
    watches_.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = {};
}

std::vector<std::shared_future<void>> DirWatchSet::SuspendVolume(const std::wstring& root) {
    const auto key = NormalizePath(root);
    std::vector<std::shared_future<void>> result;
    if (key.empty()) return result;
    for (const auto& [path, future] : retiring_) {
        if (_wcsicmp(path.c_str(), key.c_str()) == 0 || (path.size() > key.size() && _wcsnicmp(path.c_str(), key.c_str(), key.size()) == 0 &&
            (key.back() == L'\\' || path[key.size()] == L'\\'))) result.push_back(future);
    }
    if (std::find(suspended_roots_.begin(), suspended_roots_.end(), key) == suspended_roots_.end())
        suspended_roots_.push_back(key);
    for (auto it = watches_.begin(); it != watches_.end();) {
        const auto& path = it->first;
        if (_wcsicmp(path.c_str(), key.c_str()) == 0 || (path.size() > key.size() && _wcsnicmp(path.c_str(), key.c_str(), key.size()) == 0 &&
            (key.back() == L'\\' || path[key.size()] == L'\\'))) {
            auto completion = it->second->StopAsync();
            result.push_back(completion);
            retiring_.emplace_back(path, std::move(completion));
            it = watches_.erase(it);
        } else ++it;
    }
    return result;
}

void DirWatchSet::ResumeVolume(const std::wstring& root) {
    const auto key = NormalizePath(root);
    std::erase_if(suspended_roots_, [&](const auto& value) { return _wcsicmp(value.c_str(), key.c_str()) == 0; });
}

bool DirWatchSet::Armed(const std::wstring& path) const {
    const auto it = watches_.find(NormalizePath(path));
    return it != watches_.end() && it->second && it->second->Armed();
}

} // namespace pulse::fs

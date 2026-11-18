// fs_watch.h — ReadDirectoryChangesW directory watcher.
// Overlapped reads return ERROR_IO_PENDING until a change arrives; that is
// success, not a reason to reopen the directory. CreateFile runs on the
// watch thread so UNC/SMB cannot stall the UI.
#pragma once
#include <windows.h>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <vector>
#include <future>

namespace pulse::fs {
struct DirNotifyEvent;
// Overflow supersedes all pending details for this path. Keep one queue item
// while a UI consumer is paused, instead of accumulating repeated refreshes.
template<class Queue>
void QueueDirectoryNotification(Queue& queue, const std::wstring& path, bool overflow,
                                std::vector<DirNotifyEvent> events);


struct DirNotifyEvent {
    DWORD action = 0;
    std::wstring name;
    std::wstring old_name;
    // Only on the lone event of the overflow a stamp_arm watch reports once its
    // read is armed: when that was (WatchClock). A listing begun later holds
    // every change before it, so the overflow then asks for nothing (ArmedAt).
    uint64_t armed_at = 0;
};

// Clock of armed_at and of listing start times (steady, process-wide).
uint64_t WatchClock() noexcept;
// The arm time of an overflow that only reports a newly armed watch, else 0.
inline uint64_t ArmedAt(bool overflow, const std::vector<DirNotifyEvent>& events) noexcept {
    return overflow && events.size() == 1 ? events.front().armed_at : 0;
}
// Before listing a folder: while a stamp_arm watch on it is starting, wait
// for it to arm so the listing begins after that. A watch whose thread is not
// running within start_ms (a busy startup) is not waited for; one that runs
// gets up to arm_ms more.
void WaitForStartingWatch(const std::wstring& path, DWORD start_ms, DWORD arm_ms);

template<class Queue>
void QueueDirectoryNotification(Queue& queue, const std::wstring& path, bool overflow,
                                std::vector<DirNotifyEvent> events) {
    for (auto it = queue.begin(); it != queue.end();) {
        if (it->path != path) { ++it; continue; }
        if (!overflow && it->overflow) return;
        if (overflow) it = queue.erase(it);
        else ++it;
    }
    queue.push_back({path, overflow, std::move(events)});
}


class DirWatch {
public:
    using ChangeCallback = std::function<void(bool overflow, std::vector<DirNotifyEvent> events)>;

    DirWatch();
    ~DirWatch();

    // stamp_arm: the overflow reported once the read is armed carries the arm
    // time (DirNotifyEvent::armed_at), and WaitForStartingWatch waits for it.
    bool Start(const std::wstring& path, ChangeCallback cb, bool subtree = false,
               bool stamp_arm = false);
    void Stop();
    // Request stop without joining. Ready means all owned handles are closed.
    std::shared_future<void> StopAsync();
    bool Armed() const;
    const std::wstring& path() const { return path_; }

private:
    struct State;
    std::shared_ptr<State> state_;
    std::wstring path_;
    std::thread thread_;

};

class DirWatchSet {
public:
    using Callback = std::function<void(const std::wstring& path, bool overflow,
                                        std::vector<DirNotifyEvent> events)>;

    void Sync(const std::vector<std::wstring>& paths, Callback cb);
    void Stop();
    // Owner-thread calls. Wait on returned futures off-thread; Resume requires Sync.
    std::vector<std::shared_future<void>> SuspendVolume(const std::wstring& root);
    void ResumeVolume(const std::wstring& root);
    bool Armed(const std::wstring& path) const;

private:
    std::mutex mutex_;
    Callback callback_;
    std::unordered_map<std::wstring, std::unique_ptr<DirWatch>> watches_;
    std::vector<std::wstring> suspended_roots_;
    std::vector<std::pair<std::wstring, std::shared_future<void>>> retiring_;
};

} // namespace pulse::fs

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>
#include "../app/dir_notify_queue.h"
#include "../fs/fs_enum.h"

namespace {
std::atomic<int> attempts{0};
std::atomic<DWORD> injected_error{ERROR_INVALID_FUNCTION};
bool async_failure = false;
HANDLE reached = nullptr, release_read = nullptr;
std::mutex delays_mutex;
std::vector<DWORD> delays;
int wait_limit = 4;
BOOL WINAPI TestRead(HANDLE directory, LPVOID buffer, DWORD length, BOOL subtree,
                    DWORD filter, LPDWORD bytes, LPOVERLAPPED overlapped,
                    LPOVERLAPPED_COMPLETION_ROUTINE completion) {
    const int attempt = ++attempts;
    if (attempt == 20) {
        SetEvent(reached); // Bound the unfixed hot loop without hiding failure.
        WaitForSingleObject(release_read, 5000);
    }
    const DWORD error = injected_error.load();
    if (error == ERROR_SUCCESS || (error == ERROR_BUSY && attempt > 3))
        return ReadDirectoryChangesW(directory, buffer, length, subtree, filter, bytes, overlapped, completion);
    if (async_failure) { SetEvent(overlapped->hEvent); return TRUE; }
    SetLastError(error);
    return FALSE;
}
BOOL WINAPI TestResult(HANDLE directory, LPOVERLAPPED overlapped, LPDWORD bytes, BOOL wait) {
    if (async_failure) { SetLastError(injected_error); return FALSE; }
    return GetOverlappedResult(directory, overlapped, bytes, wait);
}
DWORD WINAPI TestWait(HANDLE event, DWORD duration) {
    if (duration != INFINITE) {
        size_t count;
        { std::lock_guard lock(delays_mutex); delays.push_back(duration); count = delays.size(); }
        if (count >= static_cast<size_t>(wait_limit)) {
            SetEvent(reached);
            return WaitForSingleObject(event, duration); // Stop must wake the real wait.
        }
        return WaitForSingleObject(event, 2); // Keep policy tests fast; record original deadlines.
    }
    return WaitForSingleObject(event, duration);
}
}
#define ReadDirectoryChangesW TestRead
#define GetOverlappedResult TestResult
#define WaitForSingleObject TestWait
#include "../fs/fs_watch.cpp"
#undef ReadDirectoryChangesW
#undef GetOverlappedResult
#undef WaitForSingleObject

namespace {
int failures = 0;
void Check(bool ok, const char* label) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl;
    if (!ok) ++failures;
}
void Drain() {
    for (int i = 0; i < 300 && pulse::fs::active_watchers.load(); ++i) Sleep(10);
    Check(pulse::fs::active_watchers.load() == 0, "stopped watcher drains all owned resources");
    if (pulse::fs::active_watchers.load()) std::exit(2);
}
void RunFailure(const std::wstring& path, DWORD error, bool async, int limit) {
    injected_error = error; async_failure = async; attempts = 0; wait_limit = limit;
    { std::lock_guard lock(delays_mutex); delays.clear(); }
    ResetEvent(reached); ResetEvent(release_read);
    std::atomic<int> callbacks{0};
    pulse::fs::DirWatch watch;
    Check(watch.Start(path, [&](bool, auto) { ++callbacks; }), "watcher starts on an owned real directory");
    Check(WaitForSingleObject(reached, 3000) == WAIT_OBJECT_0, "failure policy reaches a bounded checkpoint");
    Check(!watch.Armed(), "failed watcher is explicitly unarmed during retry wait");
    const auto start = GetTickCount64();
    watch.Stop();
    Check(GetTickCount64() - start < 500, "Stop interrupts retry without joining a blocked provider");
    const int stopped_callbacks = callbacks.load();
    SetEvent(release_read);
    Drain();
    Check(callbacks == stopped_callbacks, "no callbacks after Stop");
    std::lock_guard lock(delays_mutex);
    if (error == ERROR_INVALID_FUNCTION) {
        Check(delays == std::vector<DWORD>(static_cast<size_t>(limit), 30000),
              "unsupported notifications use a 30-second cancellable polling interval");
        Check(callbacks <= limit + (async ? 1 : 0), "unsupported errors have bounded refresh notifications");
    } else {
        Check(delays == std::vector<DWORD>({250, 500, 1000, 2000, 4000, 8000, 8000}),
              "transient retry grows exponentially and caps at eight seconds");
        Check(callbacks <= (async ? 2 : 1), "one unavailable episode does not emit a refresh storm");
    }
    Check(attempts == limit, "successful directory reopen cannot bypass the retry delay");
}
void RunRecovery(const std::filesystem::path& root) {
    injected_error = ERROR_BUSY; async_failure = false; attempts = 0; wait_limit = 4;
    { std::lock_guard lock(delays_mutex); delays.clear(); }
    ResetEvent(reached); ResetEvent(release_read);
    HANDLE reconciled = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE changed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!reconciled || !changed) std::exit(2);
    pulse::fs::DirWatch watch;
    Check(watch.Start(root.wstring(), [&](bool overflow, auto events) {
        if (overflow && attempts >= 4) SetEvent(reconciled);
        if (!events.empty()) SetEvent(changed);
    }), "transient recovery watcher starts");
    Check(WaitForSingleObject(reconciled, 3000) == WAIT_OBJECT_0 && watch.Armed(),
          "three failures recover to a real pending read and reconcile the missed interval");
    injected_error = ERROR_ACCESS_DENIED;
    HANDLE marker = CreateFileW((root / L"recovered.txt").c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(marker != INVALID_HANDLE_VALUE, "create owned file after recovery");
    if (marker != INVALID_HANDLE_VALUE) CloseHandle(marker);
    Check(WaitForSingleObject(changed, 3000) == WAIT_OBJECT_0, "real local change is delivered after recovery");
    Check(WaitForSingleObject(reached, 3000) == WAIT_OBJECT_0, "next failure reaches interruptible retry");
    watch.Stop(); SetEvent(release_read); Drain();
    { std::lock_guard lock(delays_mutex);
      Check(delays == std::vector<DWORD>({250, 500, 1000, 250}), "healthy read resets backoff for a later failure"); }
    CloseHandle(reconciled); CloseHandle(changed);
    DeleteFileW((root / L"recovered.txt").c_str());
}
// #58-12: a full change buffer is not a failure. Every overflow asks for one
// fresh listing; the handle stays open and armed, with no retry backoff.
void RunOverflow(const std::wstring& path) {
    injected_error = ERROR_NOTIFY_ENUM_DIR; async_failure = true; attempts = 0; wait_limit = 5;
    { std::lock_guard lock(delays_mutex); delays.clear(); }
    ResetEvent(reached); ResetEvent(release_read);
    std::atomic<int> overflows{0}, details{0};
    pulse::fs::DirWatch watch;
    Check(watch.Start(path, [&](bool overflow, auto events) {
        if (overflow) ++overflows;
        if (!events.empty()) ++details;
    }), "OVERFLOW-01 watcher starts");
    Check(WaitForSingleObject(reached, 3000) == WAIT_OBJECT_0, "OVERFLOW-02 repeated overflows keep reading");
    Check(watch.Armed(), "OVERFLOW-03 the watch stays armed through an overflow");
    const auto start = GetTickCount64();
    watch.Stop();
    Check(GetTickCount64() - start < 500, "OVERFLOW-04 Stop interrupts the overflow pause");
    SetEvent(release_read);
    Drain();
    std::lock_guard lock(delays_mutex);
    Check(delays.size() >= 5 && std::all_of(delays.begin(), delays.end(), [](DWORD d) { return d == 100; }),
          "OVERFLOW-05 only the short overflow pause, no failure backoff");
    Check(overflows >= 5 && details == 0, "OVERFLOW-06 each overflow asks for a fresh listing");
}
void RunQueue() {
    struct Batch { std::wstring path; bool overflow; std::vector<pulse::fs::DirNotifyEvent> events; };
    std::vector<Batch> queue;
    const std::vector<pulse::fs::DirNotifyEvent> events{{FILE_ACTION_ADDED, L"item", {}}};
    pulse::app::QueueDirNotify(queue, L"a", false, events);
    pulse::app::QueueDirNotify(queue, L"b", false, events);
    pulse::app::QueueDirNotify(queue, L"a", false, events);
    Check(queue.size() == 3, "ordinary event batches retain their ordering");
    for (int i = 0; i < 10000; ++i) {
        pulse::app::QueueDirNotify(queue, L"a", true, {});
        pulse::app::QueueDirNotify(queue, L"a", false, events);
    }
    Check(queue.size() == 2 && queue[0].path == L"b" && queue[0].events.size() == 1 &&
          queue[1].path == L"a" && queue[1].overflow && queue[1].events.empty(),
          "10000 repeated overflows occupy one batch per path and preserve another path");
    queue.clear();
    pulse::app::QueueDirNotify(queue, L"a", false, events);
    Check(queue.size() == 1 && !queue[0].overflow && queue[0].events.size() == 1,
          "new events are accepted after the queue is drained");
}
void RunQueueBudget() {
    struct Batch { std::wstring path; bool overflow; std::vector<pulse::fs::DirNotifyEvent> events; };
    std::vector<Batch> queue;
    for (int i = 0; i < 10000; ++i)
        pulse::app::QueueDirNotify(queue, L"busy", false, {{FILE_ACTION_ADDED, std::to_wstring(i), {}}});
    Check(queue.size() == 1 && queue[0].overflow && queue[0].events.empty(),
          "BUDGET-01 backlog requests full reconciliation instead of unbounded detail");
    queue.clear();
    pulse::app::QueueDirNotify(queue, L"armed", true, {{0, {}, {}, 123}});
    pulse::app::QueueDirNotify(queue, L"armed", false, {{FILE_ACTION_RENAMED_NEW_NAME, L"new", L"old"}});
    Check(queue.size() == 2 && pulse::fs::ArmedAt(queue[0].overflow, queue[0].events) == 123 &&
          queue[1].events[0].old_name == L"old", "BUDGET-02 armed overflow preserves subsequent rename");
    queue.clear();
    std::vector<pulse::fs::DirNotifyEvent> large(513, {FILE_ACTION_ADDED, L"item", {}});
    pulse::app::QueueDirNotify(queue, L"large", false, std::move(large));
    Check(queue.size() == 1 && queue[0].overflow && queue[0].events.empty(),
          "BUDGET-03 one oversized callback requests reconciliation");
    pulse::app::DirNotifyPacer<Batch> pacer;
    std::vector<Batch> incoming{{L"paced", false, {{FILE_ACTION_ADDED, L"first", {}}}}}, due;
    pacer.Take(incoming, 1000, due);
    due.clear();
    for (int i = 0; i < 10000; ++i) {
        incoming.push_back({L"paced", false, {{FILE_ACTION_ADDED, std::to_wstring(i), {}}}});
        pacer.Take(incoming, 1001, due);
    }
    pacer.Take(incoming, 1200, due);
    Check(due.size() == 1 && due[0].overflow && due[0].events.empty() && !pacer.Holding(),
          "BUDGET-04 paced backlog requests one final reconciliation");
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"queue-budget-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    std::filesystem::create_directory(root);
    queue.clear();
    for (int i = 0; i < 70; ++i) {
        const auto name = std::to_wstring(i);
        std::filesystem::create_directory(root / name);
        pulse::app::QueueDirNotify(queue, root.wstring(), false, {{FILE_ACTION_ADDED, name, {}}});
    }
    std::filesystem::rename(root / L"69", root / L"renamed");
    pulse::app::QueueDirNotify(queue, root.wstring(), false,
        {{FILE_ACTION_RENAMED_NEW_NAME, L"renamed", L"69"}});
    std::filesystem::remove(root / L"0");
    pulse::app::QueueDirNotify(queue, root.wstring(), false, {{FILE_ACTION_REMOVED, L"0", {}}});
    std::vector<pulse::fs::DirEntry> snapshot;
    if (queue.size() == 1 && queue[0].overflow)
        pulse::fs::EnumerateDirectory(root.wstring(), snapshot);
    bool consistent = snapshot.size() == 69;
    for (int i = 1; i < 69; ++i)
        consistent = consistent && std::any_of(snapshot.begin(), snapshot.end(), [&](const auto& entry) {
            return entry.name == std::to_wstring(i);
        });
    consistent = consistent && std::any_of(snapshot.begin(), snapshot.end(), [](const auto& entry) {
        return entry.name == L"renamed";
    });
    Check(consistent, "BUDGET-05 production enumeration after overflow includes later rename and removal");
    for (int i = 1; i < 69; ++i) std::filesystem::remove(root / std::to_wstring(i));
    std::filesystem::remove(root / L"renamed");
    Check(std::filesystem::remove(root), "BUDGET-06 owned reconciliation fixture removed");
}

void RunPacer() {
    struct Batch { std::wstring path; bool overflow; std::vector<pulse::fs::DirNotifyEvent> events; };
    using Pacer = pulse::app::DirNotifyPacer<Batch>;
    const auto removed = [](const wchar_t* name) {
        return std::vector<pulse::fs::DirNotifyEvent>{{FILE_ACTION_REMOVED, name, {}}};
    };
    Pacer pacer;
    std::vector<Batch> incoming, due;
    const ULONGLONG t0 = 1000000;
    incoming.push_back({L"a", false, removed(L"1")});
    pacer.Take(incoming, t0, due);
    Check(due.size() == 1 && !pacer.Holding() && incoming.empty(), "PACE-01 a calm folder's change lands at once");
    // A bulk delete: one item per drain.
    due.clear();
    for (int i = 2; i <= 40; ++i) {
        incoming.push_back({L"a", false, removed(std::to_wstring(i).c_str())});
        pacer.Take(incoming, t0 + static_cast<ULONGLONG>(i), due);
    }
    Check(due.empty() && pacer.Holding(), "PACE-02 a burst inside the window is held");
    incoming.push_back({L"b", false, removed(L"x")});
    pacer.Take(incoming, t0 + 50, due);
    Check(due.size() == 1 && due[0].path == L"b", "PACE-03 another folder is not held by the busy one");
    due.clear();
    pacer.Take(incoming, t0 + Pacer::kPaceMs - 1, due);
    Check(due.empty(), "PACE-04 nothing lands before the window ends");
    pacer.Take(incoming, t0 + Pacer::kPaceMs, due);
    bool ordered = due.size() == 39;
    for (size_t i = 0; ordered && i < due.size(); ++i)
        ordered = due[i].path == L"a" && due[i].events.size() == 1 && due[i].events[0].name == std::to_wstring(i + 2);
    Check(ordered && !pacer.Holding(), "PACE-05 the held burst lands together, in order");
    due.clear();
    incoming.push_back({L"a", false, removed(L"41")});
    pacer.Take(incoming, t0 + Pacer::kPaceMs + 10, due);
    Check(due.empty() && pacer.Holding(), "PACE-06 landing a burst starts the next window");
    pacer.Take(incoming, t0 + 2 * Pacer::kPaceMs, due);
    Check(due.size() == 1 && !pacer.Holding(), "PACE-07 one patch per window while the burst goes on");
    // Size changes neither start a window nor jump ahead of held removals.
    Pacer sizes;
    due.clear();
    incoming.push_back({L"c", false, {{FILE_ACTION_MODIFIED, L"f", {}}}});
    sizes.Take(incoming, t0, due);
    incoming.push_back({L"c", false, {{FILE_ACTION_MODIFIED, L"f", {}}}});
    sizes.Take(incoming, t0 + 1, due);
    Check(due.size() == 2 && !sizes.Holding(), "PACE-08 size-only changes do not start a window");
    due.clear();
    incoming.push_back({L"c", false, removed(L"g")});
    sizes.Take(incoming, t0 + 2, due);
    incoming.push_back({L"c", false, {{FILE_ACTION_MODIFIED, L"h", {}}}});
    sizes.Take(incoming, t0 + 3, due);
    Check(due.size() == 1 && sizes.Holding(), "PACE-09 a change after a removal waits behind it");
    // A full relisting starts a window too.
    Pacer overflow;
    due.clear();
    incoming.push_back({L"d", true, {}});
    overflow.Take(incoming, t0, due);
    incoming.push_back({L"d", false, removed(L"z")});
    overflow.Take(incoming, t0 + 1, due);
    Check(due.size() == 1 && due[0].overflow && overflow.Holding(), "PACE-10 an overflow starts a window");
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring(argv[1]) == L"--queue") {
        RunQueue(); RunPacer(); RunQueueBudget();
        std::cout << "Failures: " << failures << std::endl;
        return failures ? 1 : 0;
    }
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"watch-retry-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    if (!std::filesystem::create_directory(root)) return 2;
    reached = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    release_read = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!reached || !release_read) return 2;
    RunFailure(root.wstring(), ERROR_INVALID_FUNCTION, false, 4);
    RunFailure(root.wstring(), ERROR_ACCESS_DENIED, false, 7);
    RunFailure(root.wstring(), ERROR_INVALID_FUNCTION, true, 4);
    RunFailure(root.wstring(), ERROR_ACCESS_DENIED, true, 7);
    RunRecovery(root);
    RunOverflow(root.wstring());
    RunQueue();
    RunPacer();
    CloseHandle(reached); CloseHandle(release_read);
    Check(std::filesystem::remove(root), "owned fixture removed");
    std::cout << "Failures: " << failures << std::endl;
    return failures ? 1 : 0;
}

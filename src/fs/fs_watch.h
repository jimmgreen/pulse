// fs_watch.h — ReadDirectoryChangesW directory watcher.
#pragma once
#include <windows.h>
#include <functional>
#include <memory>
#include <string>
#include <atomic>
#include <thread>

namespace pulse::fs {

class DirWatch {
public:
    using ChangeCallback = std::function<void()>;

    DirWatch();
    ~DirWatch();

    // Start watching a directory. Returns false on failure.
    bool Start(const std::wstring& path, ChangeCallback cb);
    void Stop();

private:
    void WorkerThread();

    std::wstring path_;
    ChangeCallback callback_;
    HANDLE hDir_ = INVALID_HANDLE_VALUE;
    HANDLE hStop_ = nullptr;
    std::atomic<bool> running_{false};
    std::thread thread_;
    OVERLAPPED overlapped_{};
    alignas(alignof(DWORD)) BYTE buffer_[64 * 1024];
};

} // namespace pulse::fs

// fs_watch.cpp
#include "fs_watch.h"
#include "fs_enum.h"
#include <stdexcept>

namespace pulse::fs {

DirWatch::DirWatch() {
    hStop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

DirWatch::~DirWatch() {
    Stop();
    if (hStop_) CloseHandle(hStop_);
}

bool DirWatch::Start(const std::wstring& path, ChangeCallback cb) {
    Stop();
    path_ = NormalizePath(path);
    callback_ = std::move(cb);

    hDir_ = CreateFileW(
        path_.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);
    if (hDir_ == INVALID_HANDLE_VALUE) return false;

    ResetEvent(hStop_);
    running_ = true;
    thread_ = std::thread(&DirWatch::WorkerThread, this);
    return true;
}

void DirWatch::Stop() {
    running_ = false;
    if (hStop_) SetEvent(hStop_);
    if (hDir_ != INVALID_HANDLE_VALUE) CancelIoEx(hDir_, nullptr);
    if (thread_.joinable()) thread_.join();
    if (hDir_ != INVALID_HANDLE_VALUE) {
        CloseHandle(hDir_);
        hDir_ = INVALID_HANDLE_VALUE;
    }
}

void DirWatch::WorkerThread() {
    HANDLE events[2] = { hStop_, overlapped_.hEvent };
    while (running_) {
        ZeroMemory(&overlapped_, sizeof(overlapped_));
        overlapped_.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        events[1] = overlapped_.hEvent;

        DWORD returned = 0;
        BOOL ok = ReadDirectoryChangesW(
            hDir_,
            buffer_,
            sizeof(buffer_),
            FALSE,
            FILE_NOTIFY_CHANGE_FILE_NAME |
                FILE_NOTIFY_CHANGE_DIR_NAME |
                FILE_NOTIFY_CHANGE_ATTRIBUTES |
                FILE_NOTIFY_CHANGE_SIZE |
                FILE_NOTIFY_CHANGE_LAST_WRITE |
                FILE_NOTIFY_CHANGE_CREATION |
                FILE_NOTIFY_CHANGE_SECURITY,
            &returned,
            &overlapped_,
            nullptr);

        if (!ok) {
            CloseHandle(overlapped_.hEvent);
            break;
        }

        DWORD wait = WaitForMultipleObjects(2, events, FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0) {
            // Stop requested.
            CancelIoEx(hDir_, &overlapped_);
            CloseHandle(overlapped_.hEvent);
            break;
        }

        DWORD transferred = 0;
        BOOL got = GetOverlappedResult(hDir_, &overlapped_, &transferred, FALSE);
        CloseHandle(overlapped_.hEvent);

        if (!got || transferred == 0) {
            // Buffer overflow or error: mark dirty anyway.
        }

        if (callback_) {
            try {
                callback_();
            } catch (...) {
                // Ignore user callback exceptions.
            }
        }
    }
}

} // namespace pulse::fs

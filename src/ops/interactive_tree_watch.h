#pragma once

#include <windows.h>
#include <string>

namespace pulse::ops {

// Optional reuse of a completed destination-tree inspection. Subscribe before
// the walk, not after it. Any namespace/attribute change (including overflow)
// discards the proof; ordinary transfer progress does not take a manager lock.
class InteractiveTreeWatch {
public:
    InteractiveTreeWatch() = default;
    InteractiveTreeWatch(const InteractiveTreeWatch&) = delete;
    InteractiveTreeWatch& operator=(const InteractiveTreeWatch&) = delete;
    ~InteractiveTreeWatch() { Reset(); }

    void Reset() {
        if (directory_ != INVALID_HANDLE_VALUE) {
            if (pending_) {
                CancelIoEx(directory_, &operation_);
                DWORD bytes = 0;
                GetOverlappedResult(directory_, &operation_, &bytes, TRUE);
            }
            CloseHandle(directory_);
        }
        if (operation_.hEvent) CloseHandle(operation_.hEvent);
        directory_ = INVALID_HANDLE_VALUE;
        operation_ = {};
        pending_ = false;
        task_ = 0;
    }

    void Arm(unsigned long long task, const std::wstring& path) {
        Reset();
        directory_ = CreateFileW(path.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (directory_ == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(directory_, &identity_) ||
            !(identity_.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (identity_.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            !(identity_.nFileIndexHigh || identity_.nFileIndexLow)) return;
        operation_.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!operation_.hEvent) return;
        pending_ = ReadDirectoryChangesW(directory_, buffer_, sizeof(buffer_), TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_ATTRIBUTES,
            nullptr, &operation_, nullptr) != FALSE;
        if (pending_) task_ = task;
    }

    bool Unchanged(unsigned long long task, const std::wstring& path) const {
        if (!pending_ || task != task_ || WaitForSingleObject(operation_.hEvent, 0) != WAIT_TIMEOUT) return false;
        // An ancestor or the watched directory itself may have been renamed.
        // Compare against the open (and therefore non-recycled) file identity.
        const HANDLE current = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (current == INVALID_HANDLE_VALUE) return false;
        BY_HANDLE_FILE_INFORMATION info{};
        const bool same = GetFileInformationByHandle(current, &info) &&
            !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
            info.dwVolumeSerialNumber == identity_.dwVolumeSerialNumber &&
            info.nFileIndexHigh == identity_.nFileIndexHigh && info.nFileIndexLow == identity_.nFileIndexLow;
        CloseHandle(current);
        return same && WaitForSingleObject(operation_.hEvent, 0) == WAIT_TIMEOUT;
    }

private:
    HANDLE directory_ = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION identity_{};
    OVERLAPPED operation_{};
    alignas(DWORD) BYTE buffer_[1024]{};
    unsigned long long task_ = 0;
    bool pending_ = false;
};
} // namespace pulse::ops

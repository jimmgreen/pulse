#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdint>

namespace pulse::ipc {

inline bool WriteAllOverlapped(HANDLE pipe, HANDLE io_event, HANDLE child_process,
                               const void* data, DWORD bytes, ULONGLONG deadline_tick,
                               DWORD& out_error) noexcept {
    out_error = ERROR_SUCCESS;
    if (bytes == 0) return true;
    if (pipe == INVALID_HANDLE_VALUE || !io_event) {
        out_error = ERROR_INVALID_HANDLE;
        return false;
    }

    const auto* ptr = static_cast<const unsigned char*>(data);
    DWORD remaining = bytes;
    while (remaining > 0) {
        ResetEvent(io_event);
        OVERLAPPED ov{};
        ov.hEvent = io_event;
        DWORD written = 0;
        if (WriteFile(pipe, ptr, remaining, &written, &ov)) {
            if (written == 0) {
                out_error = ERROR_BROKEN_PIPE;
                return false;
            }
            ptr += written;
            remaining -= written;
            continue;
        }
        const DWORD err = GetLastError();
        if (err != ERROR_IO_PENDING) {
            out_error = err;
            return false;
        }

        const ULONGLONG now = GetTickCount64();
        if (now >= deadline_tick) {
            CancelIoEx(pipe, &ov);
            DWORD drained = 0;
            GetOverlappedResult(pipe, &ov, &drained, TRUE);
            out_error = ERROR_TIMEOUT;
            return false;
        }
        const DWORD wait_ms = static_cast<DWORD>((std::min<ULONGLONG>)(deadline_tick - now, 60000ull));
        HANDLE handles[2]{io_event, child_process};
        const DWORD count = child_process ? 2u : 1u;
        const DWORD signaled = WaitForMultipleObjects(count, handles, FALSE, wait_ms);
        if (signaled == WAIT_OBJECT_0) {
            DWORD transferred = 0;
            if (!GetOverlappedResult(pipe, &ov, &transferred, TRUE) || transferred == 0) {
                out_error = GetLastError();
                if (!out_error) out_error = ERROR_BROKEN_PIPE;
                return false;
            }
            ptr += transferred;
            remaining -= transferred;
            continue;
        }
        CancelIoEx(pipe, &ov);
        DWORD transferred = 0;
        if (GetOverlappedResult(pipe, &ov, &transferred, TRUE) && transferred > 0 &&
            signaled == WAIT_OBJECT_0 + 1) {
            ptr += transferred;
            remaining -= transferred;
            continue;
        }
        out_error = (signaled == WAIT_TIMEOUT) ? ERROR_TIMEOUT
            : (signaled == WAIT_OBJECT_0 + 1) ? ERROR_PROCESS_ABORTED
            : GetLastError();
        return false;
    }
    return true;
}

template <class CancelPred>
inline bool ReadAllOverlapped(HANDLE pipe, HANDLE io_event, HANDLE wake_event,
                              HANDLE child_process, void* data, DWORD bytes,
                              ULONGLONG deadline_tick, DWORD check_slice_ms,
                              CancelPred&& is_cancelled, DWORD& out_error,
                              bool* out_cancelled = nullptr,
                              bool* out_child_exited = nullptr) noexcept {
    out_error = ERROR_SUCCESS;
    if (out_cancelled) *out_cancelled = false;
    if (out_child_exited) *out_child_exited = false;
    if (bytes == 0) return true;
    if (pipe == INVALID_HANDLE_VALUE || !io_event) {
        out_error = ERROR_INVALID_HANDLE;
        return false;
    }

    auto* ptr = static_cast<unsigned char*>(data);
    DWORD remaining = bytes;
    while (remaining > 0) {
        if (is_cancelled()) {
            if (out_cancelled) *out_cancelled = true;
            out_error = ERROR_CANCELLED;
            return false;
        }

        ResetEvent(io_event);
        OVERLAPPED ov{};
        ov.hEvent = io_event;
        DWORD got = 0;
        if (ReadFile(pipe, ptr, remaining, &got, &ov)) {
            if (got == 0) {
                out_error = ERROR_BROKEN_PIPE;
                return false;
            }
            ptr += got;
            remaining -= got;
            continue;
        }
        const DWORD err = GetLastError();
        if (err != ERROR_IO_PENDING) {
            out_error = err;
            return false;
        }

        HANDLE handles[3]{};
        DWORD count = 0;
        handles[count++] = io_event;
        const DWORD wake_index = wake_event ? count++ : 0xFFFFFFFFu;
        if (wake_event) handles[wake_index] = wake_event;
        const DWORD child_index = child_process ? count++ : 0xFFFFFFFFu;
        if (child_process) handles[child_index] = child_process;

        for (;;) {
            if (is_cancelled()) {
                CancelIoEx(pipe, &ov);
                DWORD drained = 0;
                GetOverlappedResult(pipe, &ov, &drained, TRUE);
                if (out_cancelled) *out_cancelled = true;
                out_error = ERROR_CANCELLED;
                return false;
            }
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline_tick) {
                CancelIoEx(pipe, &ov);
                DWORD drained = 0;
                if (GetOverlappedResult(pipe, &ov, &drained, TRUE) && drained > 0) {
                    ptr += drained;
                    remaining -= drained;
                    break;
                }
                out_error = ERROR_TIMEOUT;
                return false;
            }
            ULONGLONG slice64 = deadline_tick - now;
            if (check_slice_ms > 0) slice64 = (std::min<ULONGLONG>)(slice64, check_slice_ms);
            const DWORD slice = static_cast<DWORD>(slice64);
            const DWORD signaled = WaitForMultipleObjects(count, handles, FALSE, slice);
            if (signaled == WAIT_OBJECT_0) {
                DWORD transferred = 0;
                if (!GetOverlappedResult(pipe, &ov, &transferred, TRUE) || transferred == 0) {
                    out_error = GetLastError();
                    if (!out_error) out_error = ERROR_BROKEN_PIPE;
                    return false;
                }
                ptr += transferred;
                remaining -= transferred;
                break;
            }
            if (wake_event && signaled == WAIT_OBJECT_0 + wake_index) {
                // Check caller predicate; only cancel if cancelled or expired.
                continue;
            }
            if (child_process && signaled == WAIT_OBJECT_0 + child_index) {
                CancelIoEx(pipe, &ov);
                DWORD transferred = 0;
                if (GetOverlappedResult(pipe, &ov, &transferred, TRUE) && transferred > 0) {
                    ptr += transferred;
                    remaining -= transferred;
                    break;
                }
                if (out_child_exited) *out_child_exited = true;
                out_error = ERROR_PROCESS_ABORTED;
                return false;
            }
            if (signaled == WAIT_TIMEOUT) {
                continue;
            }
            CancelIoEx(pipe, &ov);
            DWORD drained = 0;
            GetOverlappedResult(pipe, &ov, &drained, TRUE);
            out_error = GetLastError();
            return false;
        }
    }
    return true;
}

}  // namespace pulse::ipc

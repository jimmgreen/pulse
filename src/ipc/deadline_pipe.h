#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdint>

namespace pulse::ipc {
// One absolute deadline covers every fragment of a frame. A zero deadline is
// reserved for cancellable idle reads owned by an independent client worker.
template<class Stopped>
bool DeadlinePipeIo(HANDLE pipe, uint8_t* bytes, DWORD size, bool write,
                    ULONGLONG deadline, Stopped stopped) {
    if (!size) return true;
    struct Event {
        HANDLE handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ~Event() { const DWORD error = GetLastError(); if (handle) CloseHandle(handle); SetLastError(error); }
    } event;
    if (!event.handle) return false;
    while (size) {
        if (stopped()) { SetLastError(ERROR_OPERATION_ABORTED); return false; }
        if (deadline && GetTickCount64() >= deadline) { SetLastError(ERROR_TIMEOUT); return false; }
        OVERLAPPED operation{};
        operation.hEvent = event.handle;
        ResetEvent(event.handle);
        DWORD transferred = 0;
        BOOL ok = write ? WriteFile(pipe, bytes, size, &transferred, &operation)
                        : ReadFile(pipe, bytes, size, &transferred, &operation);
        DWORD error = ok ? ERROR_SUCCESS : GetLastError();
        if (!ok && error == ERROR_IO_PENDING) {
            for (;;) {
                const auto now = GetTickCount64();
                if (stopped() || (deadline && now >= deadline)) {
                    error = stopped() ? ERROR_OPERATION_ABORTED : ERROR_TIMEOUT;
                    CancelIoEx(pipe, &operation);
                    GetOverlappedResult(pipe, &operation, &transferred, TRUE);
                    break;
                }
                const DWORD wait_ms = deadline ? static_cast<DWORD>((std::min)(deadline - now, ULONGLONG{100})) : 100;
                const auto wait = WaitForSingleObject(operation.hEvent, wait_ms);
                if (wait == WAIT_TIMEOUT) continue;
                if (wait == WAIT_OBJECT_0) {
                    ok = GetOverlappedResult(pipe, &operation, &transferred, FALSE);
                    error = ok ? ERROR_SUCCESS : GetLastError();
                } else {
                    error = GetLastError();
                    CancelIoEx(pipe, &operation);
                    GetOverlappedResult(pipe, &operation, &transferred, TRUE);
                }
                break;
            }
        }
        if (!ok || !transferred) { SetLastError(error ? error : ERROR_BROKEN_PIPE); return false; }
        bytes += transferred; size -= transferred;
    }
    return true;
}
}

#include "../ipc/deadline_pipe.h"
#include <atomic>
#include <iostream>
#include <string>
#include <cstring>
#include <thread>
#include <vector>

namespace {
struct Pipe {
    HANDLE server = INVALID_HANDLE_VALUE, client = INVALID_HANDLE_VALUE;
    Pipe(unsigned serial) {
        const auto name = L"\\\\.\\pipe\\PulseDeadlineAudit-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(serial);
        server = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, nullptr);
        if (server == INVALID_HANDLE_VALUE) return;
        client = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        OVERLAPPED operation{}; operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (client != INVALID_HANDLE_VALUE && !ConnectNamedPipe(server, &operation) && GetLastError() != ERROR_PIPE_CONNECTED) {
            DWORD transferred = 0;
            if (GetLastError() == ERROR_IO_PENDING) GetOverlappedResult(server, &operation, &transferred, TRUE);
        }
        CloseHandle(operation.hEvent);
    }
    ~Pipe() {
        if (client != INVALID_HANDLE_VALUE) CloseHandle(client);
        if (server != INVALID_HANDLE_VALUE) CloseHandle(server);
    }
    bool valid() const { return server != INVALID_HANDLE_VALUE && client != INVALID_HANDLE_VALUE; }
};
}
int main() {
    bool ok = true;
    auto check = [&](bool condition, const char* label) { std::cout << (condition ? "[PASS] " : "[FAIL] ") << label << '\n'; ok &= condition; };
    const auto never = [] { return false; };
    check(pulse::ipc::DeadlinePipeIo(INVALID_HANDLE_VALUE, nullptr, 0, false, 0, never),
          "empty payload requires no event or pipe access");
    {
        Pipe pipe(0);
        uint8_t actual[16]{};
        std::thread fragments([&] {
            for (uint8_t i = 0; i < 16; ++i) {
                DWORD written = 0;
                WriteFile(pipe.client, &i, 1, &written, nullptr);
                Sleep(2);
            }
        });
        bool received = pulse::ipc::DeadlinePipeIo(pipe.server, actual, sizeof(actual), false,
            GetTickCount64() + 2000, never);
        fragments.join();
        for (uint8_t i = 0; i < 16; ++i) received &= actual[i] == i;
        check(received, "fragmented frame safely resets and reuses one operation event");
    }
    {
        Pipe pipe(1); uint8_t bytes[16]{}; const auto begin = GetTickCount64();
        const bool read = pipe.valid() && pulse::ipc::DeadlinePipeIo(pipe.server, bytes, sizeof(bytes), false, begin + 150, never);
        const auto error = GetLastError();
        check(!read && error == ERROR_TIMEOUT && GetTickCount64() - begin < 1000, "silent named-pipe peer expires server read and drains cancellation");
    }
    {
        Pipe pipe(2); std::vector<uint8_t> bytes(1024 * 1024, 42); const auto begin = GetTickCount64();
        const bool write = pipe.valid() && pulse::ipc::DeadlinePipeIo(pipe.server, bytes.data(), static_cast<DWORD>(bytes.size()), true, begin + 150, never);
        const auto error = GetLastError();
        check(!write && error == ERROR_TIMEOUT && GetTickCount64() - begin < 1000, "non-reading peer cannot block a large server write beyond deadline");
    }
    {
        Pipe pipe(3); uint8_t bytes[16]{};
        std::thread trickle([&] { for (int i = 0; i < 8; ++i) { Sleep(35); DWORD wrote = 0; const uint8_t value = 1; WriteFile(pipe.client, &value, 1, &wrote, nullptr); } });
        const auto begin = GetTickCount64();
        const bool read = pulse::ipc::DeadlinePipeIo(pipe.server, bytes, sizeof(bytes), false, begin + 150, never);
        const auto error = GetLastError(); const auto elapsed = GetTickCount64() - begin;
        trickle.join();
        check(!read && error == ERROR_TIMEOUT && elapsed < 500, "partial frame fragments share one absolute deadline");
    }
    {
        Pipe pipe(4); uint8_t bytes[16]{}; std::atomic<bool> stopped = false;
        std::thread stop([&] { Sleep(100); stopped = true; });
        const auto begin = GetTickCount64();
        const bool read = pulse::ipc::DeadlinePipeIo(pipe.server, bytes, sizeof(bytes), false, begin + 5000, [&] { return stopped.load(); });
        const auto error = GetLastError(); stop.join();
        check(!read && error == ERROR_OPERATION_ABORTED && GetTickCount64() - begin < 1000, "shutdown cancels pending server I/O promptly");
    }
    {
        Pipe slow(5), healthy(6); std::atomic<bool> timed_out = false;
        std::thread blocked([&] { std::vector<uint8_t> bytes(1024 * 1024); timed_out = !pulse::ipc::DeadlinePipeIo(slow.server, bytes.data(), static_cast<DWORD>(bytes.size()), true, GetTickCount64() + 200, never); });
        uint8_t expected[] = {1, 2, 3, 4}, actual[4]{}; DWORD wrote = 0;
        WriteFile(healthy.client, expected, sizeof(expected), &wrote, nullptr);
        const bool read = pulse::ipc::DeadlinePipeIo(healthy.server, actual, sizeof(actual), false, GetTickCount64() + 1000, never);
        blocked.join();
        check(read && memcmp(expected, actual, sizeof(actual)) == 0 && timed_out, "independent healthy pipe completes while stalled peer times out");
    }
    return ok ? 0 : 1;
}

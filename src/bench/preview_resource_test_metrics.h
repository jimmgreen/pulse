#pragma once
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <cstdio>

struct PreviewResourceMetrics {
    DWORD threads = 0, handles = 0;
    SIZE_T private_bytes = 0;
    static PreviewResourceMetrics Capture(const char* phase) {
        PreviewResourceMetrics value;
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 entry{sizeof(entry)};
        if (snapshot != INVALID_HANDLE_VALUE) {
            if (Thread32First(snapshot, &entry)) do {
                if (entry.th32OwnerProcessID == GetCurrentProcessId()) ++value.threads;
            } while (Thread32Next(snapshot, &entry));
            CloseHandle(snapshot);
        }
        GetProcessHandleCount(GetCurrentProcess(), &value.handles);
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)))
            value.private_bytes = memory.PrivateUsage;
        std::printf("[METRICS] %s threads=%lu handles=%lu private_bytes=%zu\n",
            phase, value.threads, value.handles, value.private_bytes);
        return value;
    }
};

#pragma once
#include <windows.h>
#include <array>
#include <cstdio>
#include <source_location>
#include <mutex>
#include <atomic>

namespace pulse::ui::motion_probe {
struct Requests {
    struct Site { const char* file = nullptr; unsigned line = 0; unsigned long long calls = 0; };
    std::array<Site, 256> sites{};
    wchar_t output[32768]{};
    std::mutex mutex;
    Requests() {
        if (!GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", nullptr, 0)) return;
        const auto count = GetEnvironmentVariableW(L"PULSE_TEST_MOTION_REPORT", output, ARRAYSIZE(output));
        if (count >= ARRAYSIZE(output)) output[0] = 0;
    }
    void Add(const std::source_location& location) {
        if (!output[0]) return;
        std::lock_guard lock(mutex);
        for (auto& site : sites) {
            if (!site.file) { site = {location.file_name(), location.line(), 1}; return; }
            if (site.file == location.file_name() && site.line == location.line()) { ++site.calls; return; }
        }
    }
    void Flush() {
        if (!output[0]) return;
        std::lock_guard lock(mutex);
        FILE* file = nullptr;
        if (_wfopen_s(&file, output, L"wb") || !file) return;
        std::fprintf(file, "file,line,requests\n");
        for (const auto& site : sites) if (site.file)
            std::fprintf(file, "%s,%u,%llu\n", site.file, site.line, site.calls);
        std::fclose(file);
    }
};
inline std::atomic<Requests*> active{nullptr};
inline void Enable() {
    static Requests requests;
    if (requests.output[0]) active = &requests;
}
inline void Record(const std::source_location& location) {
    if (auto* requests = active.load(std::memory_order_relaxed)) requests->Add(location);
}
inline void Flush() {
    if (auto* requests = active.exchange(nullptr)) requests->Flush();
}
}

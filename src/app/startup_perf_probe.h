#pragma once

// Only included by explicitly enabled test builds. Never writes user settings.
#include <windows.h>
#include "../fs/fs_enum.h"
#include "../ui/motion_probe.h"
#include <psapi.h>
#include <cstdio>
#include <share.h>
#include <functional>
#include <string>

namespace pulse::app::startup_perf_probe {
struct Listing {
    bool ready = false;
    size_t entries = 0;
    std::wstring path;
};
inline HWND window = nullptr;
inline FILE* report = nullptr;
inline std::function<Listing()> listing;
inline std::function<void(const std::wstring&)> navigate;
inline std::function<void()> hidden_action;
inline std::wstring original, target;
inline ULONGLONG created = 0, stage_start = 0, idle_start = 0;
inline int stage = 0, sample = 0;
inline unsigned navigation_rounds = 1, navigation_round = 0, navigation_settle_ms = 1000;
inline bool first_present = false;
inline bool startup_only = false;
inline bool minimize_probe = false;
inline bool hidden_repaint_probe = false;
inline bool hidden_navigate_probe = false;
inline bool hide_probe = false;
inline bool hidden_navigation_seen = false;
inline unsigned long long frame_wakes = 0;
inline unsigned long long ticks = 0, presents = 0;
inline double tick_ms = 0, tags_ms = 0, ops_ms = 0;
struct WorkScope {
    double& total;
    LARGE_INTEGER start{}, frequency{};
    explicit WorkScope(double& value) : total(value) {
        if (report) { QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&start); }
    }
    ~WorkScope() {
        if (start.QuadPart) { LARGE_INTEGER end{}; QueryPerformanceCounter(&end);
            total += 1000.0 * (end.QuadPart - start.QuadPart) / frequency.QuadPart; }
    }
};
inline constexpr UINT_PTR kTimer = 0x50504631;
inline ULONGLONG FileTimeValue(FILETIME value) {
    return (static_cast<ULONGLONG>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
}
inline ULONGLONG Now() {
    FILETIME value{};
    GetSystemTimePreciseAsFileTime(&value);
    return FileTimeValue(value);
}
inline void Record(const char* event, size_t entries = 0) {
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
    FILETIME c{}, e{}, kernel{}, user{};
    GetProcessTimes(GetCurrentProcess(), &c, &e, &kernel, &user);
    const auto now = Now();
    fprintf(report, "%s,%.3f,%.3f,%zu,%zu,%zu,%.3f,%llu,%llu,%.6f,%.6f,%.6f,%llu\n", event,
        static_cast<double>(now - created) / 10000.0,
        static_cast<double>(now - stage_start) / 10000.0, entries,
        memory.WorkingSetSize, memory.PrivateUsage,
        static_cast<double>(FileTimeValue(kernel) + FileTimeValue(user)) / 10000.0, ticks, presents, tick_ms, tags_ms, ops_ms, frame_wakes);
    fflush(report);
}
inline void Finish(const char* event) {
    Record(event);
    ui::motion_probe::Flush();
    KillTimer(window, kTimer);
    fclose(report);
    report = nullptr;
    PostMessageW(window, WM_CLOSE, 0, 0);
}
inline void CALLBACK Tick(HWND, UINT, UINT_PTR, DWORD);
inline void Presented(HWND hwnd) {
    if (!report || hwnd != window) return;
    ++presents;
    const auto current = listing();
    if (stage == 7 && current.ready && current.path == (hidden_navigate_probe ? target : original)) {
        Record("restored_listing_presented", current.entries);
        stage = 8; stage_start = Now(); SetTimer(window, kTimer, 5000, Tick);
    }
    if (!first_present) { first_present = true; Record("first_present", current.entries); }
    if (stage == 0 && current.ready) {
        original = current.path;
        Record("initial_listing_presented", current.entries);
        stage = 1;
        stage_start = Now();
    } else if (stage == 2 && current.ready && current.path == target) {
        Record("target_listing_presented", current.entries);
        stage = 3;
        stage_start = Now();
    } else if (stage == 4 && current.ready && current.path == original) {
        Record("return_listing_presented", current.entries);
        if (++navigation_round < navigation_rounds) {
            stage = 1; stage_start = Now(); return;
        }
        stage = 5;
        idle_start = stage_start = Now();
        SetTimer(window, kTimer, 5000, Tick);
    }
}
inline void CALLBACK Tick(HWND, UINT, UINT_PTR, DWORD) {
    if (!report) return;
    if (Now() - created > 1200000000ULL) { Finish("timeout"); return; }
    if (stage == 6) {
        const auto current = listing();
        if (hidden_navigate_probe && !hidden_navigation_seen && current.ready && current.path == target) {
            hidden_navigation_seen = true; Record("hidden_navigation_applied", current.entries);
        }
        const auto elapsed = Now() - stage_start;
        if (elapsed < 300000000ULL) {
            if (hidden_repaint_probe) RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
            else SetTimer(window, kTimer, static_cast<UINT>((300000000ULL - elapsed + 9999) / 10000), Tick);
            return;
        }
        Record("minimized_30s", listing().entries);
        stage = 7; stage_start = Now();
        ShowWindow(window, hide_probe ? SW_SHOW : SW_RESTORE); InvalidateRect(window, nullptr, FALSE);
        SetTimer(window, kTimer, 100, Tick);
    } else if (stage == 8) {
        const auto elapsed = Now() - stage_start;
        if (elapsed >= 50000000ULL) Finish("complete");
        else SetTimer(window, kTimer, static_cast<UINT>((50000000ULL - elapsed + 9999) / 10000), Tick);
    } else if (stage == 1 || stage == 3) {
        // Leave navigation out of the paint stack and allow startup warmups to settle.
        if (Now() - stage_start < static_cast<ULONGLONG>(navigation_settle_ms) * 10000) return;
        const bool outbound = stage == 1;
        stage = outbound ? 2 : 4;
        stage_start = Now();
        navigate(outbound ? target : original);
        InvalidateRect(window, nullptr, FALSE);
    } else if (stage == 5) {
        constexpr ULONGLONG delays[] = {50000000ULL, 300000000ULL, 600000000ULL};
        constexpr const char* names[] = {"idle_5s", "idle_30s", "idle_60s"};
        if (sample == 3) { Finish("complete"); return; }
        const auto elapsed = Now() - idle_start;
        if (startup_only && elapsed >= delays[0]) { Finish("complete"); return; }
        if (elapsed >= delays[sample]) Record(names[sample++], listing().entries);
        if (minimize_probe && sample == 2) {
            Record("minimize_started", listing().entries);
            stage = 6; stage_start = Now(); ShowWindow(window, hide_probe ? SW_HIDE : SW_MINIMIZE);
            if (hidden_navigate_probe) navigate(target);
            if (hidden_action) hidden_action();
            SetTimer(window, kTimer, hidden_repaint_probe ? 250 : 30000, Tick); return;
        }
        // SetTimer may fire a little early. Schedule the remaining interval,
        // not another full five/25/30 seconds from that early notification.
        const auto remaining = sample == 3 ? 2500000ULL :
            (delays[sample] > elapsed ? delays[sample] - elapsed : 10000ULL);
        SetTimer(window, kTimer, static_cast<UINT>((remaining + 9999) / 10000), Tick);
    }
}
inline void Start(HWND hwnd, bool isolated, std::function<Listing()> read,
                  std::function<void(const std::wstring&)> go, std::function<void()> on_hidden = {}) {
    wchar_t output[32768]{}, destination[32768]{}, scope[32]{};
    if (!isolated || !GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", nullptr, 0)) return;
    const DWORD n = GetEnvironmentVariableW(L"PULSE_TEST_STARTUP_PERF", output, ARRAYSIZE(output));
    const DWORD t = GetEnvironmentVariableW(L"PULSE_TEST_STARTUP_TARGET", destination, ARRAYSIZE(destination));
    GetEnvironmentVariableW(L"PULSE_TEST_P1_SCOPE", scope, ARRAYSIZE(scope));
    if (!n || n >= ARRAYSIZE(output) || !t || t >= ARRAYSIZE(destination) || wcscmp(scope, L"selection") != 0) return;
    report = _wfsopen(output, L"wb", _SH_DENYNO);
    if (!report) return;
    ui::motion_probe::Enable();
    FILETIME c{}, e{}, k{}, u{};
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    created = stage_start = FileTimeValue(c);
    hidden_action = std::move(on_hidden);
    hide_probe = GetEnvironmentVariableW(L"PULSE_TEST_IDLE_HIDE", nullptr, 0) > 0;
    hidden_navigate_probe = GetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_NAVIGATE", nullptr, 0) > 0;
    hidden_repaint_probe = GetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_REPAINT", nullptr, 0) > 0;
    minimize_probe = GetEnvironmentVariableW(L"PULSE_TEST_IDLE_MINIMIZE", nullptr, 0) > 0;
    startup_only = GetEnvironmentVariableW(L"PULSE_TEST_STARTUP_ONLY", nullptr, 0) > 0;
    wchar_t rounds[16]{};
    if (GetEnvironmentVariableW(L"PULSE_TEST_NAVIGATION_ROUNDS", rounds, ARRAYSIZE(rounds))) {
        navigation_rounds = (std::clamp)(wcstoul(rounds, nullptr, 10), 1ul, 100ul);
        navigation_settle_ms = 50;
    }
    window = hwnd; target = fs::NormalizePath(destination); listing = std::move(read); navigate = std::move(go);
    fprintf(report, "event,process_ms,stage_ms,entries,working_set_bytes,private_commit_bytes,cpu_ms,ticks,presents,tick_ms,tags_ms,ops_ms,frame_wakes\n");
    Record("probe_started");
    if (!SetTimer(hwnd, kTimer, 100, Tick)) Finish("timer_failed");
}
} // namespace pulse::app::startup_perf_probe

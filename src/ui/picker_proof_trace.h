#pragma once

#if defined(PULSE_PICKER_INPROCESS_PROOF)
#include <windows.h>
#include <cstdio>

namespace pulse::ui::picker_proof_trace {
struct TraceState {
    unsigned frames = 0;
    bool active = false;
    ULONGLONG wall = 0;
    ULONGLONG cpu = 0;
};
inline thread_local TraceState state;
inline ULONGLONG ThreadCpu(bool& valid) {
    FILETIME created{}, exited{}, kernel{}, user{};
    valid = GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user) != FALSE;
    return ((static_cast<ULONGLONG>(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime) +
        ((static_cast<ULONGLONG>(user.dwHighDateTime) << 32) | user.dwLowDateTime);
}
inline void Point(const char* phase) {
    if (!state.active) return;
    const DWORD error = GetLastError();
    const ULONGLONG wall = GetTickCount64();
    bool cpu_valid = false;
    const ULONGLONG cpu = ThreadCpu(cpu_valid);
    std::fprintf(stderr, "picker-frame phase=%s wall_ms=%llu delta_ms=%llu cpu_100ns=%llu delta_cpu_100ns=%llu cpu_valid=%u\n",
        phase, static_cast<unsigned long long>(wall), static_cast<unsigned long long>(wall - state.wall),
        static_cast<unsigned long long>(cpu), static_cast<unsigned long long>(cpu_valid && cpu >= state.cpu ? cpu - state.cpu : 0),
        cpu_valid ? 1u : 0u);
    std::fflush(stderr);
    state.wall = wall; state.cpu = cpu;
    SetLastError(error);
}
class Frame {
public:
    explicit Frame(bool enabled) {
        if (!enabled || state.active || state.frames >= 3) return;
        const DWORD error = GetLastError();
        wchar_t value[2]{};
        const bool requested = GetEnvironmentVariableW(L"PULSE_PICKER_UI_DIAGNOSTICS", value, 2) == 1 && value[0] == L'1';
        if (requested) {
            ++state.frames; state.active = true; owns_ = true;
            state.wall = GetTickCount64(); bool valid = false; state.cpu = ThreadCpu(valid);
            Point("frame-begin");
        }
        SetLastError(error);
    }
    ~Frame() { if (owns_) { Point("frame-end"); state.active = false; } }
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
private:
    bool owns_ = false;
};
} // namespace pulse::ui::picker_proof_trace
#define PULSE_PICKER_TRACE_FRAME(enabled) ::pulse::ui::picker_proof_trace::Frame picker_trace_frame(enabled)
#define PULSE_PICKER_TRACE_POINT(phase) ::pulse::ui::picker_proof_trace::Point(phase)
#else
#define PULSE_PICKER_TRACE_FRAME(enabled) ((void)0)
#define PULSE_PICKER_TRACE_POINT(phase) ((void)0)
#endif

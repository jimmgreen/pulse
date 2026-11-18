#pragma once

// Cold-start milestones for profiling (P1). Set PULSE_STARTUP_TRACE=1 and
// the process records each Mark() in milliseconds since it was created, plus
// the DLLs already loaded by then, every DLL
// loaded later and the window messages that took 2 ms or more, and writes them to
// %TEMP%\pulse_startup_trace.txt. Without the variable each call is one
// predictable branch.

#include <windows.h>

namespace pulse::app::startup_trace {

// `label` must be a string literal. A label repeats at most three times
// (frame #1..#3); later calls with it are dropped.
void Mark(const char* label) noexcept;
// Latest cumulative diagnostic value; label must be a string literal.
void Counter(const char* label, double value) noexcept;

// Writes the trace `delay_ms` after this call, from a timer on `hwnd`'s
// thread, so work that finishes just after the deferred startup is included.
void ScheduleFlush(HWND hwnd, UINT delay_ms) noexcept;

// Times one window message until the trace is written; those taking 2 ms or
// more are listed with their start and duration.
class MessageScope {
public:
    explicit MessageScope(UINT msg) noexcept;
    ~MessageScope();
    MessageScope(const MessageScope&) = delete;
    MessageScope& operator=(const MessageScope&) = delete;

private:
    UINT msg_ = 0;
    double start_ = -1.0;  // negative: not timing
};

}  // namespace pulse::app::startup_trace

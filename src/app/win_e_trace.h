#pragma once
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <thread>

namespace pulse::app {
struct WinETraceRecord {
    enum Kind { Input, Submission, Hook } kind = Input;
    HWND foreground = nullptr;
    uint64_t tick = 0, cycle = 0, id_high = 0, id_low = 0;
    uint32_t input_time = 0, phase = 0, action = 0, observed = 0, asynchronous = 0;
    uint32_t flags = 0, result = 0, error = 0, generation = 0;
};
// One producer (hook thread), one consumer. No allocation, lock, process query,
// or logger call on the keyboard callback path.
class WinETrace {
public:
    WinETrace();
    ~WinETrace();
    void Push(const WinETraceRecord& record) noexcept;
    void CountE(bool down) noexcept { if (down) ++e_down_; else ++e_up_; }
private:
    friend struct WinETraceTestPeer;
    explicit WinETrace(HANDLE test_start_gate);
    void Run() noexcept;
    std::array<WinETraceRecord, 256> records_{};
    std::atomic<size_t> written_{0}, read_{0};
    std::atomic<uint64_t> dropped_{0}, e_down_{0}, e_up_{0};
    std::atomic<bool> stopping_{false};
    HANDLE wake_ = nullptr;
    std::thread worker_;
};
}

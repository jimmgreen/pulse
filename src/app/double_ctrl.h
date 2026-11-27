#pragma once
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>

namespace pulse {
class DoubleCtrlDetector {
public:
    static constexpr uint64_t kHoldMs = 280;
    void SetGap(uint32_t ms) { gap_ = ms; }
    bool Key(bool ctrl, bool down, uint64_t now) {
        return PhysicalKey(ctrl ? VK_LCONTROL : 'A', down, now);
    }
    bool PhysicalKey(UINT key, bool down, uint64_t now, bool injected = false);
    void Prime(UINT key) { if (key < pressed_.size()) { pressed_[key] = true; Interrupt(); } }
    void Interrupt();
    bool Pending(uint64_t now) const {
        return phase_ == Phase::FirstDown || phase_ == Phase::SecondDown ||
            (phase_ == Phase::FirstUp && now - up_at_ <= gap_);
    }
private:
    enum class Phase : uint8_t { Idle, FirstDown, FirstUp, SecondDown, Spoiled };
    bool Blocked(UINT except) const;
    Phase phase_ = Phase::Idle;
    std::array<bool, 256> pressed_{};
    std::array<bool, 256> injected_{};
    UINT tap_key_ = 0;
    uint64_t down_at_ = 0, up_at_ = 0;
    uint32_t gap_ = 300;
};

// UI-owned lifecycle; hooks run only while enabled, on a dedicated message thread.
// All hook callbacks pass input through and only update state/post notifications.
class DoubleCtrlWatcher {
public:
    DoubleCtrlWatcher() = default;
    ~DoubleCtrlWatcher() { Stop(); }
    DoubleCtrlWatcher(const DoubleCtrlWatcher&) = delete;
    DoubleCtrlWatcher& operator=(const DoubleCtrlWatcher&) = delete;
    void Update(HWND target, UINT message, bool enabled, uint32_t gap_ms);
    void Stop();
    bool Running() const { return running_.load(); }
    bool IsCurrentTrigger(WPARAM generation) const {
        return Running() && generation != 0 && generation == generation_.load();
    }
    DWORD LastError() const { return error_.load(); }
    static bool QuietMoment();
private:
    friend struct DoubleCtrlWatcherTestAccess;
    void Run();
    void StopLocked();
    static LRESULT CALLBACK KeyboardProc(int code, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK MouseProc(int code, WPARAM wparam, LPARAM lparam);
    static thread_local DoubleCtrlWatcher* current_;
    std::mutex lifecycle_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<DWORD> error_{ERROR_SUCCESS};
    std::atomic<ULONG_PTR> generation_{1};
    HWND target_ = nullptr;
    UINT message_ = 0;
    uint32_t gap_ = 300;
    HANDLE ready_ = nullptr, stop_ = nullptr;
    DoubleCtrlDetector detector_;
};
}

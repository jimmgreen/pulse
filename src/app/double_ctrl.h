#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>

namespace pulse {
// Two clean Ctrl taps: each held briefly, the second starting soon after the
// first ends, with no other key or mouse button in between. The logic is kept
// apart from the listener so it can be tested with synthetic timings.
class DoubleCtrlDetector {
public:
    static constexpr uint64_t kHoldMs = 280;  // longest press that still counts as a tap
    void SetGap(uint32_t ms) { gap_ = ms; }
    // A key event (ctrl = either Ctrl key). True when it completes a double tap.
    bool Key(bool ctrl, bool down, uint64_t now);
    // A mouse button, wheel or anything else that spoils the gesture.
    void Interrupt();
    // A tap is under way (or could still be completed): the listener watches
    // mouse buttons only then.
    bool Pending(uint64_t now) const {
        return held_ || phase_ == Phase::SecondDown || (phase_ == Phase::FirstUp && now - up_at_ <= gap_);
    }
private:
    enum class Phase : uint8_t { Idle, FirstDown, FirstUp, SecondDown, Spoiled };
    Phase phase_ = Phase::Idle;
    bool held_ = false;
    uint64_t down_at_ = 0, up_at_ = 0;
    uint32_t gap_ = 300;
};

// Listens for the double tap on a small thread of its own (Raw Input sink on a
// message-only window: no keyboard hook, so it cannot slow typing anywhere)
// and posts `message` to `target`. The shelf can share this listener and
// optionally request periodic pointer samples.
class DoubleCtrlWatcher {
public:
    DoubleCtrlWatcher() = default;
    ~DoubleCtrlWatcher() { Stop(); }
    DoubleCtrlWatcher(const DoubleCtrlWatcher&) = delete;
    DoubleCtrlWatcher& operator=(const DoubleCtrlWatcher&) = delete;
    void Update(HWND target, UINT message, bool enabled, uint32_t gap_ms);
    using InputObserver = std::function<void(const RAWINPUT&)>;
    using InputPoll = std::function<void()>;
    void SetInputObserver(InputObserver observe, InputPoll poll = {});
    void Stop();
    bool Running() const { return thread_.joinable(); }
    // Full-screen games, presentations and other "do not disturb" states.
    static bool QuietMoment();
private:
    void Run();
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    void OnInput(HRAWINPUT input);
    void WatchMouse(bool on);
    void RefreshInputDevices();
    struct Observer {
        InputObserver on_input;
        InputPoll poll;
    };
    std::thread thread_;
    std::atomic<DWORD> thread_id_{0};
    std::atomic<HWND> target_{nullptr};
    std::atomic<UINT> message_{0};
    std::atomic<uint32_t> gap_{300};
    std::atomic<bool> enabled_{false};
    std::atomic<std::shared_ptr<const Observer>> observer_;
    HWND window_ = nullptr;          // listener thread only
    bool mouse_ = false;             // listener thread only
    bool polling_ = false;           // listener thread only
    DoubleCtrlDetector detector_;    // listener thread only
};
}

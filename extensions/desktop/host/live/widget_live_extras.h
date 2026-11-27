// widget_live_extras.h - Live data & state providers for Pomodoro, MediaPlayer (WinRT GSMTC),
// Countdown, and Battery/Bluetooth Power desktop widgets.
#pragma once
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pulse::desktop::live {

// ---------------------------------------------------------------------------
// 1. Now Playing (Windows Global System Media Transport Controls — GSMTC)
// ---------------------------------------------------------------------------
enum class MediaCommand : uint8_t {
    PlayPause = 0,
    Next = 1,
    Previous = 2,
};

struct MediaSessionSnapshot {
    bool active = false;
    bool playing = false;
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    std::wstring source_app;          // Friendly player name (e.g. "Spotify", "网易云音乐", "Edge")
    std::vector<uint8_t> cover_bytes; // Raw PNG/JPEG thumbnail bytes from GSMTC
    uint64_t cover_hash = 0;          // FNV-1a hash of cover_bytes (0 when no cover)
};

// Runs all WinRT GSMTC queries & transport calls on a dedicated MTA worker thread so the UI
// thread never blocks. Posts `notify_msg` to `notify_hwnd` only when the snapshot changes.
class MediaSessionMonitor {
public:
    MediaSessionMonitor();
    ~MediaSessionMonitor();
    MediaSessionMonitor(const MediaSessionMonitor&) = delete;
    MediaSessionMonitor& operator=(const MediaSessionMonitor&) = delete;

    void Start(HWND notify_hwnd, UINT notify_msg);
    void Stop() noexcept;
    void SetEnabled(bool enabled) noexcept;
    void RequestRefresh() noexcept;
    void SendCommand(MediaCommand cmd) noexcept;
    MediaSessionSnapshot Snapshot() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// 2. Battery, Power Plan & Connected Bluetooth Device Batteries
// ---------------------------------------------------------------------------
struct BatteryDeviceInfo {
    std::wstring name;
    int percent = -1; // 0..100
    uint8_t kind = 0; // 0=generic BT, 1=audio/headset, 2=mouse, 3=keyboard, 4=controller
};

struct BatteryPowerSnapshot {
    bool valid = false;
    bool has_battery = false;
    int battery_pct = 100;         // 0..100 (or 100 on AC desktop)
    bool charging = false;
    bool ac_online = true;
    bool power_saver = false;
    int remaining_min = -1;        // -1 when unknown or on AC
    uint64_t uptime_sec = 0;
    std::wstring power_plan;       // e.g. L"平衡", L"高性能", L"节能"
    std::vector<BatteryDeviceInfo> bt_devices;
};

// Queries system power, active power scheme, uptime, and connected Bluetooth battery devices.
// Pass `include_bluetooth = true` on a background thread or slow cadence (~10s).
BatteryPowerSnapshot SampleBatteryPower(bool include_bluetooth = true);

// ---------------------------------------------------------------------------
// 3. Countdown & Calendar Time-Progress Math
// ---------------------------------------------------------------------------
struct CountdownComputed {
    std::wstring title;
    std::wstring target_iso; // YYYY-MM-DD
    int days_delta = 0;      // >0 remaining, 0 today, <0 days since
    bool is_today = false;
    bool is_past = false;
    float day_pct = 0.0f;    // 0..100
    float week_pct = 0.0f;   // 0..100 (Mon..Sun)
    float month_pct = 0.0f;  // 0..100
    float year_pct = 0.0f;   // 0..100
};

bool ParseIsoDate(const std::wstring& iso, int& year, int& month, int& day) noexcept;
CountdownComputed ComputeCountdown(const std::wstring& title,
                                   const std::wstring& target_iso,
                                   const SYSTEMTIME& now);

// ---------------------------------------------------------------------------
// 4. Interactive Pomodoro Focus Timer State
// ---------------------------------------------------------------------------
enum class PomodoroPhase : uint8_t {
    Focus = 0,
    ShortBreak = 1,
    LongBreak = 2,
};

struct PomodoroState {
    bool running = false;
    int focus_minutes = 25;
    int remaining_sec = 25 * 60;
    int total_sec = 25 * 60;
    int completed_today = 0;
    int short_min = 5;    // 小组件 › 番茄钟
    int long_min = 15;
    PomodoroPhase phase = PomodoroPhase::Focus;

    void Toggle() noexcept { running = !running; }
    void Reset() noexcept {
        running = false;
        total_sec = PhaseDurationSec(phase, focus_minutes);
        remaining_sec = total_sec;
    }
    void SetPreset(PomodoroPhase new_phase, int new_focus_min) noexcept {
        running = false;
        if (new_focus_min >= 5 && new_focus_min <= 90) focus_minutes = new_focus_min;
        phase = new_phase;
        total_sec = PhaseDurationSec(phase, focus_minutes);
        remaining_sec = total_sec;
    }
    void CyclePhase() noexcept {
        running = false;
        phase = (phase == PomodoroPhase::Focus) ? PomodoroPhase::ShortBreak
              : (phase == PomodoroPhase::ShortBreak) ? PomodoroPhase::LongBreak
                                                     : PomodoroPhase::Focus;
        total_sec = PhaseDurationSec(phase, focus_minutes);
        remaining_sec = total_sec;
    }
    // Returns true if the active phase just reached 00:00 and transitioned.
    bool TickOneSecond(bool& finished_focus) noexcept {
        finished_focus = false;
        if (!running) return false;
        if (remaining_sec > 1) {
            --remaining_sec;
            return false;
        }
        running = false;
        if (phase == PomodoroPhase::Focus) {
            ++completed_today;
            finished_focus = true;
            phase = ((completed_today % 4) == 0) ? PomodoroPhase::LongBreak : PomodoroPhase::ShortBreak;
        } else {
            phase = PomodoroPhase::Focus;
        }
        total_sec = PhaseDurationSec(phase, focus_minutes);
        remaining_sec = total_sec;
        return true;
    }

    int PhaseDurationSec(PomodoroPhase p, int focus_min) const noexcept {
        if (p == PomodoroPhase::ShortBreak) return std::clamp(short_min, 1, 30) * 60;
        if (p == PomodoroPhase::LongBreak) return std::clamp(long_min, 5, 60) * 60;
        return std::clamp(focus_min, 5, 90) * 60;
    }
};

} // namespace pulse::desktop::live

#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <functional>
#include <optional>
#include <utility>

namespace pulse::desktop::video {

// UI-thread only. A bounded, low-priority retry for a final static/paused frame
// whose shared texture was still busy when its notification was consumed.
template<class Frame>
class VideoGlassRetry {
public:
    ~VideoGlassRetry() { Close(); }
    VideoGlassRetry() = default;
    VideoGlassRetry(const VideoGlassRetry&) = delete;
    VideoGlassRetry& operator=(const VideoGlassRetry&) = delete;

    void Initialize(HWND hwnd, std::function<bool(const Frame&)> import_frame,
                    std::function<void()> on_success) {
        Close();
        hwnd_ = hwnd;
        import_frame_ = std::move(import_frame);
        on_success_ = std::move(on_success);
    }

    void Submit(Frame frame) {
        Stage(std::move(frame));
        RetryNow();
    }

    void Stage(Frame frame) {
        if (!hwnd_ || !import_frame_) return;
        Clear();
        pending_ = std::move(frame);
        deferred_ = true;
    }

    void RetryNow() {
        deferred_ = false;
        Attempt();
    }

    void Clear() {
        CancelTimer();
        pending_.reset();
        retries_ = 0;
        deferred_ = false;
    }

    void Close() {
        Clear();
        hwnd_ = nullptr;
        import_frame_ = {};
        on_success_ = {};
    }

    void EnsureTimer() {
        if (!pending_ || deferred_ || timer_id_ || !hwnd_) return;
        const HANDLE owner = GetPropW(hwnd_, kProperty);
        if (owner && owner != this) return;
        if (!SetPropW(hwnd_, kProperty, reinterpret_cast<HANDLE>(this))) return;
        timer_id_ = SetTimer(hwnd_, reinterpret_cast<UINT_PTR>(this), 16, TimerProc);
        if (!timer_id_) RemovePropW(hwnd_, kProperty);
    }

    bool Pending() const { return pending_.has_value(); }

private:
    static constexpr const wchar_t* kProperty = L"Pulse.Desktop.VideoGlassRetry";
    static constexpr unsigned kMaxRetries = 16;

    static void CALLBACK TimerProc(HWND hwnd, UINT, UINT_PTR timer_id, DWORD) {
        // KillTimer does not remove already queued WM_TIMER messages. Resolve
        // through the live window property before touching the former owner.
        auto* self = reinterpret_cast<VideoGlassRetry*>(GetPropW(hwnd, kProperty));
        if (!self || self->hwnd_ != hwnd || self->timer_id_ != timer_id) return;
        ++self->retries_;
        self->Attempt();
    }

    void Attempt() {
        if (!pending_) return;
        if (import_frame_(*pending_)) {
            Clear();
            if (on_success_) on_success_();
        } else if (retries_ >= kMaxRetries) {
            // Keep the renderer's last successful image; only a new frame may
            // start another budget. The heartbeat must not revive this frame.
            Clear();
        } else {
            EnsureTimer();
        }
    }

    void CancelTimer() {
        if (hwnd_ && timer_id_) KillTimer(hwnd_, timer_id_);
        timer_id_ = 0;
        if (hwnd_ && GetPropW(hwnd_, kProperty) == reinterpret_cast<HANDLE>(this)) {
            RemovePropW(hwnd_, kProperty);
        }
    }

    HWND hwnd_ = nullptr;
    UINT_PTR timer_id_ = 0;
    unsigned retries_ = 0;
    bool deferred_ = false;
    std::optional<Frame> pending_;
    std::function<bool(const Frame&)> import_frame_;
    std::function<void()> on_success_;
};

} // namespace pulse::desktop::video

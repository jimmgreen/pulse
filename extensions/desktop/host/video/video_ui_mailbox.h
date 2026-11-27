#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>

namespace pulse::desktop::video {

// The owner serializes all calls. Wake callbacks must be nonblocking and must
// not reenter the mailbox (PostMessage is suitable, SendMessage is not).
template<class Event>
class VideoUiMailbox {
public:
    void Open() {
        Close();
        closed_ = false;
    }

    void Close() {
        closed_ = true;
        controls_.clear();
        frame_.reset();
        wake_pending_ = false;
        have_sequence_ = false;
    }

    void PushControl(Event event) {
        if (!closed_) controls_.push_back(std::move(event));
    }

    bool PushFrame(Event event, uint64_t generation, uint64_t sequence) {
        // A player generation keeps its increasing sequence through freeze /
        // pipeline reopen. Only a new generation or Open() starts a new epoch.
        if (closed_ || (have_sequence_ &&
            (generation < generation_ || (generation == generation_ && sequence <= sequence_)))) {
            return false;
        }
        generation_ = generation;
        sequence_ = sequence;
        have_sequence_ = true;
        frame_ = std::move(event);
        // Replacing a frame moves its position to its newest notification;
        // control events themselves always retain their original FIFO order.
        controls_before_frame_ = controls_.size();
        return true;
    }

    template<class PostWake>
    bool TryWake(PostWake&& post_wake) {
        if (closed_ || wake_pending_ || PendingCount() == 0) return false;
        wake_pending_ = post_wake();
        return wake_pending_;
    }

    std::deque<Event> TakeBatch() {
        std::deque<Event> batch;
        batch.swap(controls_);
        if (frame_) {
            batch.insert(batch.begin() + static_cast<std::ptrdiff_t>(controls_before_frame_),
                         std::move(*frame_));
            frame_.reset();
        }
        // Producers may now schedule one more wake while this batch renders.
        wake_pending_ = false;
        return batch;
    }

    size_t PendingCount() const { return controls_.size() + (frame_ ? 1u : 0u); }
    bool WakePending() const { return wake_pending_; }

private:
    std::deque<Event> controls_;
    std::optional<Event> frame_;
    size_t controls_before_frame_ = 0;
    uint64_t generation_ = 0;
    uint64_t sequence_ = 0;
    bool have_sequence_ = false;
    bool wake_pending_ = false;
    bool closed_ = true;
};

} // namespace pulse::desktop::video

#pragma once
#include <mutex>

namespace pulse::app {
// Owns a posted wake until explicit UI acknowledgement, independently of Status
// reads. Callbacks must only enqueue asynchronous work (PostMessage/SetTimer),
// never reenter this gate. Keep the gate alive until producer threads stop.
class OpsNotifyGate {
public:
    template<class Post> bool Request(Post&& post) {
        std::lock_guard lock(mutex_);
        if (state_ == State::Closed) return false;
        dirty_ = true;
        return state_ != State::Idle || PostLocked(post);
    }

    bool BeginDispatch() {
        std::lock_guard lock(mutex_);
        if (state_ != State::Queued) return false;
        state_ = State::Handling;
        dirty_ = false;
        return true;
    }

    // Always insert a timer-delimited quiet interval, including when currently
    // clean: a producer arriving immediately after dispatch must not bypass it.
    template<class Defer> bool EndDispatch(Defer&& defer) {
        std::lock_guard lock(mutex_);
        if (state_ != State::Handling) return false;
        state_ = State::Cooling;
        try { if (defer()) return true; } catch (...) {}
        state_ = State::Idle;
        dirty_ = true; // Retain a retry obligation when SetTimer fails.
        return false;
    }

    // No receiver exists. Retain state without posting or polling the host;
    // only a new receiver's explicit Retry resumes delivery.
    bool SuspendDispatch() {
        std::lock_guard lock(mutex_);
        if (state_ != State::Handling) return false;
        state_ = State::Suspended;
        dirty_ = true;
        return true;
    }

    template<class Post> bool Resume(Post&& post) {
        std::lock_guard lock(mutex_);
        if (state_ != State::Cooling) return false;
        state_ = State::Idle;
        return !dirty_ || PostLocked(post);
    }

    // An existing low-frequency UI timer may retry failed PostMessage/SetTimer.
    // Never spin synchronously on failure; queued/handling/cooling stay owned.
    template<class Post> bool Retry(Post&& post) {
        std::lock_guard lock(mutex_);
        if (state_ == State::Closed) return false;
        if (state_ == State::Suspended) state_ = State::Idle;
        return state_ != State::Idle || !dirty_ || PostLocked(post);
    }

    void Close() {
        std::lock_guard lock(mutex_);
        state_ = State::Closed;
        dirty_ = false;
    }

private:
    enum class State { Idle, Queued, Handling, Cooling, Suspended, Closed };
    template<class Post> bool PostLocked(Post& post) {
        state_ = State::Queued;
        try { if (post()) return true; } catch (...) {}
        state_ = State::Idle;
        dirty_ = true;
        return false;
    }
    std::mutex mutex_;
    State state_ = State::Idle;
    bool dirty_ = false;
};
}

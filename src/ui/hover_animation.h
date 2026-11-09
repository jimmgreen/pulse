#pragma once
#include <algorithm>
#include <cstdint>
#include <string>

namespace pulse::ui {
// One visible card, one outstanding frame. A decoded frame starts its own
// delay; pending decoding never skips frames or replaces the displayed image.
class HoverAnimation {
public:
    void Bind(const std::wstring& path, uint64_t modified, uint64_t size, bool enabled) {
        if (!enabled || path_ != path || modified_ != modified || size_ != size) {
            *this = HoverAnimation{};
            if (enabled) { path_ = path; modified_ = modified; size_ = size; enabled_ = true; }
        }
    }
    uint32_t Request(uint64_t now) {
        if (active_ && !waiting_ && now >= deadline_) {
            requested_ = (current_ + 1) % count_;
            waiting_ = true;
        }
        return waiting_ ? requested_ : current_;
    }
    void Commit(uint32_t count, uint32_t delay, uint64_t now) {
        if (!enabled_ || failed_ || count <= 1 || !delay) { active_ = false; return; }
        const bool changed = waiting_ || !active_;
        current_ = requested_; waiting_ = false; count_ = count; active_ = true;
        if (changed) deadline_ = now + std::clamp(delay, 20u, 2000u);
    }
    void Stop() { failed_ = true; active_ = waiting_ = false; requested_ = current_; }
    uint32_t Current() const { return current_; }
    bool Waiting() const { return waiting_; }
    unsigned NextDelay(uint64_t now) const {
        if (!active_) return 0;
        if (waiting_) return 100;
        return static_cast<unsigned>((std::max)(uint64_t{1}, deadline_ > now ? deadline_ - now : 1));
    }
private:
    std::wstring path_;
    uint64_t modified_ = 0, size_ = 0, deadline_ = 0;
    uint32_t current_ = 0, requested_ = 0, count_ = 1;
    bool active_ = false, waiting_ = false, failed_ = false, enabled_ = false;
};
}

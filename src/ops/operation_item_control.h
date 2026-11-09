#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

namespace pulse::ops {
// Tokens bind a click to one in-flight item. An old click must never cancel
// the next file, even if the worker advances before the UI receives progress.
class OperationItemControl {
public:
    struct Item {
        uint64_t task = 0, token = 0;
        std::atomic<bool> cancel{false};
        bool skipped = false;
    };
    std::shared_ptr<Item> Begin(uint64_t task, const std::atomic<bool>& task_cancel) {
        std::lock_guard lock(mutex_);
        auto item = std::make_shared<Item>();
        item->task = task;
        item->token = ++next_;
        item->cancel.store(task_cancel.load());
        current_ = item;
        return item;
    }
    bool Skip(uint64_t task, uint64_t token) {
        std::lock_guard lock(mutex_);
        if (!current_ || current_->task != task || current_->token != token || current_->cancel.load()) return false;
        current_->skipped = true;
        current_->cancel.store(true);
        return true;
    }
    void Cancel() {
        std::lock_guard lock(mutex_);
        if (current_) current_->cancel.store(true);
    }
    bool Finish(const std::shared_ptr<Item>& item) {
        std::lock_guard lock(mutex_);
        if (current_ == item) current_.reset();
        return item->skipped;
    }
private:
    std::mutex mutex_;
    std::shared_ptr<Item> current_;
    uint64_t next_ = 0;
};
}

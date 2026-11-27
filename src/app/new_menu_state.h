#pragma once
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace pulse::app {

enum class NewMenuLoadState { Loading, Ready, Failed };

template <typename Template>
class NewMenuCache {
public:
    struct Snapshot {
        std::vector<Template> list;
        NewMenuLoadState state = NewMenuLoadState::Loading;
        uint64_t revision = 0;
    };

    Snapshot Read() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshot_;
    }

    bool BeginRefresh() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (refreshing_) return false;
        refreshing_ = true;
        // Retain a usable previous list during refresh (including an empty list).
        snapshot_.state = loaded_ ? NewMenuLoadState::Ready : NewMenuLoadState::Loading;
        return true;
    }

    void Complete(std::vector<Template> list) {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.list = std::move(list);
        snapshot_.state = NewMenuLoadState::Ready;
        ++snapshot_.revision;
        loaded_ = true;
        refreshing_ = false;
    }

    void Fail() {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.state = NewMenuLoadState::Failed;
        ++snapshot_.revision;
        refreshing_ = false;
    }

private:
    mutable std::mutex mutex_;
    Snapshot snapshot_;
    bool loaded_ = false;
    bool refreshing_ = false;
};

} // namespace pulse::app


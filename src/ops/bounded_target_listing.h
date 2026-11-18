#pragma once
#include "source_listing.h"
#include <deque>
#include <memory>
#include <unordered_map>

namespace pulse::ops {
// Advisory conflict-count hints only. Execution must still probe identities
// and use exclusive creation; another writer can invalidate any snapshot.
class BoundedTargetListing {
public:
    static constexpr size_t kCapacity = 16;
    static constexpr ULONGLONG kLifetimeMs = 500;
    bool SnapshotAbsent(const std::wstring& folder, const std::wstring& name,
                        ULONGLONG now = GetTickCount64()) {
        auto found = folders_.find(folder);
        if (found == folders_.end()) {
            if (folders_.size() == kCapacity) {
                folders_.erase(order_.front());
                order_.pop_front();
            }
            order_.push_back(folder);
            found = folders_.try_emplace(folder).first;
            found->second.began = now;
        }
        auto& state = found->second;
        if (now - state.began >= kLifetimeMs) {
            state = State{};
            state.began = now;
        }
        // A directory with only a handful of destinations is cheaper to probe.
        if (++state.queries < SourceListing::kMinSourcesPerFolder) return false;
        if (!state.listing)
            state.listing = std::make_unique<TargetListing>(folder, SourceListing::kMinSourcesPerFolder);
        return state.listing->SurelyAbsent(name);
    }
    void Added(const std::wstring& folder, const std::wstring& name) {
        const auto found = folders_.find(folder);
        if (found != folders_.end() && found->second.listing) found->second.listing->Added(name);
    }
    void Invalidate(const std::wstring& folder) {
        const auto found = folders_.find(folder);
        if (found != folders_.end()) found->second = State{};
    }
    size_t Size() const { return folders_.size(); }
private:
    struct State {
        ULONGLONG began = 0;
        size_t queries = 0;
        std::unique_ptr<TargetListing> listing;
    };
    std::unordered_map<std::wstring, State> folders_;
    std::deque<std::wstring> order_;
};
}

#pragma once
#include "snapshot_patch.h"
#include "../fs/fs_snapshot.h"
#include <atomic>
#include <functional>
#include <memory>
#include <unordered_map>

namespace pulse::app {
struct NotifySnapshotGuard {
    const void* owner = nullptr;
    uint64_t view_generation = 0;
    std::weak_ptr<const std::vector<fs::DirEntry>> snapshot;
    bool Matches(const void* tab, uint64_t generation, const fs::SnapshotPtr& value) const {
        const auto captured = snapshot.lock();
        return captured && owner == tab && view_generation == generation && captured == value;
    }
};

// UI owns the slots. Workers only own a detached result, never a Tab/AppState.
class DirNotifyAsync {
public:
    using Enqueue = std::function<void(std::function<void()>)>;
    using Prepare = std::function<PreparedDirNotifyBatch(const std::wstring&,
        const std::vector<fs::DirNotifyEvent>&)>;
    using Guards = std::vector<NotifySnapshotGuard>;
    static constexpr size_t kMaxFolders = 128;
    static constexpr size_t kMaxEvents = 512;
    static constexpr unsigned kMaxPreparePasses = 3;
    struct Completion {
        std::wstring path;
        Guards guards;
        PreparedDirNotifyBatch prepared;
        bool fallback = false;
    };
    bool Pending(const std::wstring& path) const { return slots_.contains(path); }
    bool Empty() const { return slots_.empty(); }
    size_t Size() const { return slots_.size(); }

    bool Queue(const std::wstring& path, const std::vector<fs::DirNotifyEvent>& events,
        uint64_t round, Guards guards, const Enqueue& enqueue,
        const Prepare& prepare = PrepareDirNotifyBatch) {
        auto found = slots_.find(path);
        if (found == slots_.end()) {
            if (slots_.size() >= kMaxFolders || events.size() > kMaxEvents) return false;
            Slot slot;
            slot.guards = std::move(guards);
            slot.events = events;
            slot.round = round;
            slot.round_batches.push_back(events);
            Start(path, slot, enqueue, prepare);
            slots_.emplace(path, std::move(slot));
            return true;
        }
        Slot& slot = found->second;
        if (slot.round != round) {
            slot.round = round;
            slot.round_batches.clear();
        }
        // Each window sees the same drain. Deduplicate complete batches, not
        // individual events: remove/add/remove must retain its final remove.
        for (const auto& prior : slot.round_batches) if (Same(prior, events)) return true;
        if (slot.overflow) return true;
        if (slot.events.size() + slot.following.size() + events.size() > kMaxEvents) {
            slot.overflow = true;
            slot.following.clear();
            slot.round_batches.clear();
            return true;
        }
        slot.round_batches.push_back(events);
        slot.following.insert(slot.following.end(), events.begin(), events.end());
        return true;
    }

    std::vector<Completion> TakeReady(const Enqueue& enqueue,
        const Prepare& prepare = PrepareDirNotifyBatch) {
        std::vector<Completion> done;
        for (auto it = slots_.begin(); it != slots_.end();) {
            auto& slot = it->second;
            if (!slot.result->ready.load(std::memory_order_acquire)) { ++it; continue; }
            if (!slot.following.empty() && slot.passes >= kMaxPreparePasses) slot.overflow = true;
            if (!slot.overflow && !slot.result->failed && !slot.following.empty()) {
                // Read the entire rename chain against one final filesystem
                // state instead of publishing an intermediate missing name.
                slot.events.insert(slot.events.end(), slot.following.begin(), slot.following.end());
                slot.following.clear();
                Start(it->first, slot, enqueue, prepare);
                ++it;
                continue;
            }
            Completion result;
            result.path = it->first;
            result.guards = std::move(slot.guards);
            result.prepared = std::move(slot.result->prepared);
            result.fallback = slot.overflow || slot.result->failed || result.prepared.need_full_enum;
            done.push_back(std::move(result));
            it = slots_.erase(it);
        }
        return done;
    }

private:
    struct Result {
        std::atomic<bool> ready{false};
        bool failed = false;
        PreparedDirNotifyBatch prepared;
    };
    struct Slot {
        Guards guards;
        std::vector<fs::DirNotifyEvent> events, following;
        std::shared_ptr<Result> result;
        uint64_t round = 0;
        std::vector<std::vector<fs::DirNotifyEvent>> round_batches;
        bool overflow = false;
        unsigned passes = 0;
    };
    static bool Same(const std::vector<fs::DirNotifyEvent>& a, const std::vector<fs::DirNotifyEvent>& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i].action != b[i].action || a[i].name != b[i].name || a[i].old_name != b[i].old_name) return false;
        return true;
    }
    static void Start(const std::wstring& path, Slot& slot, const Enqueue& enqueue, const Prepare& prepare) {
        ++slot.passes;
        slot.result = std::make_shared<Result>();
        auto result = slot.result;
        try {
            enqueue([result, path, events = slot.events, prepare] {
                try { result->prepared = prepare(path, events); }
                catch (...) { result->failed = true; }
                result->ready.store(true, std::memory_order_release);
            });
        } catch (...) {
            result->failed = true;
            result->ready.store(true, std::memory_order_release);
        }
    }
    std::unordered_map<std::wstring, Slot> slots_;
};
} // namespace pulse::app

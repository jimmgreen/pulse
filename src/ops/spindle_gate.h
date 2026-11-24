// spindle_gate.h — Operations sharing a spinning disk take turns on it.
//
// Two operations reading and writing one hard disk at the same time make its
// head jump between them: measured on a 4 TB drive, a big copy and a small-file
// copy run together took 137 s, against 113.5 s one after the other. Here each
// operation (a task: every thread of it shares the turn) holds the disk for a
// slice, then hands it to the next operation waiting, in order. Both make
// progress and the disk keeps reading in long runs. Solid-state disks and
// unknown storage never enter (SpinningDisksFor returns nothing for them).
//
// Disks are always entered in ascending order, so two operations on two disks
// each cannot wait on each other.
#pragma once
#include <windows.h>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

namespace pulse::ops {

class SpindleGate {
public:
    static constexpr ULONGLONG kSliceMs = 3000;

    static SpindleGate& Instance() {
        static SpindleGate gate;
        return gate;
    }

    // Blocks until `task` may use `disk`; re-entrant for the task holding it
    // while its slice lasts or no one else waits. False: `abort` became true.
    template <typename Abort>
    bool Enter(long disk, const void* task, Abort&& abort) {
        std::unique_lock lock(mutex_);
        auto& state = disks_[disk];
        // The owner keeps its turn for the whole slice, between files too, so
        // small files do not hand the disk back and forth one at a time.
        const auto ours = [&] {
            const bool slice_left = GetTickCount64() - state.since < kSliceMs;
            if (state.owner == task) return state.waiting.empty() || slice_left;
            const bool free = state.owner == nullptr || (state.holders == 0 && !slice_left);
            return free && (state.waiting.empty() || state.waiting.front() == task);
        };
        while (!ours()) {
            if (std::find(state.waiting.begin(), state.waiting.end(), task) == state.waiting.end())
                state.waiting.push_back(task);
            changed_.wait_for(lock, std::chrono::milliseconds(50));
            if (abort()) {
                std::erase(state.waiting, task);
                changed_.notify_all();
                return false;
            }
        }
        if (state.owner != task) {
            state.owner = task;
            state.since = GetTickCount64();
            std::erase(state.waiting, task);
        }
        ++state.holders;
        return true;
    }

    // Someone waits for a disk `task` holds and its slice is used up.
    bool Contended(const std::vector<long>& disks, const void* task) {
        std::lock_guard lock(mutex_);
        for (const long disk : disks) {
            const auto found = disks_.find(disk);
            if (found != disks_.end() && found->second.owner == task && !found->second.waiting.empty() &&
                GetTickCount64() - found->second.since >= kSliceMs) return true;
        }
        return false;
    }

    void Leave(long disk, const void* task) {
        {
            std::lock_guard lock(mutex_);
            auto found = disks_.find(disk);
            if (found == disks_.end() || found->second.owner != task || found->second.holders == 0) return;
            auto& state = found->second;
            // Idle with no one waiting: free. Otherwise the owner keeps the rest of
            // its slice, and the next in line takes over once it is used up.
            if (--state.holders == 0 && (state.waiting.empty() || GetTickCount64() - state.since >= kSliceMs))
                state.owner = nullptr;
        }
        changed_.notify_all();
    }

private:
    struct Disk {
        const void* owner = nullptr;
        unsigned holders = 0;
        ULONGLONG since = 0;
        std::deque<const void*> waiting;
    };
    std::mutex mutex_;
    std::condition_variable changed_;
    std::map<long, Disk> disks_;
};

// Holds the turn on a set of spinning disks for one piece of work (a file, a
// chunk). Checkpoint hands the disks on when the slice is up and someone waits.
class SpindleTurn {
public:
    SpindleTurn() = default;
    SpindleTurn(const SpindleTurn&) = delete;
    SpindleTurn& operator=(const SpindleTurn&) = delete;
    ~SpindleTurn() { Release(); }

    template <typename Abort>
    bool Acquire(const std::vector<long>& disks, const void* task, Abort&& abort) {
        Release();
        task_ = task;
        for (const long disk : disks) {  // ascending, see SpindleGate
            if (!SpindleGate::Instance().Enter(disk, task, abort)) { Release(); return false; }
            held_.push_back(disk);
        }
        disks_ = disks;
        return true;
    }
    void Release() {
        for (auto it = held_.rbegin(); it != held_.rend(); ++it) SpindleGate::Instance().Leave(*it, task_);
        held_.clear();
    }
    // Between chunks of one long copy: whether to hand the disks on now.
    bool Contended() const { return !held_.empty() && SpindleGate::Instance().Contended(disks_, task_); }
    // Take the same disks again after Release (waits behind those in line).
    template <typename Abort>
    bool Reacquire(Abort&& abort) {
        if (disks_.empty() || !held_.empty()) return true;
        const auto disks = disks_;
        return Acquire(disks, task_, abort);
    }
    bool Held() const { return !held_.empty(); }
    const std::vector<long>& Disks() const { return disks_; }

private:
    const void* task_ = nullptr;
    std::vector<long> disks_;
    std::vector<long> held_;
};

}  // namespace pulse::ops

// History replay of a recycle undo (B1): restore requests are batched, and a
// retry after a partial failure skips exactly the originals already put back.
// No Recycle Bin access: requests are read from the queue and their results
// are simulated through the same completion path the worker uses.
#include "../ops/ops_manager.h"
#include <cstdio>
#include <string>
#include <vector>

namespace pulse::ops {
struct OpsManagerAuditAccess {
    static void SetRunning(OpsManager& m, bool running) { m.running_ = running; }
    static void AddStep(OpsManager& m, OpType type, uint64_t task, size_t n) {
        UndoEntry e;
        e.type = type;
        e.task_id = task;
        for (size_t i = 0; i < n; ++i) e.sources.push_back(L"C:\\b1\\f" + std::to_wstring(i) + L".txt");
        if (type == OpType::Move)
            for (size_t i = 0; i < n; ++i) e.destinations.push_back(L"C:\\b1\\to\\f" + std::to_wstring(i) + L".txt");
        std::lock_guard<std::mutex> lock(m.mutex_);
        m.undo_.push_back(std::move(e));
    }
    static bool Start(OpsManager& m, bool redo) {
        std::lock_guard<std::mutex> lock(m.mutex_);
        return m.StartHistoryLocked(redo, 0, false);
    }
    // The queued request, as the worker would take it.
    static OpRequest Front(OpsManager& m) {
        std::lock_guard<std::mutex> lock(m.mutex_);
        return m.queue_.empty() ? OpRequest{} : m.queue_.front().req;
    }
    static size_t Requests(OpsManager& m) {
        std::lock_guard<std::mutex> lock(m.mutex_);
        return m.history_replay_ ? m.history_replay_->requests.size() : 0;
    }
    // Finish the queued request having put back `restored` of its sources.
    static void Finish(OpsManager& m, const std::vector<std::wstring>& restored, bool success) {
        std::lock_guard<std::mutex> lock(m.mutex_);
        if (m.queue_.empty()) return;
        const uint64_t seq = m.queue_.front().seq;
        m.queue_.pop_front();
        m.restored_by_last_request_ = restored;
        m.FinishHistoryRequestLocked(seq, success);
    }
    static bool Replaying(OpsManager& m) { std::lock_guard<std::mutex> lock(m.mutex_); return m.history_replay_.has_value(); }
    static size_t UndoCount(OpsManager& m) { std::lock_guard<std::mutex> lock(m.mutex_); return m.undo_.size(); }
    static size_t RedoCount(OpsManager& m) { std::lock_guard<std::mutex> lock(m.mutex_); return m.redo_.size(); }
    static size_t RestoredKept(OpsManager& m) {
        std::lock_guard<std::mutex> lock(m.mutex_);
        size_t n = 0;
        for (const auto& [task, set] : m.history_restored_) n += set.size();
        return n;
    }
    static void Reset(OpsManager& m) {
        std::lock_guard<std::mutex> lock(m.mutex_);
        m.history_replay_.reset();
        m.queue_.clear();
        m.undo_.clear();
        m.redo_.clear();
        m.history_progress_.clear();
        m.history_restored_.clear();
    }
};
}  // namespace pulse::ops

namespace {
using pulse::ops::OpsManagerAuditAccess;
using pulse::ops::OpType;
int failures = 0;
void Check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    failures += !ok;
}
std::wstring Item(size_t i) { return L"C:\\b1\\f" + std::to_wstring(i) + L".txt"; }
}

int wmain() {
    pulse::ops::OpsManager m;
    using A = OpsManagerAuditAccess;
    A::SetRunning(m, true);

    // 1200 recycled items: 3 requests (512, 512, 176), newest item first.
    A::AddStep(m, OpType::RecycleDelete, 100, 1200);
    Check(A::Start(m, false), "recycle undo starts");
    Check(A::Requests(m) == 3, "1200 originals replay as 3 restore requests, not 1200");
    auto first = A::Front(m);
    Check(first.type == OpType::RestoreRecycle && first.is_undo && first.undo_of == OpType::RecycleDelete &&
          first.sources.size() == 512 && first.sources.front() == Item(1199) && first.sources.back() == Item(688),
          "first batch: 512 originals in reverse order");
    A::Finish(m, first.sources, true);
    auto second = A::Front(m);
    Check(second.sources.size() == 512 && second.sources.front() == Item(687), "second batch follows the first");

    // Second batch puts back 100 items, then fails (one ambiguous version, a lock...).
    std::vector<std::wstring> some(second.sources.begin(), second.sources.begin() + 100);
    A::Finish(m, some, false);
    Check(!A::Replaying(m) && A::UndoCount(m) == 1 && A::RedoCount(m) == 0, "partial failure stops; step stays undoable");
    Check(A::RestoredKept(m) == 612, "the 612 originals actually put back are recorded");

    // Retry restores only the 588 still in the bin.
    Check(A::Start(m, false), "retry starts");
    Check(A::Requests(m) == 2, "retry replays only the remaining 588 (2 requests)");
    auto retry = A::Front(m);
    bool skipped = retry.sources.size() == 512 && retry.sources.front() == Item(587);
    for (const auto& source : retry.sources)
        for (const auto& done : some) skipped = skipped && source != done;
    Check(skipped, "retry skips every restored original and keeps reverse order");
    A::Finish(m, retry.sources, true);
    auto last = A::Front(m);
    Check(last.sources.size() == 76 && last.sources.back() == Item(0), "last retry batch ends at the oldest item");
    A::Finish(m, last.sources, true);
    Check(!A::Replaying(m) && A::UndoCount(m) == 0 && A::RedoCount(m) == 1 && A::RestoredKept(m) == 0,
          "step completes, moves to redo, and the retry record is dropped");

    // Redo still recycles every original (the entry kept all 1200 sources).
    Check(A::Start(m, true), "redo starts");
    auto redo = A::Front(m);
    Check(A::Requests(m) == 1200 && redo.type == OpType::RecycleDelete && redo.sources.size() == 1 && redo.sources.front() == Item(0),
          "redo recycles all 1200 originals, unchanged per-item order");
    A::Reset(m);

    // A failure reported after every item came back (e.g. cancel after the
    // last one): the retry has nothing left to restore and just completes.
    A::AddStep(m, OpType::RecycleDelete, 200, 3);
    Check(A::Start(m, false) && A::Requests(m) == 1, "3 originals replay as 1 request");
    auto all = A::Front(m);
    A::Finish(m, all.sources, false);
    Check(A::UndoCount(m) == 1, "failure with all restored keeps the step until retried");
    Check(A::Start(m, false), "retry with nothing left reports started");
    Check(!A::Replaying(m) && A::UndoCount(m) == 0 && A::RedoCount(m) == 1, "retry with nothing left completes the step");
    A::Reset(m);

    // Other step types keep one request per item.
    A::AddStep(m, OpType::Move, 300, 5);
    Check(A::Start(m, false) && A::Requests(m) == 5 && A::Front(m).type == OpType::Move, "move undo still replays per item");
    A::Reset(m);

    A::SetRunning(m, false);
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}

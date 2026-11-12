// Headless console unit test for the native runtime global worker budget pool
// (PLG-09 / C2). Depends only on worker_budget.cpp and tasks_adapter.cpp with
// zero USER32.dll or GDI32.dll imports.

#include "../app/extensions/native/tasks_adapter.h"
#include "../app/extensions/native/worker_budget.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace pulse::extensions::native;

int g_passes = 0;
int g_failures = 0;

void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (ok) {
        ++g_passes;
    } else {
        ++g_failures;
    }
}

PulseStringView View(const char* text) noexcept {
    return {text, static_cast<uint32_t>(std::strlen(text))};
}

void UpdatePeak(std::atomic<size_t>& peak, size_t sample) noexcept {
    size_t observed = peak.load(std::memory_order_relaxed);
    while (sample > observed &&
           !peak.compare_exchange_weak(observed, sample, std::memory_order_relaxed)) {
    }
}

class PoolLimitGuard final {
public:
    PoolLimitGuard(WorkerPermitPool& pool, size_t temp_limit) noexcept
        : pool_(pool), previous_(pool.Limit()) {
        pool_.SetLimitForTesting(temp_limit);
    }
    ~PoolLimitGuard() noexcept {
        pool_.SetLimitForTesting(previous_);
    }
    PoolLimitGuard(const PoolLimitGuard&) = delete;
    PoolLimitGuard& operator=(const PoolLimitGuard&) = delete;

private:
    WorkerPermitPool& pool_;
    size_t previous_;
};

struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool open = false;

    void Open() noexcept {
        {
            std::lock_guard lock(mutex);
            open = true;
        }
        cv.notify_all();
    }

    bool WaitOpen(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, timeout, [this] { return open; });
    }
};

struct WakeCoordinator {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<uint8_t> dirty;

    explicit WakeCoordinator(size_t count) : dirty(count, 0) {}

    void Signal(size_t index) noexcept {
        {
            std::lock_guard lock(mutex);
            if (index < dirty.size()) {
                dirty[index] = 1;
            }
        }
        cv.notify_one();
    }

    bool TakeDirty(std::vector<size_t>& indices, std::chrono::milliseconds wait_budget) {
        indices.clear();
        std::unique_lock lock(mutex);
        auto any_dirty = [this] {
            return std::any_of(dirty.begin(), dirty.end(), [](uint8_t v) { return v != 0; });
        };
        if (!any_dirty()) {
            cv.wait_for(lock, wait_budget, any_dirty);
        }
        for (size_t i = 0; i < dirty.size(); ++i) {
            if (dirty[i] != 0) {
                dirty[i] = 0;
                indices.push_back(i);
            }
        }
        return !indices.empty();
    }
};

TasksBackend MakeNoopBackend(std::atomic<size_t>* finished_counter = nullptr,
                             std::atomic<size_t>* released_counter = nullptr) {
    TasksBackend backend;
    backend.create = [](const TaskSnapshot&) { return true; };
    backend.update = [](const TaskSnapshot&) {};
    backend.finish = [finished_counter](const TaskSnapshot&) {
        if (finished_counter) {
            finished_counter->fetch_add(1, std::memory_order_relaxed);
        }
    };
    backend.cancelled = [](uint64_t) { return false; };
    backend.release = [released_counter](uint64_t) {
        if (released_counter) {
            released_counter->fetch_add(1, std::memory_order_relaxed);
        }
    };
    return backend;
}

void PULSE_CALL CaptureTaskState(void* user, const PulseTaskSnapshot* snapshot) {
    if (snapshot) {
        *static_cast<PulseTaskState*>(user) = snapshot->state;
    }
}

PulseTaskState PULSE_CALL CountUnexpectedRun(void* user, const PulseTaskContext*) {
    static_cast<std::atomic<int>*>(user)->fetch_add(1, std::memory_order_acq_rel);
    return PULSE_TASK_COMPLETED;
}

PulseTaskState ReadTaskState(const PulseTasksV1& api, PulseTaskId id) {
    PulseTaskState result = static_cast<PulseTaskState>(999);
    api.get(api.context, id, CaptureTaskState, &result);
    return result;
}

// 1. Default capacities and WorkerPermit RAII / move / idempotent Release semantics.
void TestDefaultCapacitiesAndPermitSemantics() {
    Check(GlobalTaskWorkerPool().Limit() == kDefaultGlobalTaskWorkers &&
              kDefaultGlobalTaskWorkers == 8 &&
              GlobalTaskWorkerPool().ActiveCount() == 0,
          "default global task pool limit is 8 with 0 active permits");
    Check(GlobalPreviewWorkerPool().Limit() == kDefaultGlobalPreviewWorkers &&
              kDefaultGlobalPreviewWorkers == 4 &&
              GlobalPreviewWorkerPool().ActiveCount() == 0,
          "default global preview pool limit is 4 with 0 active permits");

    WorkerPermitPool local_pool(2);
    Check(local_pool.Limit() == 2 && local_pool.ActiveCount() == 0,
          "custom WorkerPermitPool initializes limit and zero active count");

    WorkerPermit empty_permit;
    Check(!empty_permit, "default-constructed WorkerPermit evaluates to false");
    empty_permit.Release();
    Check(local_pool.ActiveCount() == 0, "releasing empty WorkerPermit is a safe no-op");

    {
        WorkerPermit p1 = local_pool.TryAcquire();
        WorkerPermit p2 = local_pool.TryAcquire();
        WorkerPermit p3 = local_pool.TryAcquire();
        Check(static_cast<bool>(p1) && static_cast<bool>(p2) && !p3 && local_pool.ActiveCount() == 2,
              "TryAcquire enforces pool capacity and returns empty permit when full");

        WorkerPermit moved_ctor(std::move(p1));
        Check(!p1 && static_cast<bool>(moved_ctor) && local_pool.ActiveCount() == 2,
              "move construction transfers ownership without changing active count");

        // Move-assigning over a live permit must release the target's old permit first.
        moved_ctor = std::move(p2);
        Check(!p2 && static_cast<bool>(moved_ctor) && local_pool.ActiveCount() == 1,
              "move assignment releases overwritten permit immediately");

        moved_ctor.Release();
        moved_ctor.Release();
        Check(!moved_ctor && local_pool.ActiveCount() == 0,
              "WorkerPermit::Release is idempotent");

        WorkerPermit scoped = local_pool.TryAcquire();
        Check(static_cast<bool>(scoped) && local_pool.ActiveCount() == 1,
              "permit re-acquired after Release frees slot");
    }
    Check(local_pool.ActiveCount() == 0,
          "WorkerPermit destructor automatically returns permit to pool");

    local_pool.SetLimitForTesting(0);
    Check(local_pool.Limit() == 1, "SetLimitForTesting clamps minimum limit to 1");
}

// 2. Waiter registration, unregistration, and lock-free callback re-entrancy.
void TestWaiterReentrancyAndLifecycle() {
    WorkerPermitPool pool(1);
    WorkerPermit held = pool.TryAcquire();
    Check(static_cast<bool>(held) && !pool.TryAcquire(), "pool of 1 saturated before waiter registration");

    int waiter1_calls = 0;
    int waiter2_calls = 0;
    WorkerPermit reentrant_acquired;
    uint64_t transient_id = 0;

    const uint64_t id1 = pool.RegisterWaiter([&] {
        ++waiter1_calls;
        // Re-enter pool methods directly inside waiter notification: must not deadlock!
        (void)pool.ActiveCount();
        (void)pool.Limit();
        transient_id = pool.RegisterWaiter([] {});
        pool.UnregisterWaiter(transient_id);
        if (!reentrant_acquired) {
            reentrant_acquired = pool.TryAcquire();
        }
    });
    const uint64_t id2 = pool.RegisterWaiter([&] {
        ++waiter2_calls;
    });
    Check(id1 != 0 && id2 != 0 && id1 != id2 && pool.RegisterWaiter({}) == 0,
          "RegisterWaiter assigns unique non-zero IDs and rejects empty functor");

    held.Release();
    Check(waiter1_calls == 1 && waiter2_calls == 1 &&
              static_cast<bool>(reentrant_acquired) && pool.ActiveCount() == 1,
          "Release notifies waiters outside lock and permits re-entrant TryAcquire without deadlock");

    // Raising the limit while saturated wakes waiters outside the lock as well.
    pool.SetLimitForTesting(2);
    Check(waiter1_calls == 2 && waiter2_calls == 2,
          "SetLimitForTesting notifies registered waiters when limit increases");

    // Lowering the limit does not spuriously notify waiters.
    pool.SetLimitForTesting(1);
    Check(waiter1_calls == 2 && waiter2_calls == 2,
          "lowering pool limit does not trigger waiter notifications");

    pool.UnregisterWaiter(id1);
    pool.UnregisterWaiter(id1); // Idempotent double-unregister.
    pool.UnregisterWaiter(0);   // Safe zero-ID unregister.

    reentrant_acquired.Release();
    Check(waiter1_calls == 2 && waiter2_calls == 3 && pool.ActiveCount() == 0,
          "unregistered waiter receives no further notifications while remaining waiter still fires");

    pool.UnregisterWaiter(id2);
}

// 3. Multi-session TasksAdapter global budget contention (4 sessions x 2 tasks, limit = 3).
void TestMultiSessionTasksBudgetContention() {
    PoolLimitGuard limit_guard(GlobalTaskWorkerPool(), 3);

    constexpr size_t kSessionCount = 4;
    constexpr size_t kTasksPerSession = 2;
    constexpr size_t kTotalTasks = kSessionCount * kTasksPerSession;

    WakeCoordinator coordinator(kSessionCount);
    std::atomic<size_t> finished_total{0};
    std::atomic<size_t> released_total{0};

    struct SessionSlot {
        std::unique_ptr<TasksAdapter> adapter;
        PulseTasksV1 api{};
        std::array<PulseTaskId, kTasksPerSession> ids{};
    };
    std::array<SessionSlot, kSessionCount> sessions;

    for (size_t i = 0; i < kSessionCount; ++i) {
        // Exercise both owner == nullptr (pure console) and opaque non-null handles.
        const HWND owner = (i == 0) ? nullptr : reinterpret_cast<HWND>(static_cast<uintptr_t>(0x1000 + i));
        sessions[i].adapter = std::make_unique<TasksAdapter>(
            owner,
            [] { return true; },
            MakeNoopBackend(&finished_total, &released_total),
            [&coordinator, i] { coordinator.Signal(i); },
            "bench.pkg." + std::to_string(i));
        Check(sessions[i].adapter->Query(PULSE_TASKS_VERSION_1, &sessions[i].api, sizeof(PulseTasksV1)) == PULSE_OK,
              "query PulseTasksV1 for multi-session contention slot");
    }

    struct ContentionContext {
        Gate gate;
        std::mutex entered_mutex;
        std::condition_variable entered_cv;
        size_t entered_total = 0;
        std::atomic<size_t> active_workers{0};
        std::atomic<size_t> peak_workers{0};
        std::atomic<size_t> completed_workers{0};

        static PulseTaskState PULSE_CALL Run(void* user, const PulseTaskContext*) {
            auto& self = *static_cast<ContentionContext*>(user);
            const size_t current = self.active_workers.fetch_add(1, std::memory_order_acq_rel) + 1;
            UpdatePeak(self.peak_workers, current);
            {
                std::lock_guard lock(self.entered_mutex);
                ++self.entered_total;
            }
            self.entered_cv.notify_all();

            const bool released = self.gate.WaitOpen(std::chrono::seconds(5));
            // Brief overlap window so any over-admission bug would be observed in peak_workers.
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            self.active_workers.fetch_sub(1, std::memory_order_acq_rel);
            self.completed_workers.fetch_add(1, std::memory_order_acq_rel);
            return released ? PULSE_TASK_COMPLETED : PULSE_TASK_FAILED;
        }
    } ctx;

    bool all_created = true;
    for (size_t i = 0; i < kSessionCount; ++i) {
        for (size_t t = 0; t < kTasksPerSession; ++t) {
            const PulseTaskDescriptor desc{
                sizeof(PulseTaskDescriptor),
                View("Global budget task"),
                View("bench.budget"),
                ContentionContext::Run,
                &ctx,
                nullptr,
            };
            if (sessions[i].api.create(sessions[i].api.context, &desc, &sessions[i].ids[t]) != PULSE_OK ||
                sessions[i].ids[t] == 0) {
                all_created = false;
            }
        }
    }
    Check(all_created, "all 8 tasks admitted across 4 independent TasksAdapter sessions");

    bool first_wave_entered = false;
    {
        std::unique_lock lock(ctx.entered_mutex);
        first_wave_entered = ctx.entered_cv.wait_for(
            lock, std::chrono::seconds(5), [&] { return ctx.entered_total >= 3; });
    }
    // Give any buggy extra worker thread a chance to start before sampling.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    size_t adapter_active_sum = 0;
    size_t adapter_queued_sum = 0;
    for (const auto& s : sessions) {
        adapter_active_sum += s.adapter->ActiveWorkerCount();
        adapter_queued_sum += s.adapter->QueuedCount();
    }

    Check(first_wave_entered &&
              ctx.active_workers.load(std::memory_order_acquire) == 3 &&
              ctx.peak_workers.load(std::memory_order_acquire) == 3 &&
              GlobalTaskWorkerPool().ActiveCount() == 3 &&
              adapter_active_sum == 3 &&
              adapter_queued_sum == 5,
          "first wave caps active workers strictly at 3 across 4 sessions while 5 tasks queue");

    // Release the gate; drive sessions strictly via wake notifications + Poll() until all 8 finish.
    ctx.gate.Open();

    std::vector<size_t> woken;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline &&
           finished_total.load(std::memory_order_acquire) < kTotalTasks) {
        if (coordinator.TakeDirty(woken, std::chrono::milliseconds(50))) {
            for (const size_t idx : woken) {
                sessions[idx].adapter->Poll();
            }
        }
    }
    for (auto& s : sessions) {
        s.adapter->Poll();
    }

    bool all_completed_terminal = (finished_total.load(std::memory_order_acquire) == kTotalTasks);
    for (size_t i = 0; i < kSessionCount; ++i) {
        for (size_t t = 0; t < kTasksPerSession; ++t) {
            if (ReadTaskState(sessions[i].api, sessions[i].ids[t]) != PULSE_TASK_COMPLETED) {
                all_completed_terminal = false;
            }
            if (sessions[i].api.release(sessions[i].api.context, sessions[i].ids[t]) != PULSE_OK) {
                all_completed_terminal = false;
            }
        }
    }

    Check(all_completed_terminal &&
              ctx.completed_workers.load(std::memory_order_acquire) == kTotalTasks &&
              ctx.peak_workers.load(std::memory_order_acquire) == 3 &&
              GlobalTaskWorkerPool().ActiveCount() == 0 &&
              released_total.load(std::memory_order_acquire) == kTotalTasks,
          "waiter wakeups + Poll drain all 8 tasks with peak concurrency == 3 and final ActiveCount == 0");
}

// 4. Cancellation while queued for global permit skips worker execution and leaks no permit.
void TestCancelWhileQueuedInPool() {
    PoolLimitGuard limit_guard(GlobalTaskWorkerPool(), 1);

    WakeCoordinator coordinator(2);
    TasksAdapter session1(
        nullptr, [] { return true; }, MakeNoopBackend(), [&] { coordinator.Signal(0); }, "pkg.one");
    TasksAdapter session2(
        nullptr, [] { return true; }, MakeNoopBackend(), [&] { coordinator.Signal(1); }, "pkg.two");

    PulseTasksV1 api1{};
    PulseTasksV1 api2{};
    session1.Query(PULSE_TASKS_VERSION_1, &api1, sizeof(api1));
    session2.Query(PULSE_TASKS_VERSION_1, &api2, sizeof(api2));

    struct BlockerWork {
        Gate gate;
        std::mutex mutex;
        std::condition_variable cv;
        bool entered = false;

        static PulseTaskState PULSE_CALL Run(void* user, const PulseTaskContext*) {
            auto& self = *static_cast<BlockerWork*>(user);
            {
                std::lock_guard lock(self.mutex);
                self.entered = true;
            }
            self.cv.notify_all();
            return self.gate.WaitOpen(std::chrono::seconds(5)) ? PULSE_TASK_COMPLETED : PULSE_TASK_FAILED;
        }
    } blocker;

    struct QueuedCandidate {
        std::atomic<int> work_entered{0};
        std::atomic<int> cancel_callbacks{0};

        static PulseTaskState PULSE_CALL Run(void* user, const PulseTaskContext*) {
            static_cast<QueuedCandidate*>(user)->work_entered.fetch_add(1, std::memory_order_acq_rel);
            return PULSE_TASK_COMPLETED;
        }
        static void PULSE_CALL OnCancel(void* user, PulseTaskId) {
            static_cast<QueuedCandidate*>(user)->cancel_callbacks.fetch_add(1, std::memory_order_acq_rel);
        }
    } cancelled_task, follow_up_task;

    PulseTaskId id_blocker = 0;
    const PulseTaskDescriptor blocker_desc{
        sizeof(PulseTaskDescriptor), View("Blocker"), View("bench"), BlockerWork::Run, &blocker, nullptr};
    Check(api1.create(api1.context, &blocker_desc, &id_blocker) == PULSE_OK && id_blocker != 0,
          "session 1 starts blocker task occupying single global permit");

    {
        std::unique_lock lock(blocker.mutex);
        Check(blocker.cv.wait_for(lock, std::chrono::seconds(5), [&] { return blocker.entered; }) &&
                  GlobalTaskWorkerPool().ActiveCount() == 1,
              "global task pool saturated at 1 by session 1");
    }

    PulseTaskId id_cancelled = 0;
    const PulseTaskDescriptor cancel_desc{
        sizeof(PulseTaskDescriptor),
        View("Queued cancel target"),
        View("bench"),
        QueuedCandidate::Run,
        &cancelled_task,
        QueuedCandidate::OnCancel,
    };
    PulseTaskId id_followup = 0;
    const PulseTaskDescriptor followup_desc{
        sizeof(PulseTaskDescriptor),
        View("Queued follow-up"),
        View("bench"),
        QueuedCandidate::Run,
        &follow_up_task,
        QueuedCandidate::OnCancel,
    };

    Check(api2.create(api2.context, &cancel_desc, &id_cancelled) == PULSE_OK &&
              api2.create(api2.context, &followup_desc, &id_followup) == PULSE_OK &&
              session2.QueuedCount() == 2,
          "session 2 queues two tasks while global permit pool is full");

    // Cancel the first queued task before freeing session 1's permit.
    Check(session2.Cancel(id_cancelled) == PULSE_OK && session2.QueuedCount() == 1,
          "cancelling queued task immediately decrements unstarted queued count");

    blocker.gate.Open();

    std::vector<size_t> woken;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (coordinator.TakeDirty(woken, std::chrono::milliseconds(30))) {
            for (const size_t idx : woken) {
                if (idx == 0) session1.Poll();
                if (idx == 1) session2.Poll();
            }
        }
        session1.Poll();
        session2.Poll();
        if (!session1.CallbackActive() && !session2.CallbackActive()) {
            break;
        }
    }

    Check(cancelled_task.work_entered.load(std::memory_order_acquire) == 0 &&
              cancelled_task.cancel_callbacks.load(std::memory_order_acquire) == 1 &&
              ReadTaskState(api2, id_cancelled) == PULSE_TASK_CANCELLED,
          "queued cancelled task never executes background worker and finalizes as Cancelled");
    Check(follow_up_task.work_entered.load(std::memory_order_acquire) == 1 &&
              ReadTaskState(api2, id_followup) == PULSE_TASK_COMPLETED &&
              GlobalTaskWorkerPool().ActiveCount() == 0,
          "subsequent queued task acquires freed permit cleanly with zero permit leak");

    api1.release(api1.context, id_blocker);
    api2.release(api2.context, id_cancelled);
    api2.release(api2.context, id_followup);
}

// 5. Close/destruction safety while queued + GlobalPreviewWorkerPool 4-worker cap & LIFO wakeup order.
void TestCloseDestructionSafetyAndPreviewPoolCap() {
    // Part 5A: Close() and ~TasksAdapter() while tasks are queued waiting for a global permit.
    {
        PoolLimitGuard limit_guard(GlobalTaskWorkerPool(), 1);

        struct HoldState {
            Gate gate;
            std::mutex mutex;
            std::condition_variable cv;
            bool entered = false;
            static PulseTaskState PULSE_CALL Run(void* user, const PulseTaskContext*) {
                auto& s = *static_cast<HoldState*>(user);
                {
                    std::lock_guard lock(s.mutex);
                    s.entered = true;
                }
                s.cv.notify_all();
                return s.gate.WaitOpen(std::chrono::seconds(5)) ? PULSE_TASK_COMPLETED : PULSE_TASK_FAILED;
            }
        } holder;

        std::atomic<int> closed_session_runs{0};
        std::atomic<int> destroyed_session_runs{0};
        std::atomic<int> late_wakes_closed{0};
        std::atomic<int> late_wakes_destroyed{0};

        TasksAdapter keeper(nullptr, [] { return true; }, MakeNoopBackend());
        PulseTasksV1 keeper_api{};
        keeper.Query(PULSE_TASKS_VERSION_1, &keeper_api, sizeof(keeper_api));
        PulseTaskId keeper_id = 0;
        const PulseTaskDescriptor hold_desc{
            sizeof(PulseTaskDescriptor), View("Hold"), View("bench"), HoldState::Run, &holder, nullptr};
        keeper_api.create(keeper_api.context, &hold_desc, &keeper_id);
        {
            std::unique_lock lock(holder.mutex);
            holder.cv.wait_for(lock, std::chrono::seconds(5), [&] { return holder.entered; });
        }

        auto closing_session = std::make_unique<TasksAdapter>(
            nullptr,
            [] { return true; },
            MakeNoopBackend(),
            [&] { late_wakes_closed.fetch_add(1, std::memory_order_relaxed); },
            "pkg.closing");
        auto dying_session = std::make_unique<TasksAdapter>(
            nullptr,
            [] { return true; },
            MakeNoopBackend(),
            [&] { late_wakes_destroyed.fetch_add(1, std::memory_order_relaxed); },
            "pkg.dying");

        PulseTasksV1 closing_api{};
        PulseTasksV1 dying_api{};
        closing_session->Query(PULSE_TASKS_VERSION_1, &closing_api, sizeof(closing_api));
        dying_session->Query(PULSE_TASKS_VERSION_1, &dying_api, sizeof(dying_api));

        for (int i = 0; i < 2; ++i) {
            PulseTaskId out_a = 0;
            PulseTaskId out_b = 0;
            const PulseTaskDescriptor d1{
                sizeof(PulseTaskDescriptor), View("Closing"), View("bench"), CountUnexpectedRun, &closed_session_runs, nullptr};
            const PulseTaskDescriptor d2{
                sizeof(PulseTaskDescriptor), View("Dying"), View("bench"), CountUnexpectedRun, &destroyed_session_runs, nullptr};
            closing_api.create(closing_api.context, &d1, &out_a);
            dying_api.create(dying_api.context, &d2, &out_b);
        }

        // Explicitly Close() one queued session and completely destroy the other before permit release.
        closing_session->Close();
        dying_session.reset();

        const int wakes_closed_before = late_wakes_closed.load(std::memory_order_acquire);
        const int wakes_destroyed_before = late_wakes_destroyed.load(std::memory_order_acquire);

        holder.gate.Open();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline && keeper.CallbackActive()) {
            keeper.Poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        keeper_api.release(keeper_api.context, keeper_id);

        Check(closed_session_runs.load(std::memory_order_acquire) == 0 &&
                  destroyed_session_runs.load(std::memory_order_acquire) == 0 &&
                  late_wakes_closed.load(std::memory_order_acquire) == wakes_closed_before &&
                  late_wakes_destroyed.load(std::memory_order_acquire) == wakes_destroyed_before &&
                  GlobalTaskWorkerPool().ActiveCount() == 0,
              "Close and destructor unregister pool waiters cleanly with zero dangling callbacks or leaks");
    }

    // Part 5B: GlobalPreviewWorkerPool() 4-worker cap across simulated multi-window sessions + sequence order.
    {
        Check(GlobalPreviewWorkerPool().Limit() == 4 && GlobalPreviewWorkerPool().ActiveCount() == 0,
              "GlobalPreviewWorkerPool ready at limit 4 before multi-window simulation");

        constexpr size_t kWindowCount = 3;
        WakeCoordinator preview_wakes(kWindowCount);
        Gate first_preview_wave_gate;
        std::mutex wave_mutex;
        std::condition_variable wave_cv;
        size_t first_wave_entered = 0;

        std::atomic<size_t> active_previews{0};
        std::atomic<size_t> peak_previews{0};
        std::mutex order_mutex;
        std::vector<uint64_t> window2_execution_order;
        std::atomic<size_t> completed_previews{0};
        std::atomic<int> cancelled_preview_runs{0};

        // Headless model of PreviewsAdapter's per-window 2-worker + GlobalPreviewWorkerPool(4) + sequence priority.
        struct SimulatedPreviewWindow : std::enable_shared_from_this<SimulatedPreviewWindow> {
            struct Job {
                uint64_t sequence = 0;
                bool block_on_wave = false;
                bool record_window2 = false;
                std::atomic<bool> cancelled{false};
                bool started = false;
            };
            size_t window_index = 0;
            std::function<void()> wake;
            std::atomic<bool> closed{false};
            std::atomic<unsigned> workers{0};
            std::atomic<size_t> queued_count{0};
            std::atomic<uint64_t> waiter_id{0};
            std::vector<std::shared_ptr<Job>> jobs;

            Gate* wave_gate = nullptr;
            std::mutex* wave_mtx = nullptr;
            std::condition_variable* wave_cond = nullptr;
            size_t* wave_entered = nullptr;
            std::atomic<size_t>* global_active = nullptr;
            std::atomic<size_t>* global_peak = nullptr;
            std::mutex* order_mtx = nullptr;
            std::vector<uint64_t>* win2_order = nullptr;
            std::atomic<size_t>* completed = nullptr;
            std::atomic<int>* cancelled_runs = nullptr;

            ~SimulatedPreviewWindow() { Close(); }

            void Register() {
                std::weak_ptr<SimulatedPreviewWindow> weak = shared_from_this();
                waiter_id.store(
                    GlobalPreviewWorkerPool().RegisterWaiter([weak]() noexcept {
                        if (const auto s = weak.lock()) {
                            if (!s->closed.load(std::memory_order_acquire) &&
                                s->queued_count.load(std::memory_order_seq_cst) > 0 && s->wake) {
                                s->wake();
                            }
                        }
                    }),
                    std::memory_order_release);
            }

            void Close() noexcept {
                closed.store(true, std::memory_order_release);
                queued_count.store(0, std::memory_order_seq_cst);
                const uint64_t id = waiter_id.exchange(0, std::memory_order_acq_rel);
                if (id != 0) GlobalPreviewWorkerPool().UnregisterWaiter(id);
            }

            size_t CountUnstarted() const noexcept {
                size_t n = 0;
                for (const auto& j : jobs) {
                    if (!j->started && !j->cancelled.load(std::memory_order_acquire)) ++n;
                }
                return n;
            }

            void StartPending() {
                if (closed.load(std::memory_order_relaxed)) {
                    queued_count.store(0, std::memory_order_seq_cst);
                    return;
                }
                std::vector<std::shared_ptr<Job>> pending;
                for (const auto& j : jobs) {
                    if (!j->started && !j->cancelled.load(std::memory_order_acquire)) {
                        pending.push_back(j);
                    }
                }
                if (pending.empty()) {
                    queued_count.store(0, std::memory_order_seq_cst);
                    return;
                }
                std::sort(pending.begin(), pending.end(),
                          [](const auto& a, const auto& b) { return a->sequence > b->sequence; });
                queued_count.store(pending.size(), std::memory_order_seq_cst);
                for (const auto& job : pending) {
                    if (job->started || job->cancelled.load(std::memory_order_acquire)) continue;
                    if (workers.load(std::memory_order_acquire) >= 2) break;
                    auto permit = GlobalPreviewWorkerPool().TryAcquire();
                    if (!permit) break;
                    job->started = true;
                    ++workers;
                    auto self = shared_from_this();
                    std::thread([self, job, permit = std::move(permit)]() mutable {
                        if (job->cancelled.load(std::memory_order_acquire)) {
                            self->cancelled_runs->fetch_add(1, std::memory_order_acq_rel);
                        } else {
                            const size_t cur = self->global_active->fetch_add(1, std::memory_order_acq_rel) + 1;
                            UpdatePeak(*self->global_peak, cur);
                            if (job->record_window2) {
                                std::lock_guard lock(*self->order_mtx);
                                self->win2_order->push_back(job->sequence);
                            }
                            if (job->block_on_wave) {
                                {
                                    std::lock_guard lock(*self->wave_mtx);
                                    ++(*self->wave_entered);
                                }
                                self->wave_cond->notify_all();
                                self->wave_gate->WaitOpen(std::chrono::seconds(5));
                            }
                            std::this_thread::sleep_for(std::chrono::milliseconds(8));
                            self->global_active->fetch_sub(1, std::memory_order_acq_rel);
                            self->completed->fetch_add(1, std::memory_order_acq_rel);
                        }
                        --self->workers;
                        permit.Release();
                        if (self->wake) self->wake();
                    }).detach();
                }
                queued_count.store(CountUnstarted(), std::memory_order_seq_cst);
            }
        };

        std::array<std::shared_ptr<SimulatedPreviewWindow>, kWindowCount> windows;
        for (size_t w = 0; w < kWindowCount; ++w) {
            auto win = std::make_shared<SimulatedPreviewWindow>();
            win->window_index = w;
            win->wake = [&preview_wakes, w] { preview_wakes.Signal(w); };
            win->wave_gate = &first_preview_wave_gate;
            win->wave_mtx = &wave_mutex;
            win->wave_cond = &wave_cv;
            win->wave_entered = &first_wave_entered;
            win->global_active = &active_previews;
            win->global_peak = &peak_previews;
            win->order_mtx = &order_mutex;
            win->win2_order = &window2_execution_order;
            win->completed = &completed_previews;
            win->cancelled_runs = &cancelled_preview_runs;
            win->Register();
            windows[w] = std::move(win);
        }

        // Window 0 and Window 1 each submit 3 blocking-wave jobs (per-window cap = 2 => 2 + 2 = 4 saturate pool).
        uint64_t seq = 0;
        for (size_t w = 0; w < 2; ++w) {
            for (int j = 0; j < 3; ++j) {
                auto job = std::make_shared<SimulatedPreviewWindow::Job>();
                job->sequence = ++seq;
                job->block_on_wave = true;
                windows[w]->jobs.push_back(std::move(job));
            }
            windows[w]->StartPending();
        }

        // Window 2 submits 3 jobs while global preview pool is already saturated at 4:
        // seq 100 (kept), seq 200 (cancelled while queued), seq 300 (latest, must start before seq 100!).
        auto w2_older = std::make_shared<SimulatedPreviewWindow::Job>();
        w2_older->sequence = 100;
        w2_older->record_window2 = true;
        auto w2_cancelled = std::make_shared<SimulatedPreviewWindow::Job>();
        w2_cancelled->sequence = 200;
        w2_cancelled->record_window2 = true;
        auto w2_newest = std::make_shared<SimulatedPreviewWindow::Job>();
        w2_newest->sequence = 300;
        w2_newest->record_window2 = true;

        windows[2]->jobs.push_back(w2_older);
        windows[2]->jobs.push_back(w2_cancelled);
        windows[2]->StartPending();
        // Cancel seq 200 while queued and enqueue seq 300.
        w2_cancelled->cancelled.store(true, std::memory_order_release);
        windows[2]->jobs.push_back(w2_newest);
        windows[2]->StartPending();

        {
            std::unique_lock lock(wave_mutex);
            Check(wave_cv.wait_for(lock, std::chrono::seconds(5), [&] { return first_wave_entered == 4; }),
                  "first 4 preview workers enter across Window 0 and Window 1");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));

        Check(active_previews.load(std::memory_order_acquire) == 4 &&
                  peak_previews.load(std::memory_order_acquire) == 4 &&
                  GlobalPreviewWorkerPool().ActiveCount() == 4 &&
                  windows[2]->workers.load(std::memory_order_acquire) == 0 &&
                  windows[2]->queued_count.load(std::memory_order_acquire) == 2,
              "GlobalPreviewWorkerPool strictly caps concurrent previews at 4 while Window 2 queues 2 live requests");

        // Temporarily setWindow 2 limit to 1 worker at a time? Wait: if 1 permit frees first or if we check
        // StartPending order on Window 2: StartPending sorts descending (300 before 100), so even if only 1
        // permit becomes available first (or we hold Window 2 to 1 permit test), 300 is acquired first!
        // Let's verify exact sequence dispatch order by temporarily occupying 1 slot on Window 2 or checking first push:
        // Even better: in StartPending(), jobs are started sequentially on the UI thread in [300, 100] order; to make
        // worker entry order deterministic regardless of OS thread scheduling, let's verify when pool frees 1 slot!
        // Here 6 jobs exist in W0+W1 (4 running + 2 queued) and 2 live jobs in W2 = 8 total live jobs.
        first_preview_wave_gate.Open();

        std::vector<size_t> woken;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline &&
               completed_previews.load(std::memory_order_acquire) < 8) {
            if (preview_wakes.TakeDirty(woken, std::chrono::milliseconds(40))) {
                for (const size_t idx : woken) {
                    windows[idx]->StartPending();
                }
            }
        }

        // Wait for all detached preview threads to finish releasing permits.
        const auto drain_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < drain_deadline &&
               (GlobalPreviewWorkerPool().ActiveCount() != 0 ||
                windows[0]->workers.load() != 0 ||
                windows[1]->workers.load() != 0 ||
                windows[2]->workers.load() != 0)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        for (auto& win : windows) {
            win->Close();
        }

        // Also test deterministic single-slot wakeup order (newest sequence 500 before 400) on GlobalPreviewWorkerPool:
        bool newest_woke_first = false;
        {
            PoolLimitGuard single_preview_slot(GlobalPreviewWorkerPool(), 1);
            WorkerPermit hold_one = GlobalPreviewWorkerPool().TryAcquire();

            auto seq_win = std::make_shared<SimulatedPreviewWindow>();
            std::atomic<bool> woke{false};
            seq_win->wake = [&] { woke.store(true, std::memory_order_release); };
            seq_win->wave_gate = &first_preview_wave_gate;
            seq_win->wave_mtx = &wave_mutex;
            seq_win->wave_cond = &wave_cv;
            seq_win->wave_entered = &first_wave_entered;
            seq_win->global_active = &active_previews;
            seq_win->global_peak = &peak_previews;
            seq_win->order_mtx = &order_mutex;
            std::vector<uint64_t> single_slot_order;
            seq_win->win2_order = &single_slot_order;
            std::atomic<size_t> seq_done{0};
            seq_win->completed = &seq_done;
            seq_win->cancelled_runs = &cancelled_preview_runs;
            seq_win->Register();

            auto j_old = std::make_shared<SimulatedPreviewWindow::Job>();
            j_old->sequence = 400;
            j_old->record_window2 = true;
            auto j_new = std::make_shared<SimulatedPreviewWindow::Job>();
            j_new->sequence = 500;
            j_new->record_window2 = true;
            seq_win->jobs.push_back(j_old);
            seq_win->jobs.push_back(j_new);
            seq_win->StartPending();

            // Release the single global preview slot: wakeup + StartPending can only launch 1 job (must be 500!).
            hold_one.Release();
            if (woke.exchange(false, std::memory_order_acq_rel)) {
                seq_win->StartPending();
            }
            const auto step1_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (std::chrono::steady_clock::now() < step1_deadline && seq_done.load() < 1) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            while (std::chrono::steady_clock::now() < step1_deadline && GlobalPreviewWorkerPool().ActiveCount() != 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            seq_win->StartPending();
            while (std::chrono::steady_clock::now() < step1_deadline &&
                   (seq_done.load() < 2 || seq_win->workers.load() != 0 || GlobalPreviewWorkerPool().ActiveCount() != 0)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            seq_win->Close();
            newest_woke_first = (single_slot_order.size() == 2 &&
                                 single_slot_order[0] == 500 &&
                                 single_slot_order[1] == 400);
        }

        Check(completed_previews.load(std::memory_order_acquire) == 8 &&
                  cancelled_preview_runs.load(std::memory_order_acquire) == 0 &&
                  peak_previews.load(std::memory_order_acquire) == 4 &&
                  newest_woke_first &&
                  GlobalPreviewWorkerPool().ActiveCount() == 0,
              "multi-window preview pool enforces 4-worker ceiling, skips cancelled jobs, wakes newest sequence first, and drains to 0");
    }
}

} // namespace

int main() {
    TestDefaultCapacitiesAndPermitSemantics();
    TestWaiterReentrancyAndLifecycle();
    TestMultiSessionTasksBudgetContention();
    TestCancelWhileQueuedInPool();
    TestCloseDestructionSafetyAndPreviewPoolCap();

    std::printf("%d PASS, %d FAIL\n", g_passes, g_failures);
    return g_failures == 0 ? 0 : 1;
}

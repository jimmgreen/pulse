#include "../ops/transfer_device_budget.h"
#include <cstdio>
#include <windows.h>
#include <utility>
#include <thread>
#include <chrono>
using namespace pulse::ops;
int main() {
    struct FairEnvironment {
        const wchar_t* name;
        std::wstring previous;
        bool existed = false;
        explicit FairEnvironment(const wchar_t* variable = L"PULSE_FAIR_TRANSFER_BUDGET") : name(variable) {
            const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
            existed = size != 0;
            if (existed) {
                previous.resize(size);
                const DWORD used = GetEnvironmentVariableW(name, previous.data(), size);
                previous.resize(used);
            }
            SetEnvironmentVariableW(name, L"1");
        }
        ~FairEnvironment() {
            SetEnvironmentVariableW(name, existed ? previous.c_str() : nullptr);
        }
    } fair_environment;
    FairEnvironment wake_environment(L"PULSE_TRANSFER_BUDGET_WAKE");
    int failures = 0;
    const auto check = [&](bool ok, const char* name) {
        printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) ++failures;
    };
    TransferDeviceIdentity hdd{{L"disk:0"}, 1}, partition{{L"disk:0"}, 1};
    TransferDeviceIdentity ssd{{L"disk:1"}, 4}, other{{L"disk:2"}, 4}, unknown;
    TransferDeviceBudget budget;
    auto copy = budget.TryAcquire(hdd, partition, 10);
    check(static_cast<bool>(copy), "same disk charged once");
    check(!budget.TryAcquire(partition, other, 10), "partition shares physical disk budget");
    auto independent = budget.TryAcquire(ssd, other, 10);
    check(static_cast<bool>(independent), "independent devices overlap");
    check(!budget.TryAcquire(unknown, ssd, 10), "unknown waits for all known work");
    copy.Reset(); independent.Reset();
    auto exclusive = budget.TryAcquire(unknown, other, 10);
    check(static_cast<bool>(exclusive) && !budget.TryAcquire(ssd, other, 10), "unknown topology runs alone");
    exclusive.Reset();
    auto cross = budget.TryAcquire(ssd, hdd, 10);
    check(static_cast<bool>(cross) && !budget.TryAcquire(other, hdd, 10), "copy and verification share target tokens");
    cross.Reset();
    auto verify = budget.TryAcquire(other, hdd, 10);
    check(static_cast<bool>(verify), "released copy budget permits verification");
    auto moved = std::move(verify);
    verify.Reset();
    check(budget.Active() == 1, "moving lease does not release live resources");
    moved.Reset(); moved.Reset();
    check(budget.Active() == 0, "lease release is idempotent");
    auto a = budget.TryAcquire(ssd, other, 128ull << 20);
    auto b = budget.TryAcquire(ssd, other, 128ull << 20);
    check(a && b && !budget.TryAcquire(ssd, other, 1), "byte budget bounds active work");
    a.Reset(); b.Reset();
    auto oversized = budget.TryAcquire(ssd, other, 512ull << 20);
    check(oversized && !budget.TryAcquire(ssd, other, 1), "oversized job allowed only alone");
    oversized.Reset();
    TransferDeviceBudget::Lease leases[4];
    for (auto& lease : leases) lease = budget.TryAcquire(ssd, other, 1);
    check(budget.Active() == 4 && !budget.TryAcquire(ssd, other, 1), "solid state capacity is bounded");
    for (auto& lease : leases) lease.Reset();
    {
        auto scoped = budget.TryAcquire(hdd, other, 1);
        check(static_cast<bool>(scoped), "scope lease acquired");
    }
    check(budget.Active() == 0, "destruction releases tokens");
    {
        TransferDeviceBudget many;
        TransferDeviceIdentity first{{L"span:0", L"span:1", L"span:2", L"span:3", L"span:4", L"span:4"}, 4};
        TransferDeviceIdentity second{{L"span:4", L"span:5", L"span:6"}, 1};
        auto spanning = many.TryAcquire(first, second, 17);
        check(spanning && many.Active() == 1, "overflow resources acquired with duplicate endpoints");
        for (unsigned i = 0; i < 7; ++i) {
            TransferDeviceIdentity probe{{L"span:" + std::to_wstring(i)}, 1};
            check(!many.TryAcquire(probe, probe, 1), "every overflow resource remains charged");
        }
        // Many insertions exercise map growth while a multi-resource lease remains active.
        bool independent_admission = true;
        for (unsigned i = 0; i < 256; ++i) {
            TransferDeviceIdentity unrelated{{L"rehash:" + std::to_wstring(i)}, 1};
            auto transient = many.TryAcquire(unrelated, unrelated, 1);
            independent_admission = independent_admission && static_cast<bool>(transient);
        }
        check(independent_admission, "map growth preserves independent admission");
        TransferDeviceBudget::Lease relocated;
        relocated = std::move(spanning);
        spanning.Reset();
        check(many.Active() == 1, "overflow move assignment retains all tokens after rehash");
        relocated.Reset();
        check(many.Active() == 0, "overflow reset releases after map rehash");
        auto reacquired = many.TryAcquire(first, second, 17);
        check(static_cast<bool>(reacquired), "overflow resources fully reusable");
    }
    {
        TransferDeviceBudget lifetime;
        TransferDeviceBudget::Lease retained;
        {
            TransferDeviceBudget owner = lifetime;
            TransferDeviceIdentity temporary{{L"lifetime:0", L"lifetime:1"}, 1};
            retained = owner.TryAcquire(temporary, temporary, 23);
        }
        check(lifetime.Active() == 1, "lease survives endpoint strings and budget wrapper destruction");
        TransferDeviceIdentity replacement{{L"lifetime:2"}, 1};
        auto next = lifetime.TryAcquire(replacement, replacement, 5);
        retained = std::move(next);
        check(lifetime.Active() == 1, "move assignment releases previous tokens exactly once");
        retained.Reset();
        check(lifetime.Active() == 0, "moved lease restores global counts");
    }
    {
        TransferDeviceBudget::Lease sole_owner;
        {
            TransferDeviceBudget ephemeral;
            TransferDeviceIdentity device{{L"sole-owner"}, 1};
            sole_owner = ephemeral.TryAcquire(device, device, 1);
        }
        check(static_cast<bool>(sole_owner), "lease keeps resource state alive without budget wrapper");
        sole_owner.Reset();
        check(!sole_owner, "last-owner lease releases safely");
    }
    {
        TransferDeviceBudget fair;
        auto task_a = TransferDeviceBudget::CreateTask();
        auto task_b = TransferDeviceBudget::CreateTask();
        auto a1 = fair.Wait(hdd, hdd, 1, task_a);
        auto a2 = fair.Wait(hdd, hdd, 1, task_a);
        auto b1 = fair.Wait(hdd, hdd, 1, task_b);
        check(!fair.TryAcquire(b1) && !fair.TryAcquire(a2), "older conflicting ticket wins initial turn");
        auto first = fair.TryAcquire(a1);
        check(first && !a1 && fair.Waiting() == 2, "grant consumes only its ticket");
        first.Reset();
        check(!fair.TryAcquire(a2) && !fair.TryAcquire(hdd, hdd, 1), "task with more workers and unticketed calls cannot bypass other task");
        auto second = fair.TryAcquire(b1);
        check(static_cast<bool>(second), "next registered task receives turn before same-task second worker");
        second.Reset();
        auto third = fair.TryAcquire(a2);
        check(third && fair.Waiting() == 0, "first task resumes after competing task grant");
    }
    {
        TransferDeviceBudget fair;
        auto blocker = fair.TryAcquire(hdd, hdd, 1);
        auto older = fair.Wait(hdd, hdd, 1, TransferDeviceBudget::CreateTask());
        auto independent_work = fair.TryAcquire(ssd, other, 1);
        check(static_cast<bool>(independent_work), "unrelated resources proceed with room for older waiter");
        independent_work.Reset(); blocker.Reset();
        auto later = fair.Wait(hdd, hdd, 1, TransferDeviceBudget::CreateTask());
        check(!fair.TryAcquire(later), "persistent waiter retains place across retries");
        auto relocated = std::move(older);
        older.Reset();
        check(fair.Waiting() == 2, "moving waiter preserves registration");
        relocated.Reset();
        auto next = fair.TryAcquire(later);
        check(next && fair.Waiting() == 0, "cancelled or paused ticket no longer blocks successor");
    }
    {
        TransferDeviceBudget fair;
        auto active = fair.TryAcquire(ssd, other, 1);
        auto exclusive_wait = fair.Wait(unknown, other, 1, TransferDeviceBudget::CreateTask());
        check(!fair.TryAcquire(ssd, other, 1), "unknown waiter prevents later known work from extending drain");
        active.Reset();
        auto exclusive_turn = fair.TryAcquire(exclusive_wait);
        check(static_cast<bool>(exclusive_turn), "unknown waiter acquires after existing work drains");
    }
    {
        TransferDeviceBudget fair;
        auto active = fair.TryAcquire(ssd, ssd, 1);
        auto large_wait = fair.Wait(other, other, 512ull << 20, TransferDeviceBudget::CreateTask());
        check(!fair.TryAcquire(ssd, ssd, 1), "oversized waiter cannot starve behind small unrelated jobs");
        active.Reset();
        auto large_turn = fair.TryAcquire(large_wait);
        check(static_cast<bool>(large_turn), "oversized waiter receives solitary turn");
    }
    {
        TransferDeviceBudget fair(TransferSchedulerConfig{true, 2, 100});
        auto active = fair.TryAcquire(hdd, hdd, 10);
        auto wait = fair.Wait(hdd, hdd, 10, TransferDeviceBudget::CreateTask());
        check(!fair.TryAcquire(ssd, other, 1), "global slot reservation prevents draining waiter starvation");
        wait.Reset();
        check(static_cast<bool>(fair.TryAcquire(ssd, other, 1)), "unregistering waiter restores unrelated slot admission");
    }
    {
        TransferDeviceBudget fair(TransferSchedulerConfig{true, 8, 100});
        auto active = fair.TryAcquire(hdd, hdd, 40);
        auto wait = fair.Wait(other, other, 80, TransferDeviceBudget::CreateTask());
        check(!fair.TryAcquire(ssd, ssd, 1), "large byte waiter reserves global drain against later disjoint work");
        active.Reset();
        check(static_cast<bool>(fair.TryAcquire(wait)), "large byte waiter acquires after drain");
        auto first = fair.Wait(hdd, hdd, 1, TransferDeviceBudget::CreateTask());
        auto second = fair.Wait(ssd, ssd, 1, TransferDeviceBudget::CreateTask());
        first = std::move(second);
        check(fair.Waiting() == 1, "waiter move assignment unregisters previous ticket");
        first.Reset();
        check(fair.Waiting() == 0, "waiter reset removes final registration");
    }
    {
        check(SetEnvironmentVariableW(L"PULSE_FAIR_TRANSFER_BUDGET", L"0") != 0, "set fairness-only rollback");
        TransferDeviceBudget rollback(TransferSchedulerConfig{true, 8, 100});
        check(SetEnvironmentVariableW(L"PULSE_FAIR_TRANSFER_BUDGET", L"1") != 0, "restore fairness for subsequent states");
        auto absent = rollback.Wait(hdd, hdd, 1, TransferDeviceBudget::CreateTask());
        check(!absent && rollback.Waiting() == 0, "fairness off is captured once and creates no tickets");
        auto first = rollback.TryAcquire(hdd, hdd, 60);
        check(first && !rollback.TryAcquire(hdd, hdd, 1), "rollback preserves device capacity");
        check(!rollback.TryAcquire(ssd, other, 41), "rollback preserves shared byte cap");
        check(!rollback.TryAcquire(unknown, other, 1), "rollback preserves unknown exclusion");
        first.Reset();
        check(static_cast<bool>(rollback.TryAcquire(hdd, hdd, 1)), "empty-ticket caller can retry ordinary acquisition");
        TransferDeviceBudget enabled;
        auto present = enabled.Wait(hdd, hdd, 1, TransferDeviceBudget::CreateTask());
        check(present && enabled.Waiting() == 1, "later state captures restored fairness setting");
    }
    // Identity query failure is explicitly conservative without touching real files.
    TransferDeviceIdentityCache cache;
    check(cache.Resolve(L"").resources.empty(), "unresolved identity is conservative");
    auto& shared_a = SharedTransferDeviceBudget();
    auto& shared_b = SharedTransferDeviceBudget();
    check(&shared_a == &shared_b, "shared budget is a single process instance");
    {
        auto first_task = shared_a.TryAcquire(hdd, partition, 1);
        check(first_task && !shared_b.TryAcquire(partition, other, 1), "separate callers share device capacity");
        first_task.Reset();
        auto second_task = shared_b.TryAcquire(partition, other, 1);
        check(static_cast<bool>(second_task), "other caller acquires after release");
    }
    check(shared_a.Active() == 0, "shared lease scope restores capacity");
    TransferDeviceIdentity broad{{L"test:shared-wide"}, 16};
    TransferDeviceBudget::Lease shared_leases[8];
    for (auto& lease : shared_leases) lease = shared_a.TryAcquire(broad, broad, 1);
    check(shared_b.Active() == 8 && !shared_b.TryAcquire(broad, broad, 1), "shared global eight-job cap");
    for (auto& lease : shared_leases) lease.Reset();
    auto first_bytes = shared_a.TryAcquire(broad, broad, 128ull << 20);
    auto second_bytes = shared_b.TryAcquire(broad, broad, 128ull << 20);
    check(first_bytes && second_bytes && !shared_a.TryAcquire(broad, broad, 1), "shared 256MiB byte cap across callers");
    first_bytes.Reset(); second_bytes.Reset();
    check(shared_b.Active() == 0, "shared global capacity restored");
    TransferDeviceBudget changing_capacity;
    TransferDeviceIdentity medium{{L"test:changing-capacity"}, 4};
    TransferDeviceIdentity tiny{{L"test:changing-capacity"}, 8};
    TransferDeviceBudget::Lease changing_leases[8];
    for (size_t i = 0; i < 4; ++i) changing_leases[i] = changing_capacity.TryAcquire(medium, medium, 1);
    check(changing_capacity.Active() == 4 && !changing_capacity.TryAcquire(tiny, tiny, 1), "active medium limit cannot be raised by tiny request");
    for (auto& lease : changing_leases) lease.Reset();
    for (auto& lease : changing_leases) lease = changing_capacity.TryAcquire(tiny, tiny, 1);
    check(changing_capacity.Active() == 8, "inactive resource raises capacity from four to eight");
    for (auto& lease : changing_leases) lease.Reset();
    changing_leases[0] = changing_capacity.TryAcquire(tiny, tiny, 1);
    changing_leases[1] = changing_capacity.TryAcquire(medium, medium, 1);
    changing_leases[2] = changing_capacity.TryAcquire(tiny, tiny, 1);
    changing_leases[3] = changing_capacity.TryAcquire(tiny, tiny, 1);
    check(changing_capacity.Active() == 4 && !changing_capacity.TryAcquire(tiny, tiny, 1), "mixed active leases keep lower admitted limit");
    changing_leases[1].Reset();
    changing_leases[1] = changing_capacity.TryAcquire(tiny, tiny, 1);
    check(changing_capacity.Active() == 4 && !changing_capacity.TryAcquire(tiny, tiny, 1), "lower limit remains until resource becomes idle");
    for (auto& lease : changing_leases) lease.Reset();
    for (auto& lease : changing_leases) lease = changing_capacity.TryAcquire(tiny, tiny, 1);
    check(changing_capacity.Active() == 8, "idle resource restores next caller policy");
    TransferDeviceIdentityCache remote_on(true), remote_off(false);
    const auto share_a = remote_on.Resolve(LR"(\\BudgetServer\ShareA\one.bin)");
    const auto share_b = remote_on.Resolve(LR"(\\?\unc\budgetserver\ShareB\two.bin)");
    check(share_a.resources == share_b.resources && share_a.resources.size() == 1 &&
          share_a.resources.front() == L"unc-server:budgetserver",
          "UNC server case and extended prefix normalize across shares without network access");
    check(share_a.capacity == 1 && share_b.capacity == 1 && share_a.tiny_capacity == 4 && share_b.tiny_capacity == 4,
          "remote ordinary capacity stays one while tiny opt-in is four");
    const auto remote_disabled = remote_off.Resolve(LR"(\\BudgetServer\ShareA\one.bin)");
    check(remote_disabled.resources.empty() && remote_disabled.capacity == 1 && remote_disabled.tiny_capacity == 1,
          "disabled remote policy preserves unknown exclusive identity");
    check(DirectUncTransferIdentity(LR"(Z:\mapped\file.bin)").resources.empty() &&
          DirectUncTransferIdentity(LR"(\\?\C:\local\file.bin)").resources.empty() &&
          DirectUncTransferIdentity(LR"(\\.\PhysicalDrive0)").resources.empty(),
          "UNC parser never guesses mapped drives or device namespaces");
    check(DirectUncTransferIdentity(LR"(\\server)").resources.empty() &&
          DirectUncTransferIdentity(LR"(\\server\\file)").resources.empty() &&
          remote_on.Resolve(LR"(\\server@SSL\DavWWWRoot\file)").resources.empty() &&
          remote_on.Resolve(LR"(\\server\davwwwroot\file)").resources.empty(),
          "incomplete UNC and recognizable WebDAV remain unknown");
    TransferDeviceBudget remote_budget;
    auto remote_first = remote_budget.TryAcquire(share_a, share_a, 1);
    check(remote_first && !remote_budget.TryAcquire(share_b, share_b, 1),
          "ordinary copies to different shares share one server token");
    remote_first.Reset();
    auto tiny_a = share_a, tiny_b = share_b;
    tiny_a.capacity = tiny_a.tiny_capacity;
    tiny_b.capacity = tiny_b.tiny_capacity;
    TransferDeviceBudget::Lease remote_leases[4];
    for (size_t i = 0; i < 4; ++i)
        remote_leases[i] = remote_budget.TryAcquire(i % 2 ? tiny_a : tiny_b, tiny_a, 1);
    check(remote_budget.Active() == 4 && !remote_budget.TryAcquire(tiny_b, tiny_b, 1),
          "tiny copies share four total server permits across shares");
    check(!remote_budget.TryAcquire(remote_disabled, remote_disabled, 1),
          "unknown device remains globally exclusive with opt-in UNC traffic");
    for (auto& lease : remote_leases) lease.Reset();
    auto unknown_remote = remote_budget.TryAcquire(remote_disabled, remote_disabled, 1);
    check(unknown_remote && !remote_budget.TryAcquire(tiny_a, tiny_a, 1),
          "unknown activity still excludes known remote activity");
    unknown_remote.Reset();
    check(share_a.capacity != 4 && share_b.capacity != 4,
          "remote identities cannot enable local SSD large-verify overlap");
    wchar_t current_directory[32768]{};
    const DWORD current_length = GetCurrentDirectoryW(32768, current_directory);
    check(current_length && current_length < 32768, "read current directory for local policy check");
    if (current_length && current_length < 32768) {
        const auto local = remote_off.Resolve(current_directory);
        const unsigned previous_tiny = local.resources.empty() ? 1u : local.capacity == 4 ? 8u : 2u;
        check(local.tiny_capacity == previous_tiny,
              "local policy preserves unknown one known non-SSD two and local SSD eight");
    }
    {
        using Clock = std::chrono::steady_clock;
        TransferDeviceBudget notified;
        auto owner = notified.TryAcquire(hdd, hdd, 1);
        auto ticket = notified.Wait(hdd, hdd, 1, TransferDeviceBudget::CreateTask());
        check(!notified.TryAcquire(ticket), "wake fixture contends for occupied device");
        owner.Reset(); // Release before waiting must not be lost.
        auto started = Clock::now();
        ticket.WaitForChange(std::chrono::seconds(2));
        check(Clock::now() - started < std::chrono::seconds(1), "release before wait is remembered");
        auto acquired = notified.TryAcquire(ticket);
        check(static_cast<bool>(acquired), "notified waiter still acquires through budget checks");
        ticket = notified.Wait(hdd, hdd, 1, TransferDeviceBudget::CreateTask());
        check(!notified.TryAcquire(ticket), "second wake fixture blocked");
        std::thread release([&] { Sleep(30); acquired.Reset(); });
        started = Clock::now();
        ticket.WaitForChange(std::chrono::seconds(2));
        const auto elapsed = Clock::now() - started;
        release.join();
        check(elapsed < std::chrono::seconds(1), "active wait wakes on another thread release");
        auto next = notified.TryAcquire(ticket);
        check(static_cast<bool>(next), "wake does not grant or leak permits");
        next.Reset();
        auto older = notified.Wait(hdd, hdd, 1, TransferDeviceBudget::CreateTask());
        ticket = notified.Wait(hdd, hdd, 1, TransferDeviceBudget::CreateTask());
        check(!notified.TryAcquire(ticket), "new ticket cannot bypass older conflicting task");
        older.Reset();
        started = Clock::now();
        ticket.WaitForChange(std::chrono::seconds(2));
        check(Clock::now() - started < std::chrono::seconds(1) && notified.TryAcquire(ticket),
              "cancelled older ticket wakes eligible successor");
    }
    return failures ? 1 : 0;
}

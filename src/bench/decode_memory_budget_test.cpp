#include "../common/decode_memory_budget.h"
#include "preview_host_client.h"
#include "gif_fixture.h"
#include "../preview_host/decode_budget.h"
#include <atomic>
#include <barrier>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

using pulse::preview::DecodeMemoryBudget;
int main() {
    int failures = 0;
    auto check = [&](bool value, const char* label) { std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label); failures += !value; };
    check(pulse::preview::DecodeEstimate(UINT64_MAX, 20, 1024) == UINT64_MAX,
        "source pixel estimate saturates instead of wrapping at extreme dimensions");
    DecodeMemoryBudget budget(128);
    std::atomic<unsigned> completed = 0;
    std::atomic<uint64_t> peak = 0;
    std::vector<std::thread> lanes;
    for (unsigned i = 0; i < 4; ++i) lanes.emplace_back([&] {
        DecodeMemoryBudget::Lease lease;
        while (!budget.Reserve(lease, 64, false)) Sleep(1);
        auto used = budget.Used(); auto prior = peak.load();
        while (prior < used && !peak.compare_exchange_weak(prior, used)) {}
        Sleep(5); ++completed;
    });
    for (auto& lane : lanes) lane.join();
    check(completed == 4 && peak <= 128 && budget.Used() == 0, "four lanes progress under two-lane capacity without leaking reservations");
    std::barrier ready(4);
    std::atomic<unsigned> rejected = 0;
    lanes.clear();
    for (unsigned i = 0; i < 4; ++i) lanes.emplace_back([&] {
        DecodeMemoryBudget::Lease lease;
        budget.Reserve(lease, 32, false); ready.arrive_and_wait();
        if (!budget.Reserve(lease, 96, false)) ++rejected;
    });
    for (auto& lane : lanes) lane.join();
    check(rejected > 0 && budget.Used() == 0, "competing upgrades reject atomically instead of waiting while holding leases");
    { DecodeMemoryBudget::Lease a, b;
      check(budget.Reserve(a, 32, true) && !budget.Reserve(b, 32, true), "unknown codec work is restricted to one active lease");
      a.Reset(); check(budget.Reserve(b, 32, true), "unknown codec recovers when prior owner releases"); }
    { DecodeMemoryBudget::Lease a;
      check(!budget.Reserve(a, 129, false) && budget.Used() == 0 && budget.Reserve(a, 128, false), "over-budget admission leaves capacity intact for recovery"); }
    { DecodeMemoryBudget::Lease owner; budget.Reserve(owner, 128, false);
      std::atomic<bool> cancel = false, stopped = false;
      std::thread waiter([&] { DecodeMemoryBudget::Lease lease;
        while (!cancel && !budget.Reserve(lease, 64, false)) Sleep(1);
        stopped = cancel.load(); });
      Sleep(5); cancel = true; waiter.join();
      check(stopped && budget.Used() == 128, "cancelled waiting lane owns no reservation"); }
    check(budget.Used() == 0, "scope exit releases capacity after cancellation");

    const auto folder = std::filesystem::current_path() / L"bench_data" /
        (L"decode-budget-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!std::filesystem::create_directory(folder)) return 2;
    const auto file = folder / L"first.gif";
    pulse_test::gif_fixture::Bytes gif{'G','I','F','8','9','a'};
    pulse_test::gif_fixture::Word(gif, 4); pulse_test::gif_fixture::Word(gif, 2);
    gif.insert(gif.end(), {0x81,0,0,255,0,0,0,255,0,0,0,255,255,255,255});
    pulse_test::gif_fixture::Frame(gif, 0, 4, 2, 0, 1); gif.push_back(0x3b);
    { std::ofstream out(file, std::ios::binary); out.write(reinterpret_cast<const char*>(gif.data()), static_cast<std::streamsize>(gif.size())); }
    pulse_test::Result legacy, denied, resumed;
    { pulse_test::Host host; check(host.Start() && host.Request(file, legacy), "legacy client remains compatible with new host"); }
    auto& shared = pulse::preview::SharedDecodeMemoryBudget();
    { pulse_test::Host host; host.SetBudgeted(true);
      check(host.Start(), "budget protocol host starts");
      { DecodeMemoryBudget::Lease blocker; shared.Reserve(blocker, shared.Limit(), false);
        check(host.Request(file, denied) && denied.error == L"decode-memory-budget" && denied.pixels.empty(),
            "host denies before decoding and returns recoverable resource status"); }
      check(host.Request(file, resumed) && resumed.pixels == legacy.pixels && !resumed.pixels.empty() && shared.Used() == 0,
          "same host recovers after capacity returns and retains pixels through mapping ACK"); }
    { pulse_test::Host host; host.SetBudgeted(true);
      host.SetBudgetGrantHook([](HANDLE process) { TerminateProcess(process, 91); WaitForSingleObject(process, 2000); });
      pulse_test::Result crash;
      check(host.Start() && !host.Request(file, crash) && shared.Used() == 0,
          "host death after grant releases parent lease on broken pipe"); }
    std::filesystem::remove_all(folder);
    std::printf("Failures: %d\n", failures);
    return failures ? 1 : 0;
}

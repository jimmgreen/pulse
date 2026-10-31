#include "../ops/ops_manager.h"
#include "../ops/native_delete_stream.h"
#include "../common/localization.h"
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <objbase.h>
using namespace pulse;
namespace fs = std::filesystem;
namespace {
fs::path held_fixture;
HANDLE held_handle = INVALID_HANDLE_VALUE;
void LockAfterFirstMutation() {
    ops::native_delete::after_mutation = nullptr;
    std::ofstream(held_fixture) << "fixture";
    held_handle = CreateFileW(held_fixture.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
}
}
int wmain(int argc, wchar_t**) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const auto root = fs::current_path() / L"bench_data" / (L"delete_delivery_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
    if (fs::exists(root) || !fs::create_directories(root)) return 2;
    ops::OpsManager manager;
    manager.SetJournalPath((root / L"journal.json").wstring());
    manager.Start([] {}, true);
    int failures = 0;
    auto check = [&](bool ok, const char* name) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); failures += !ok; };
    auto run = [&](std::vector<std::wstring> sources, bool cancel) {
        ops::OpRequest request; request.type = ops::OpType::RealDelete; request.sources = std::move(sources);
        auto task = manager.Submit(std::move(request));
        if (!task) { check(false, "request accepted"); return task; }
        if (cancel) {
            const auto wait = GetTickCount64();
            while (manager.Status().task_id != task && GetTickCount64() - wait < 5000) Sleep(1);
            manager.CancelCurrent();
        }
        const auto begin = GetTickCount64();
        while (!manager.FinalStatus(task) && GetTickCount64() - begin < 30000) Sleep(1);
        check(manager.FinalStatus(task).has_value(), "request terminates");
        return task;
    };
    auto file = root / L"file"; std::ofstream(file) << "fixture";
    run({file.wstring()}, false); check(!fs::exists(file), "ordinary file success");
    auto tree = root / L"tree"; fs::create_directories(tree / L"child");
    std::ofstream(tree / L"child" / L"file") << "fixture";
    const auto overlap = run({(tree / L"child").wstring(), tree.wstring(), tree.wstring()}, false);
    auto overlap_status = manager.FinalStatus(overlap);
    check(overlap_status && overlap_status->phase == ops::OpPhase::Completed && overlap_status->failed_items == 0,
          "descendant ancestor duplicate finishes without failures");
    check(!fs::exists(tree), "overlapping and duplicate roots");
    fs::create_directories(tree / L"child");
    std::ofstream(tree / L"child" / L"file") << "fixture";
    const auto ancestor_first = run({tree.wstring(), (tree / L"child").wstring()}, false);
    const auto ancestor_status = manager.FinalStatus(ancestor_first);
    check(ancestor_status && ancestor_status->phase == ops::OpPhase::Completed && ancestor_status->failed_items == 0,
          "ancestor descendant finishes without failures");
    fs::create_directory(tree);
    for (int i = 0; i < 100; ++i) std::ofstream(tree / (L"a" + std::to_wstring(i))) << "fixture";
    auto held = tree / L"zzheld";
    held_fixture = held;
    ops::native_delete::after_mutation = LockAfterFirstMutation;
    const auto failed = run({tree.wstring()}, false);
    bool refreshed = false;
    for (const auto& completion : manager.DrainCompletions()) refreshed |= completion.task_id == failed && completion.refresh_only;
    check(refreshed && fs::exists(held), "partial deletion publishes refresh only");
    check(manager.RetryFailedItems(failed).empty() && !manager.RetryLockedOperation(failed, false), "partial root is not registered for replay");
    if (held_handle != INVALID_HANDLE_VALUE) CloseHandle(held_handle);
    run({tree.wstring()}, false);
    fs::create_directory(tree);
    for (int i = 0; i < 10000; ++i) std::ofstream(tree / (L"f" + std::to_wstring(i)));
    run({tree.wstring()}, true);
    check(fs::exists(tree), "cancel preserves unfinished root");
    if (argc > 1) {
        run({tree.wstring()}, false); fs::create_directory(tree);
        for (int i = 0; i < 10000; ++i) std::ofstream(tree / (L"f" + std::to_wstring(i)));
        ops::OpRequest req; req.type = ops::OpType::RealDelete; req.sources = {tree.wstring()};
        const auto start = GetTickCount64(); auto id = manager.Submit(req); ULONGLONG first = 0;
        check(id != 0, "benchmark request accepted");
        while (id && !manager.FinalStatus(id) && GetTickCount64() - start < 120000) {
            if (!first && !fs::exists(tree / L"f0")) first = GetTickCount64() - start;
            Sleep(1);
        }
        printf("wide10000 first_observed_mutation_ms=%llu total_ms=%llu\n", first, GetTickCount64() - start);
    }
    manager.Stop();
    // Cleanup is restricted to the unique directory successfully created above.
    fs::remove_all(root);
    CoUninitialize();
    return failures ? 1 : 0;
}

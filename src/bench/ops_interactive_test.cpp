#include "../ops/ops_manager.h"
#include "../common/localization.h"
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <chrono>
#include <thread>
#include <objbase.h>
#include <winioctl.h>
#include <algorithm>
using namespace pulse;
namespace {
int failures = 0;
void Check(bool value, const char* name) { std::printf("[%s] %s\n", value ? "PASS" : "FAIL", name); if (!value) ++failures; }
void Pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
}
void ResolveUnattendedAuthorization(ops::OpsManager& manager) {
    static std::set<std::pair<const ops::OpsManager*, uint64_t>> handled;
    const auto status = manager.Status();
    if (status.authorization == ops::AuthorizationState::ActionRequired && status.can_skip_authorization &&
        handled.emplace(&manager, status.task_id).second) {
        std::printf("[FAIL] unattended authorization task=%llu error=%ls\n", status.task_id, status.last_error.c_str());
        ++failures;
        manager.ResolveAuthorization(status.task_id, false);
    }
}
void WaitIdle(ops::OpsManager& manager) {
    const auto deadline = GetTickCount64() + 60000;
    while (manager.HasPendingOperations() && GetTickCount64() < deadline) {
        Pump(); ResolveUnattendedAuthorization(manager); std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (manager.HasPendingOperations()) {
        const auto status = manager.Status();
        std::printf("[FAIL] idle deadline task=%llu phase=%d error=%ls\n", status.task_id, static_cast<int>(status.phase), status.last_error.c_str());
        ExitProcess(1);
    }
}
std::wstring Extended(std::filesystem::path path) {
    std::wstring prefix(2, wchar_t(92));
    prefix.push_back(L'?'); prefix.push_back(wchar_t(92));
    return prefix + path.make_preferred().wstring();
}
bool Wait(ops::OpsManager& manager, uint64_t task, unsigned timeout = 30000) {
    const auto start = GetTickCount64();
    while (GetTickCount64() - start < timeout) {
        if (manager.FinalStatus(task)) return true;
        Pump(); ResolveUnattendedAuthorization(manager); std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}
void Write(const std::filesystem::path& path) { std::ofstream out(path, std::ios::binary); out << "interactive regression content"; }
}
int main(int argc, char** argv) {
    const bool baseline = argc > 1 && std::string(argv[1]) == "--baseline-probe";
    const bool core_only = argc > 1 && std::string(argv[1]) == "--core-only";
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    const auto root = std::filesystem::absolute(std::filesystem::path("bench_data/small-files-responsive-20261119") /
        ("ops-fixture-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64())));
    std::filesystem::create_directories(root / "heavy/source");
    std::filesystem::create_directories(root / "heavy/destination");
    std::filesystem::create_directories(root / "light/source");
    std::filesystem::create_directories(root / "light/destination");
    for (int i = 0; i < 12000; ++i) Write(root / "heavy/source" / (std::to_string(i) + ".txt"));
    Write(root / "light/source/item.txt");
    Write(root / "light/source/delete.txt");
    Write(root / "light/source/rename.txt");
    ops::OpsManager manager;
    manager.Start([] {}, false);
    ops::OpRequest heavy;
    heavy.type = ops::OpType::Copy;
    heavy.sources = {(root / "heavy/source").make_preferred().wstring()};
    heavy.dest_dir = (root / "heavy/destination").make_preferred().wstring();
    const auto main_task = manager.Submit(heavy);
    for (int i = 0; i < 1000; ++i) {
        auto status = manager.QueryTask(main_task);
        if (status && status->phase != ops::OpPhase::Queued) break;
        Pump(); ResolveUnattendedAuthorization(manager); std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    Check(!manager.TryPrepareForUpdate(), "update rejected during primary task");
    ops::OpRequest create;
    create.type = ops::OpType::CreateFolder;
    create.collision_policy = ops::CollisionPolicy::KeepBoth;
    create.sources = {Extended(root / "heavy/destination/new-folder")};
    auto start = GetTickCount64();
    const auto create_task = manager.Submit(create);
    Check(manager.RetainTask(create_task), "interactive ID can be retained");
    Check(Wait(manager, create_task), "independent create completes");
    const bool primary_during_create = !manager.FinalStatus(main_task).has_value();
    std::printf("METRIC create_ms=%llu primary_still_running=%d\n", GetTickCount64() - start, primary_during_create);
    if (baseline) {
        Check(!primary_during_create, "baseline serial queue delays create until primary finishes");
        Check(std::filesystem::is_directory(root / "heavy/destination/new-folder"), "baseline created folder exists");
        manager.Stop();
        std::printf("RESULT failures=%d fixture=%ls\n", failures, root.c_str());
        CoUninitialize();
        return failures ? 1 : 0;
    }
    Check(primary_during_create, "independent create completes before primary copy");
    Check(std::filesystem::is_directory(root / "heavy/destination/new-folder"), "created folder exists on disk");
    const auto duplicate_create = manager.Submit(create);
    Check(Wait(manager, duplicate_create) && std::filesystem::is_directory(root / "heavy/destination/new-folder (2)"),
        "production KeepBoth create preserves numbered sibling semantics");
    ops::OpRequest rename;
    rename.type = ops::OpType::Rename;
    rename.sources = {(root / "light/source/rename.txt").make_preferred().wstring()};
    rename.new_name = L"renamed.txt";
    auto rename_task = manager.Submit(rename);
    Check(Wait(manager, rename_task) && std::filesystem::exists(root / "light/source/renamed.txt"), "independent rename executes production path");
    const auto sparse_source = (root / "light/source/large-rename.bin").make_preferred().wstring();
    HANDLE sparse = CreateFileW(sparse_source.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool sparse_ok = sparse != INVALID_HANDLE_VALUE;
    if (sparse_ok) {
        DWORD returned = 0;
        sparse_ok = DeviceIoControl(sparse, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr) != FALSE;
        LARGE_INTEGER length{}; length.QuadPart = 128 * 1024 * 1024;
        sparse_ok = sparse_ok && SetFilePointerEx(sparse, length, nullptr, FILE_BEGIN) && SetEndOfFile(sparse);
        CloseHandle(sparse);
    }
    Check(sparse_ok, "128 MiB sparse rename fixture created without large data write");
    rename.sources = {sparse_source};
    rename.new_name = L"large-renamed.bin";
    auto large_rename_task = manager.Submit(rename);
    Check(Wait(manager, large_rename_task) && std::filesystem::exists(root / "light/source/large-renamed.bin") && !manager.FinalStatus(main_task),
        "large single-file rename is not constrained by small-copy byte limit");
    ops::OpRequest copy;
    copy.type = ops::OpType::Copy;
    copy.sources = {(root / "light/source/item.txt").make_preferred().wstring()};
    copy.dest_dir = (root / "light/destination").make_preferred().wstring();
    auto copy_task = manager.Submit(copy);
    Check(Wait(manager, copy_task) && std::filesystem::exists(root / "light/destination/item.txt"), "independent small copy executes production path");
    ops::OpRequest remove;
    remove.type = ops::OpType::RealDelete;
    remove.sources = {(root / "light/source/delete.txt").make_preferred().wstring()};
    auto remove_task = manager.Submit(remove);
    Check(Wait(manager, remove_task) && !std::filesystem::exists(root / "light/source/delete.txt"), "independent single-file delete executes production path");
    if (auto status = manager.FinalStatus(remove_task)) std::printf("DELETE status=%d error=%ls source=%ls\n", static_cast<int>(status->phase), status->last_error.c_str(), remove.sources.front().c_str());
    Check(!manager.FinalStatus(main_task), "primary still running after four independent operations");
    ops::OpRequest overlap;
    overlap.type = ops::OpType::CreateTextFile;
    overlap.sources = {Extended(root / "heavy/destination/source/after.txt")};
    auto overlap_task = manager.Submit(overlap);
    Write(root / "light/source/ordered.txt");
    auto ordered_copy = copy;
    ordered_copy.sources = {(root / "light/source/ordered.txt").make_preferred().wstring()};
    ordered_copy.dest_dir = (root / "heavy/destination/source").make_preferred().wstring();
    const auto ordered_first = manager.Submit(ordered_copy);
    auto ordered_delete = remove;
    ordered_delete.sources = ordered_copy.sources;
    const auto ordered_second = manager.Submit(ordered_delete);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    Check(!manager.FinalStatus(ordered_second), "later independent-looking delete cannot bypass earlier queued source dependency");
    Check(!manager.FinalStatus(overlap_task), "overlapping destination remains queued");
    Check(manager.CancelTask(main_task), "cancel exact primary while interactive completions exist");
    Check(Wait(manager, main_task) && Wait(manager, overlap_task), "cancelled primary releases queued overlap");
    Check(std::filesystem::exists(root / "heavy/destination/source/after.txt"), "ordered overlap executes after primary terminal");
    Check(Wait(manager, ordered_first) && Wait(manager, ordered_second) &&
        std::filesystem::exists(root / "heavy/destination/source/ordered.txt") && !std::filesystem::exists(root / "light/source/ordered.txt"),
        "queued copy precedes dependent delete and preserves its output");
    const auto history = manager.UndoHistory();
    Check(std::any_of(history.entries.begin(), history.entries.end(), [&](const auto& entry) { return entry.entry.task_id == create_task; }),
        "interactive undo is in shared history with exact task ID");
    auto completions = manager.DrainCompletions();
    for (auto task : {create_task, rename_task, copy_task, remove_task})
        Check(std::count_if(completions.begin(), completions.end(), [&](const auto& item) { return item.task_id == task; }) == 1,
            "interactive completion delivered once with original task ID");
    manager.ReleaseTask(create_task);
    ops::OpRequest large_delete;
    large_delete.type = ops::OpType::RealDelete;
    large_delete.sources = {(root / "heavy/source").make_preferred().wstring()};
    const auto delete_task = manager.Submit(large_delete);
    for (int i = 0; i < 1000; ++i) {
        auto status = manager.QueryTask(delete_task);
        if (status && status->phase != ops::OpPhase::Queued) break;
        Pump(); ResolveUnattendedAuthorization(manager); std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    create.sources = {Extended(root / "light/during-delete")};
    start = GetTickCount64();
    const auto during_delete = manager.Submit(create);
    Check(Wait(manager, during_delete), "independent create completes during real large delete");
    const bool deleting = !manager.FinalStatus(delete_task).has_value();
    std::printf("METRIC delete_create_ms=%llu delete_still_running=%d\n", GetTickCount64() - start, deleting);
    Check(deleting, "large deletion does not serialize independent create");
    Check(Wait(manager, delete_task) && !std::filesystem::exists(root / "heavy/source"), "real large deletion removes all source files");
    if (auto status = manager.FinalStatus(delete_task)) std::printf("LARGE_DELETE status=%d error=%ls source=%ls\n", static_cast<int>(status->phase), status->last_error.c_str(), large_delete.sources.front().c_str());
    const auto summary = manager.UndoHistorySummary(false);
    Check(summary.size() == 1 && summary.front().task_id == during_delete && manager.CanUndoTask(during_delete),
        "reverse completion order keeps displayed and executable undo task consistent");
    if (!core_only) {
    const auto preserved = root / "heavy/destination/source/after.txt";
    Check(manager.UndoTask(during_delete), "undo accepts the displayed interactive create");
    WaitIdle(manager);
    Check(!std::filesystem::exists(root / "light/during-delete") && std::filesystem::exists(preserved),
        "actual undo removes only interactive folder and preserves primary output");
    std::filesystem::create_directories(root / "recycle/source");
    for (int i = 0; i < 2048; ++i) Write(root / "recycle/source" / (std::to_string(i) + ".txt"));
    ops::OpRequest recycle;
    recycle.type = ops::OpType::RecycleDelete;
    recycle.sources = {(root / "recycle/source").make_preferred().wstring()};
    const auto recycle_task = manager.Submit(recycle);
    for (int i = 0; i < 1000; ++i) {
        auto status = manager.QueryTask(recycle_task);
        if (status && status->phase != ops::OpPhase::Queued) break;
        Pump(); ResolveUnattendedAuthorization(manager); std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    create.sources = {Extended(root / "light/during-recycle")};
    start = GetTickCount64();
    const auto during_recycle = manager.Submit(create);
    Check(Wait(manager, during_recycle), "create completes during ordinary RecycleDelete");
    const bool recycling = !manager.FinalStatus(recycle_task).has_value();
    std::printf("METRIC recycle_create_ms=%llu recycle_still_running=%d\n", GetTickCount64() - start, recycling);
    Check(recycling, "ordinary recycle deletion does not serialize unrelated create");
    const bool recycled = Wait(manager, recycle_task) && !std::filesystem::exists(root / "recycle/source");
    Check(recycled, "ordinary deletion recycles fixture directory");
    const bool undo_recycled = manager.UndoTo(recycle_task);
    Check(undo_recycled, "shared undo restores only test recycle operation and its later create");
    WaitIdle(manager);
    size_t restored = 0;
    if (std::filesystem::is_directory(root / "recycle/source"))
        for (const auto& item : std::filesystem::directory_iterator(root / "recycle/source")) if (item.is_regular_file()) ++restored;
    Check(recycled && undo_recycled && restored == 2048, "all recycled fixture files restored without touching other recycle entries");
    recycle.type = ops::OpType::RealDelete;
    const auto cleanup_task = manager.Submit(recycle);
    Check(Wait(manager, cleanup_task) && !std::filesystem::exists(root / "recycle/source"), "recycle fixture permanently cleaned by exact path");
    }
    Check(manager.TryPrepareForUpdate(), "update accepted after both lanes idle");
    Check(manager.Submit(create) == 0, "update barrier rejects new submissions");
    manager.CancelUpdatePreparation();
    std::filesystem::create_directories(root / "stop/source");
    std::filesystem::create_directories(root / "stop/destination");
    for (int i = 0; i < 2048; ++i) Write(root / "stop/source" / (std::to_string(i) + ".txt"));
    heavy.sources = {(root / "stop/source").make_preferred().wstring()};
    heavy.dest_dir = (root / "stop/destination").make_preferred().wstring();
    const auto stopped_primary = manager.Submit(heavy);
    create.sources = {Extended(root / "light/stopping")};
    const auto stopped_light = manager.Submit(create);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    manager.Stop();
    Check(!manager.HasPendingOperations(), "stop leaves no pending task in either executor");
    const auto stopped = manager.DrainFinishedTasks();
    for (auto task : {stopped_primary, stopped_light})
        Check(std::count_if(stopped.begin(), stopped.end(), [&](const auto& item) { return item.task_id == task; }) == 1,
            "stop reports each accepted primary and light task exactly once");
    std::printf("RESULT failures=%d fixture=%ls\n", failures, root.c_str());
    CoUninitialize();
    return failures ? 1 : 0;
}

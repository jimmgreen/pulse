#include "../ops/ops_manager.h"
#include "../ops/interactive_target_walk.h"
#include "../common/localization.h"
#include "../common/runtime_log.h"
#include <objbase.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <chrono>
#include <thread>
#include <algorithm>
#include <winioctl.h>

namespace pulse::ops {
struct OpsManagerAuditAccess {
    static void SetTransfer(OpsManager& manager, uint64_t id, bool paused = false) {
        OpsManager::QueueItem item;
        item.seq = id;
        item.req.type = OpType::Copy;
        item.transfer_paused = paused;
        manager.active_item_ = item;
        manager.status_.task_id = id;
        manager.status_.active = true;
        manager.status_.pause_requested = paused;
        manager.transfer_active_.store(true);
        manager.transfer_pause_.store(paused);
    }
    static std::shared_ptr<OpsManager> Child(OpsManager& manager) {
        std::lock_guard lock(manager.mutex_);
        for (const auto& lane : manager.lanes_) if (lane.item) return lane.executor;
        return nullptr;
    }
    static inline std::shared_ptr<OpsManager> saved_lane;
    static void CancelTransfer(OpsManager& manager) { manager.active_item_->transfer_cancelled = true; }
    static bool Paused(OpsManager& manager) { return manager.transfer_pause_.load(); }
    static void ClearTransfer(OpsManager& manager) {
        manager.active_item_.reset();
        manager.status_ = {};
        manager.transfer_active_.store(false);
        manager.transfer_pause_.store(false);
    }
    static bool RetryMergeBounded(const std::filesystem::path& root) {
        OpsManager manager;
        manager.running_ = true;
        for (auto& lane : manager.lanes_) lane.executor = std::make_shared<OpsManager>();
        OpsManager::QueueItem primary, candidate;
        primary.seq = 1;
        primary.req.type = OpType::RealDelete;
        primary.req.sources = {(root / "primary").wstring()};
        manager.active_item_ = primary;
        candidate.seq = 20;
        candidate.req.type = OpType::CreateFolder;
        candidate.req.sources = {(root / "candidate").wstring()};
        manager.queue_.push_back(candidate);
        for (uint64_t id = 2; id < 10; ++id) {
            OpsManager::FailedItemsRetry retry;
            retry.task_id = id;
            manager.failed_retries_.push_back(std::move(retry));
        }
        std::thread scheduler([&] { manager.InteractiveThread(); });
        bool dispatched = false;
        const auto deadline = GetTickCount64() + 10000;
        while (GetTickCount64() < deadline) {
            {
                std::lock_guard lock(manager.mutex_);
                if (manager.lanes_[0].item) { dispatched = true; break; }
            }
            Sleep(1);
        }
        if (dispatched) {
            auto& child = *manager.lanes_[0].executor;
            {
                std::lock_guard lock(child.mutex_);
                child.queue_.clear();
                for (uint64_t id = 10; id < 12; ++id) {
                    OpsManager::FailedItemsRetry retry;
                    retry.task_id = id;
                    child.failed_retries_.push_back(std::move(retry));
                }
            }
            child.cv_.notify_all();
        }
        bool merged = false, bounded = false;
        while (dispatched && GetTickCount64() < deadline) {
            {
                std::lock_guard lock(manager.mutex_);
                if (!manager.lanes_[0].item) {
                    merged = true;
                    bounded = manager.failed_retries_.size() == 8 &&
                        manager.failed_retries_.front().task_id == 4 && manager.failed_retries_.back().task_id == 11;
                    break;
                }
            }
            Sleep(1);
        }
        manager.running_ = false;
        manager.cv_.notify_all();
        scheduler.join();
        manager.active_item_.reset();
        return dispatched && merged && bounded;
    }
    static void DisableLane(OpsManager& manager) { manager.interactive_child_ = true; }
    static uint64_t InjectInteraction(OpsManager& manager) {
        auto child = std::make_shared<OpsManager>();
        std::lock_guard lock(manager.mutex_);
        const auto id = manager.next_seq_++;
        OpsManager::QueueItem item;
        item.seq = id;
        item.req.type = OpType::Copy;
        child->active_item_ = item;
        child->status_.task_id = id;
        child->status_.type = OpType::Copy;
        child->status_.active = true;
        child->status_.phase = OpPhase::WaitingForConflict;
        child->pending_conflict_ = ConflictItemInfo{};
        child->pending_conflict_->task_id = id;
        child->pending_conflict_->token = (uint64_t{1} << 63) | 1;
        manager.lanes_[0].item = item;
        saved_lane = std::exchange(manager.lanes_[0].executor, std::move(child));
        {
            std::lock_guard control_lock(manager.transfer_control_mutex_);
            manager.pending_conflict_ = ConflictItemInfo{};
            manager.pending_conflict_->task_id = manager.active_item_ ? manager.active_item_->seq : 0;
            manager.pending_conflict_->token = 4;
        }
        return id;
    }
    static bool ConflictResolved(OpsManager& manager) {
        auto child = manager.lanes_[0].executor;
        std::lock_guard lock(child->transfer_control_mutex_);
        return child->resolved_conflict_token_ == ((uint64_t{1} << 63) | 1) && child->resolved_conflict_choice_ == ConflictChoice::Skip;
    }
    static void SetAuthorization(OpsManager& manager, uint64_t id) {
        auto child = manager.InteractiveExecutor(id);
        std::scoped_lock lock(child->mutex_, child->transfer_control_mutex_);
        child->pending_conflict_.reset();
        child->status_.phase = OpPhase::Running;
        child->status_.authorization = AuthorizationState::ActionRequired;
        child->authorization_task_ = id;
    }
    static bool AuthorizationResolved(OpsManager& manager, uint64_t id) {
        auto child = manager.InteractiveExecutor(id);
        std::lock_guard lock(child->transfer_control_mutex_);
        return child->authorization_choice_ == AuthorizationChoice::Retry;
    }
    static void RemoveInteraction(OpsManager& manager) {
        std::scoped_lock lock(manager.mutex_, manager.transfer_control_mutex_);
        manager.lanes_[0].item.reset();
        manager.pending_conflict_.reset();
        manager.lanes_[0].executor = std::exchange(saved_lane, nullptr);
        manager.cv_.notify_all();
    }
};
}
using namespace pulse;
namespace {
int failures = 0;
void Check(bool ok, const char* name) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); failures += !ok; }
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
bool MakeJunction(const std::filesystem::path& link, const std::filesystem::path& target) {
    std::filesystem::create_directories(link);
    const auto printable = std::filesystem::path(target).make_preferred().wstring();
    const std::wstring substitute = std::wstring(1, wchar_t(92)) + L"??" + wchar_t(92) + printable;
    struct Header { DWORD tag; WORD length, reserved, substitute_offset, substitute_length, print_offset, print_length; };
    std::vector<unsigned char> buffer(sizeof(Header) + (substitute.size() + printable.size() + 2) * sizeof(wchar_t));
    auto* header = reinterpret_cast<Header*>(buffer.data());
    header->tag = IO_REPARSE_TAG_MOUNT_POINT;
    header->length = static_cast<WORD>(buffer.size() - 8);
    header->substitute_length = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    header->print_offset = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t));
    header->print_length = static_cast<WORD>(printable.size() * sizeof(wchar_t));
    memcpy(buffer.data() + sizeof(Header), substitute.c_str(), (substitute.size() + 1) * sizeof(wchar_t));
    memcpy(buffer.data() + sizeof(Header) + header->print_offset, printable.c_str(), (printable.size() + 1) * sizeof(wchar_t));
    HANDLE handle = CreateFileW(link.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    DWORD returned = 0;
    const bool result = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.data(), static_cast<DWORD>(buffer.size()),
        nullptr, 0, &returned, nullptr) != FALSE;
    CloseHandle(handle);
    return result;
}
bool Wait(ops::OpsManager& manager, uint64_t task) {
    const auto deadline = GetTickCount64() + 30000;
    while (GetTickCount64() < deadline) {
        if (manager.FinalStatus(task)) return true;
        Pump(); ResolveUnattendedAuthorization(manager); std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}
int RunOversizedFreshTargetTest(const std::wstring& custom_dest_root) {
    namespace fs = std::filesystem;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");

    const std::string suffix = "oversized-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64());
    const fs::path root = fs::absolute(fs::path("bench_data/copy-toolbar-20261122/ops") / suffix);
    fs::create_directories(root);

    fs::path dest_cleanup;
    fs::path dest_base;
    if (custom_dest_root.empty()) {
        dest_base = root / "dest";
        fs::create_directories(dest_base);
    } else {
        const fs::path requested = fs::absolute(fs::path(custom_dest_root));
        const bool is_drive_root = requested == requested.root_path();
        if (!is_drive_root && !fs::exists(requested)) {
            fs::create_directories(requested);
            dest_base = requested;
            dest_cleanup = requested;
        } else {
            dest_base = requested / suffix;
            fs::create_directories(dest_base);
            dest_cleanup = dest_base;
        }
    }

    struct CleanupGuard {
        fs::path root_dir;
        fs::path custom_dir;
        std::vector<fs::path> junctions;
        ~CleanupGuard() {
            diagnostics::runtime::Shutdown();
            for (const auto& link : junctions) {
                RemoveDirectoryW(Extended(link).c_str());
                RemoveDirectoryW(link.c_str());
            }
            std::error_code ec;
            if (!custom_dir.empty()) fs::remove_all(custom_dir, ec);
            if (!root_dir.empty()) fs::remove_all(root_dir, ec);
        }
    } cleanup{root, dest_cleanup, {}};

    const bool log_ok = diagnostics::runtime::Initialize(root.wstring(), "ops_interactive_control_test");
    Check(log_ok, "isolated runtime diagnostics initialized");

    const fs::path outside = root / "outside";
    const fs::path source_tree = root / "src" / "tree";
    fs::create_directories(outside);
    fs::create_directories(source_tree);

    constexpr size_t kDirs = 67;
    constexpr size_t kFilesPerDir = 1000;
    constexpr size_t kWorkers = 8;
    std::atomic<bool> fixture_ok{true};
    {
        std::vector<std::thread> builders;
        builders.reserve(kWorkers);
        const char payload[] = "pulse-oversized-tree-fixture\n";
        for (size_t w = 0; w < kWorkers; ++w) {
            builders.emplace_back([&, w] {
                for (size_t d = w; d < kDirs; d += kWorkers) {
                    wchar_t dir_name[16]{};
                    std::swprintf(dir_name, 16, L"d%03zu", d);
                    const fs::path dir = source_tree / dir_name;
                    if (!CreateDirectoryW(Extended(dir).c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
                        fixture_ok = false;
                        return;
                    }
                    for (size_t f = 0; f < kFilesPerDir; ++f) {
                        wchar_t file_name[16]{};
                        std::swprintf(file_name, 16, L"f%03zu.txt", f);
                        HANDLE h = CreateFileW(Extended(dir / file_name).c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
                        if (h == INVALID_HANDLE_VALUE) {
                            fixture_ok = false;
                            return;
                        }
                        DWORD written = 0;
                        const BOOL ok = WriteFile(h, payload, static_cast<DWORD>(sizeof(payload) - 1), &written, nullptr);
                        CloseHandle(h);
                        if (!ok || written != sizeof(payload) - 1) {
                            fixture_ok = false;
                            return;
                        }
                    }
                }
            });
        }
        for (auto& t : builders) t.join();
    }
    Check(fixture_ok.load(), "created 67,000 small files across 67 directories");

    const fs::path source_junction = source_tree / "leaf-junction";
    const bool junction_ok = MakeJunction(source_junction, outside);
    if (junction_ok) cleanup.junctions.push_back(source_junction);
    Check(junction_ok, "created leaf junction inside oversized source tree");

    const fs::path z_tail = source_tree / "z_tail";
    fs::create_directories(z_tail);
    bool tail_ok = true;
    {
        std::vector<char> chunk(4ull * 1024 * 1024, 'P');
        for (const wchar_t* name : {L"tail1.bin", L"tail2.bin"}) {
            HANDLE h = CreateFileW(Extended(z_tail / name).c_str(), GENERIC_WRITE, 0, nullptr,
                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) { tail_ok = false; break; }
            for (int i = 0; i < 64; ++i) {
                DWORD written = 0;
                if (!WriteFile(h, chunk.data(), static_cast<DWORD>(chunk.size()), &written, nullptr) ||
                    written != chunk.size()) {
                    tail_ok = false;
                    break;
                }
            }
            CloseHandle(h);
            if (!tail_ok) break;
        }
    }
    Check(tail_ok, "created 512 MiB tail files so primary copy stays active after 67k+ entries");

    ops::OpsManager manager;
    manager.Start([] {}, false);

    const auto run_subcase = [&](const char* label, const fs::path& dest_parent, bool preexisting_empty) {
        fs::create_directories(dest_parent);
        if (preexisting_empty) fs::create_directories(dest_parent / "tree");
        manager.DrainCompletions();
        manager.DrainFinishedTasks();

        const fs::path dest_tree = dest_parent / "tree";
        const fs::path dest_junction = dest_tree / "leaf-junction";
        cleanup.junctions.push_back(dest_junction);

        ops::OpRequest copy;
        copy.type = ops::OpType::Copy;
        copy.sources = {fs::path(source_tree).make_preferred().wstring()};
        copy.dest_dir = fs::path(dest_parent).make_preferred().wstring();
        const auto copy_started = GetTickCount64();
        const auto primary = manager.Submit(std::move(copy));
        manager.RetainTask(primary);

        bool ready_mid_copy = false;
        const auto ready_deadline = GetTickCount64() + 300000;
        while (GetTickCount64() < ready_deadline) {
            const auto st = manager.QueryTask(primary);
            const DWORD jattrs = GetFileAttributesW(Extended(dest_junction).c_str());
            if (st && st->active && st->completed_items >= 66000 && jattrs != INVALID_FILE_ATTRIBUTES) {
                ready_mid_copy = true;
                break;
            }
            if (manager.FinalStatus(primary)) break;
            Pump();
            ResolveUnattendedAuthorization(manager);
            Sleep(2);
        }
        Check(ready_mid_copy && !manager.FinalStatus(primary),
            (std::string(label) + ": >66k items and leaf junction copied while primary stays active").c_str());

        const fs::path folder_path = fs::path(dest_parent / L"新建文件夹").make_preferred();
        ops::OpRequest create;
        create.type = ops::OpType::CreateFolder;
        create.collision_policy = ops::CollisionPolicy::KeepBoth;
        create.sources = {folder_path.wstring()};
        const auto create_started = GetTickCount64();
        const auto create_task = manager.Submit(std::move(create));

        bool create_done_mid_copy = false;
        ULONGLONG create_ms = 0;
        const auto create_deadline = GetTickCount64() + 15000;
        while (GetTickCount64() < create_deadline) {
            if (const auto final = manager.FinalStatus(create_task)) {
                create_ms = GetTickCount64() - create_started;
                create_done_mid_copy = final->phase == ops::OpPhase::Completed && !manager.FinalStatus(primary);
                break;
            }
            Pump();
            ResolveUnattendedAuthorization(manager);
            Sleep(1);
        }
        const auto create_completions = manager.DrainCompletions();
        const auto create_finished = manager.DrainFinishedTasks();
        const bool create_events_ok =
            std::any_of(create_completions.begin(), create_completions.end(), [&](const auto& c) {
                return c.task_id == create_task && c.destinations.size() == 1 &&
                    _wcsicmp(c.destinations.front().c_str(), folder_path.c_str()) == 0;
            }) &&
            std::any_of(create_finished.begin(), create_finished.end(), [&](const auto& f) {
                return f.task_id == create_task && f.success;
            });
        Check(create_done_mid_copy && create_ms < 5000 && create_events_ok && fs::is_directory(folder_path),
            (std::string(label) + ": CreateFolder and completion/intent events succeed during active copy").c_str());

        const fs::path renamed_path = fs::path(dest_parent / L"已重命名文件夹").make_preferred();
        ops::OpRequest rename;
        rename.type = ops::OpType::Rename;
        rename.collision_policy = ops::CollisionPolicy::KeepBoth;
        rename.sources = {folder_path.wstring()};
        rename.new_name = L"已重命名文件夹";
        const auto rename_started = GetTickCount64();
        const auto rename_task = manager.Submit(std::move(rename));

        bool rename_done_mid_copy = false;
        ULONGLONG rename_ms = 0;
        const auto rename_deadline = GetTickCount64() + 15000;
        while (GetTickCount64() < rename_deadline) {
            if (const auto final = manager.FinalStatus(rename_task)) {
                rename_ms = GetTickCount64() - rename_started;
                rename_done_mid_copy = final->phase == ops::OpPhase::Completed && !manager.FinalStatus(primary);
                break;
            }
            Pump();
            ResolveUnattendedAuthorization(manager);
            Sleep(1);
        }
        const auto rename_completions = manager.DrainCompletions();
        const auto rename_finished = manager.DrainFinishedTasks();
        const bool rename_events_ok =
            std::any_of(rename_completions.begin(), rename_completions.end(), [&](const auto& c) {
                return c.task_id == rename_task && c.sources.size() == 1 && c.destinations.size() == 1 &&
                    _wcsicmp(c.sources.front().c_str(), folder_path.c_str()) == 0 &&
                    _wcsicmp(c.destinations.front().c_str(), renamed_path.c_str()) == 0;
            }) &&
            std::any_of(rename_finished.begin(), rename_finished.end(), [&](const auto& f) {
                return f.task_id == rename_task && f.success;
            });
        Check(rename_done_mid_copy && rename_ms < 5000 && rename_events_ok &&
            fs::is_directory(renamed_path) && !fs::exists(folder_path),
            (std::string(label) + ": inline Rename succeeds during active copy").c_str());

        bool primary_done = false;
        const auto copy_deadline = GetTickCount64() + 300000;
        while (GetTickCount64() < copy_deadline) {
            if (manager.FinalStatus(primary)) { primary_done = true; break; }
            Pump();
            ResolveUnattendedAuthorization(manager);
            Sleep(5);
        }
        const auto copy_ms = GetTickCount64() - copy_started;
        const auto final_copy = manager.FinalStatus(primary);
        const DWORD out_jattrs = GetFileAttributesW(Extended(dest_junction).c_str());
        Check(primary_done && final_copy && final_copy->phase == ops::OpPhase::Completed &&
            out_jattrs != INVALID_FILE_ATTRIBUTES && (out_jattrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
            fs::is_empty(outside),
            (std::string(label) + ": primary copy finished cleanly and preserved leaf junction without writing outside").c_str());
        std::printf("METRIC subcase=%s create_ms=%llu rename_ms=%llu copy_ms=%llu\n",
            label, create_ms, rename_ms, copy_ms);
    };

    run_subcase("fresh", dest_base / "fresh", false);
    run_subcase("empty_root", dest_base / "empty", true);

    manager.Stop();

    Check(diagnostics::runtime::Flush(5000), "runtime diagnostics flushed");
    diagnostics::runtime::Shutdown();
    const fs::path log_file = root / "Diagnostics" / "Runtime" /
        ("ops_interactive_control_test-" + std::to_string(GetCurrentProcessId()) + ".jsonl");
    std::ifstream log_in(log_file);
    std::string line;
    size_t admission_events = 0;
    size_t proof2_pass_events = 0;
    size_t reject16_events = 0;
    while (std::getline(log_in, line)) {
        if (line.find("\"ops_interactive_admission\"") == std::string::npos) continue;
        ++admission_events;
        if (line.find("\"reject\":0") != std::string::npos &&
            line.find("\"visited\":0") != std::string::npos &&
            line.find("\"target_proof\":2") != std::string::npos) {
            ++proof2_pass_events;
        }
        if (line.find("\"reject\":16") != std::string::npos) ++reject16_events;
    }
    std::printf("METRIC admission_events=%zu proof2_pass_events=%zu reject16_events=%zu\n",
        admission_events, proof2_pass_events, reject16_events);
    Check(admission_events >= 4 && proof2_pass_events == admission_events && reject16_events == 0,
        "diagnostics confirm target_proof=2, visited=0, reject=0 for all interactive admissions");

    std::printf("RESULT failures=%d dest=%ls\n", failures, dest_base.c_str());
    CoUninitialize();
    return failures ? 1 : 0;
}
}
// Before the task-bound API exists, exercise the exact legacy callback behavior.
template<class T> bool PauseBound(T& manager, uint64_t id) {
    if constexpr (requires { manager.PauseTask(id); }) return manager.PauseTask(id);
    else { manager.PauseCurrent(); return true; }
}
template<class T> bool ResumeBound(T& manager, uint64_t id) {
    if constexpr (requires { manager.ResumeTask(id); }) return manager.ResumeTask(id);
    else { manager.ResumeCurrent(); return true; }
}
template<class T> bool HasFailureSnapshot(const T& finished, uint64_t id) {
    if constexpr (requires { finished.final_status; })
        return finished.final_status && finished.final_status->task_id == id &&
            finished.final_status->phase == ops::OpPhase::Failed && !finished.final_status->last_error.empty();
    else return false;
}
int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "--oversized-fresh-target") {
        std::wstring dest_root;
        if (argc == 4 && std::string(argv[2]) == "--dest-root") {
            dest_root = std::filesystem::path(argv[3]).wstring();
        } else if (argc != 2) {
            std::fprintf(stderr, "Usage: --oversized-fresh-target [--dest-root <path>]\n");
            return 2;
        }
        return RunOversizedFreshTargetTest(dest_root);
    }
    if (argc == 2 && std::string(argv[1]) == "--target-tree-slices") {
        namespace fs = std::filesystem;
        const auto root = fs::absolute(fs::path("bench_data/copy-toolbar-20261122/ops") /
            ("walk-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64())));
        const auto target = root / "destination", branch = target / "branch";
        fs::create_directories(branch);
        for (int i = 0; i < 16; ++i) std::ofstream(branch / std::to_string(i)).put('x');
        std::atomic<bool> stopping{false};
        const auto walk = [&](ops::InteractiveTreeWalkBudget& budget, bool slow = false) {
            std::vector<std::wstring> work{target.wstring()};
            return ops::WalkPlainInteractiveTargets(work, budget, [&](const std::wstring& path) {
                if (slow) { slow = false; Sleep(300); } // controlled single slow directory read
                return path;
            }, [](const std::wstring& p, const wchar_t* leaf) { return (fs::path(p) / leaf).wstring(); });
        };
        ops::InteractiveTreeWalkBudget stable{stopping, [] { return true; }, GetTickCount64() + 250};
        Check(walk(stable, true) && stable.continuations && stable.visited == 19,
            "single read slower than 250ms resumes the same walk: each directory/entry visited only once");
        unsigned checks = 0;
        ops::InteractiveTreeWalkBudget superseded{stopping, [&] { return ++checks < 5; }, 0, 0};
        Check(!walk(superseded) && superseded.interrupted && superseded.visited == 4,
            "superseded/cancelled admission interrupts in-progress traversal before it can authorize");
        ops::InteractiveTreeWalkBudget oversized{stopping, [] { return true; }, 0, 0};
        oversized.visited = 65535;
        Check(!walk(oversized) && !oversized.interrupted, "renewals never waive the original entry bound");
        fs::create_directory(root / "outside");
        Check(MakeJunction(branch / "escape", root / "outside"), "real nested junction fixture");
        ops::InteractiveTreeWalkBudget alias{stopping, [] { return true; }, 0, 0};
        Check(!walk(alias) && alias.continuations && !alias.interrupted, "actual reparse still rejects after multiple checkpoints");
        stopping = true;
        ops::InteractiveTreeWalkBudget stopped{stopping, [] { return true; }, GetTickCount64() + 250};
        Check(!walk(stopped) && stopped.interrupted && !stopped.visited, "shutdown stops before the first filesystem query");
        std::printf("failures=%d\n", failures);
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--task-controls-filesystem-only") {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
        const auto root = std::filesystem::absolute(std::filesystem::path("bench_data/concurrent-review-20261119") /
            ("pause-fs-" + std::to_string(GetCurrentProcessId())));
        std::filesystem::create_directories(root / "source");
        std::filesystem::create_directories(root / "destination");
        for (int i = 0; i < 1500; ++i) { std::ofstream out(root / "source" / std::to_string(i)); out << "pause-task production transfer"; }
        ops::OpsManager manager;
        manager.Start([] {}, false);
        ops::OpRequest request;
        request.type = ops::OpType::Copy;
        request.sources = {(root / "source").wstring()};
        request.dest_dir = (root / "destination").wstring();
        const auto task = manager.Submit(request);
        bool paused = false;
        const auto deadline = GetTickCount64() + 10000;
        while (GetTickCount64() < deadline && !manager.FinalStatus(task)) {
            if (PauseBound(manager, task)) { paused = true; break; }
            Pump(); Sleep(1);
        }
        Check(paused, "task-bound pause accepted on real copy");
        const auto hold = GetTickCount64() + 100;
        while (GetTickCount64() < hold) { Pump(); Sleep(1); }
        Check(!manager.FinalStatus(task), "paused real copy remains nonterminal");
        Check(ResumeBound(manager, task), "task-bound resume accepted on real copy");
        WaitIdle(manager);
        bool payloads = true;
        for (int i = 0; i < 1500; ++i) {
            std::ifstream input(root / "destination" / "source" / std::to_string(i));
            const std::string value((std::istreambuf_iterator<char>(input)), {});
            payloads = payloads && value == "pause-task production transfer";
        }
        Check(payloads && manager.FinalStatus(task) && manager.FinalStatus(task)->phase == ops::OpPhase::Completed,
            "resumed copy completes with all 1500 payloads intact");
        Check(!PauseBound(manager, task) && !ResumeBound(manager, task), "real completed task rejects stale controls");
        manager.Stop();
        std::filesystem::remove_all(root);
        CoUninitialize();
        std::printf("RESULT failures=%d\n", failures);
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--finished-retention-only") {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
        const auto root = std::filesystem::absolute(std::filesystem::path("bench_data/concurrent-review-20261119") /
            ("finished-" + std::to_string(GetCurrentProcessId())));
        std::filesystem::create_directories(root);
        ops::OpsManager manager;
        ops::OpsManagerAuditAccess::DisableLane(manager);
        manager.Start([] {}, false);
        ops::OpRequest request;
        request.type = ops::OpType::Copy;
        request.sources = {(root / "absent.txt").wstring()};
        request.dest_dir = root.wstring();
        const auto failed = manager.Submit(request);
        WaitIdle(manager);
        const auto first = manager.QueryTask(failed);
        Check(first && first->phase == ops::OpPhase::Failed && !first->last_error.empty(), "real missing-source copy fails with diagnostic");
        for (int i = 0; i < 20; ++i) {
            request = {};
            request.type = ops::OpType::CreateFolder;
            request.sources = {(root / std::to_string(i)).wstring()};
            manager.Submit(request);
        }
        WaitIdle(manager);
        Check(!manager.FinalStatus(failed), "first failure evicted from sixteen-entry final cache");
        const auto finished = manager.DrainFinishedTasks();
        const auto it = std::find_if(finished.begin(), finished.end(), [=](const auto& value) { return value.task_id == failed; });
        Check(finished.size() == 21 && it != finished.end() && HasFailureSnapshot(*it, failed), "undrained failure preserves diagnostic beyond final cache");
        manager.Stop();
        std::filesystem::remove_all(root);
        CoUninitialize();
        std::printf("RESULT failures=%d\n", failures);
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--retry-retention-only") {
        const auto root = std::filesystem::absolute(std::filesystem::path("bench_data/concurrent-review-20261119") /
            ("retry-merge-" + std::to_string(GetCurrentProcessId())));
        std::filesystem::create_directories(root / "primary");
        Check(ops::OpsManagerAuditAccess::RetryMergeBounded(root), "scheduler retains newest eight retry records across lanes");
        std::filesystem::remove_all(root);
        std::printf("RESULT failures=%d\n", failures);
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--task-controls-only") {
        using A = ops::OpsManagerAuditAccess;
        ops::OpsManager manager;
        A::SetTransfer(manager, 41);
        Check(!PauseBound(manager, 40) && !A::Paused(manager), "stale pause leaves replacement primary running");
        Check(PauseBound(manager, 41) && A::Paused(manager), "exact primary pause accepted");
        Check(!ResumeBound(manager, 40) && A::Paused(manager), "stale resume leaves primary paused");
        Check(ResumeBound(manager, 41) && !A::Paused(manager), "exact primary resume accepted");
        const auto child_id = A::InjectInteraction(manager);
        // Presented child is waiting for conflict while the primary remains live.
        Check(PauseBound(manager, 41) && A::Paused(manager), "primary pause ignores presented child");
        Check(ResumeBound(manager, 41) && !A::Paused(manager), "primary resume ignores presented child");
        auto child = manager.Status();
        Check(child.task_id == child_id, "interaction presents child task");
        auto executor = A::Child(manager);
        A::SetTransfer(*executor, child_id);
        Check(PauseBound(manager, child_id) && A::Paused(*executor) && !A::Paused(manager), "exact child pause leaves primary running");
        Check(ResumeBound(manager, child_id) && !A::Paused(*executor), "exact child resume accepted");
        A::SetTransfer(*executor, child_id + 100);
        Check(!PauseBound(manager, child_id) && !A::Paused(*executor), "reused child executor rejects former task");
        A::CancelTransfer(manager);
        Check(!PauseBound(manager, 41) && !ResumeBound(manager, 41), "cancelled task controls rejected");
        A::ClearTransfer(*executor);
        A::RemoveInteraction(manager);
        A::ClearTransfer(manager);
        Check(!PauseBound(manager, 41) && !ResumeBound(manager, 41), "completed task controls rejected");
        std::printf("RESULT failures=%d\n", failures);
        return failures ? 1 : 0;
    }
    if (argc != 1) { std::fprintf(stderr, "Unknown test filter\n"); return 2; }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    const auto root = std::filesystem::absolute(std::filesystem::path("bench_data/small-files-responsive-20261119") /
        ("control-fixture-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64())));
    std::filesystem::create_directories(root / "source");
    std::filesystem::create_directories(root / "injected");
    std::filesystem::create_directories(root / "actual");
    for (int i = 0; i < 3000; ++i) { std::ofstream out(root / "source" / (std::to_string(i) + ".txt")); out << "history and routing fixture"; }
    ops::OpRequest copy;
    copy.type = ops::OpType::Copy;
    copy.sources = {(root / "source").make_preferred().wstring()};
    copy.dest_dir = (root / "injected").make_preferred().wstring();
    ops::OpsManager injected;
    ops::OpsManagerAuditAccess::DisableLane(injected);
    injected.Start([] {}, false);
    const auto primary = injected.Submit(copy);
    injected.RetainTask(primary);
    for (int i = 0; i < 1000; ++i) {
        auto status = injected.QueryTask(primary);
        if (status && status->phase != ops::OpPhase::Queued) break;
        Pump(); ResolveUnattendedAuthorization(injected); std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    // This injects only two UI interaction states, not copy progress or timing.
    // The primary operation below still runs the real production filesystem path.
    const auto child = ops::OpsManagerAuditAccess::InjectInteraction(injected);
    const auto presented = injected.Status();
    const auto conflict = injected.PendingConflict();
    Check(presented.task_id == child && conflict && conflict->task_id == child,
        "simultaneous conflicts present status and conflict from the same executor");
    if (conflict) injected.ResolveConflict(conflict->token, ops::ConflictChoice::Skip, false);
    Check(ops::OpsManagerAuditAccess::ConflictResolved(injected), "high-bit conflict token resolves only interactive executor");
    ops::OpsManagerAuditAccess::SetAuthorization(injected, child);
    Check(injected.Status().task_id == child && !injected.PendingConflict(), "interactive authorization suppresses unrelated primary conflict");
    injected.ResolveAuthorization(child, true);
    Check(ops::OpsManagerAuditAccess::AuthorizationResolved(injected, child), "authorization resolution is task-ID scoped");
    Check(Wait(injected, primary), "primary archives its own terminal result while child interaction remains visible");
    const auto final = injected.QueryTask(primary);
    Check(final && final->task_id == primary && final->phase == ops::OpPhase::Completed,
        "retained primary result keeps original task ID and completed phase");
    Check(injected.Status().task_id == child, "UI can still present child while primary terminal result is archived");
    ops::OpsManagerAuditAccess::RemoveInteraction(injected);
    injected.Stop();

    ops::OpsManager actual;
    actual.Start([] {}, false);
    copy.dest_dir = (root / "actual").make_preferred().wstring();
    const auto heavy = actual.Submit(copy);
    std::filesystem::create_directories(root / "metadata");
    { std::ofstream file(root / "metadata/before.txt"); file << "undo exact interactive rename"; }
    ops::OpRequest rename;
    rename.type = ops::OpType::Rename;
    rename.sources = {Extended(root / "metadata/before.txt")};
    rename.new_name = L"after.txt";
    const auto light = actual.Submit(rename);
    Check(Wait(actual, light) && !actual.FinalStatus(heavy), "real interactive rename finishes before full primary copy");
    Check(Wait(actual, heavy), "full primary copy finishes after interactive rename");
    const auto summary = actual.UndoHistorySummary(false);
    Check(summary.size() == 1 && summary.front().task_id == light && actual.CanUndoTask(light),
        "out-of-order completion preserves consistent acceptance-ordered undo top");
    Check(actual.UndoTask(light), "undo executes displayed interactive task");
    WaitIdle(actual);
    size_t files = 0;
    for (const auto& item : std::filesystem::directory_iterator(root / "actual/source")) {
        std::ifstream in(item.path()); std::string bytes; std::getline(in, bytes);
        if (bytes == "history and routing fixture") ++files;
    }
    Check(std::filesystem::exists(root / "metadata/before.txt") && !std::filesystem::exists(root / "metadata/after.txt") && files == 3000,
        "undo reverses only light rename and preserves all 3000 copied file contents");
    actual.Stop();
    std::filesystem::create_directories(root / "source/linked");
    std::filesystem::create_directories(root / "outside");
    { std::ofstream file(root / "source/linked/through-junction.txt"); file << "junction target fixture"; }
    const auto junction = root / "junction-destination/source/linked";
    const bool linked = MakeJunction(junction, root / "outside");
    Check(linked, "isolated existing destination junction fixture created");
    if (linked) {
        ops::OpsManager guarded;
        guarded.Start([] {}, false);
        copy.dest_dir = (root / "junction-destination").make_preferred().wstring();
        const auto primary_junction = guarded.Submit(copy);
        const auto waiting = GetTickCount64() + 3000;
        while (GetTickCount64() < waiting) {
            auto state = guarded.QueryTask(primary_junction);
            if (state && state->phase != ops::OpPhase::Queued) break;
            Pump(); std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        ops::OpRequest outside_create;
        outside_create.type = ops::OpType::CreateFolder;
        outside_create.sources = {Extended(root / "outside/after-copy")};
        const auto after = guarded.Submit(outside_create);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const auto queued = guarded.QueryTask(after);
        Check(!guarded.FinalStatus(primary_junction) && queued && queued->phase == ops::OpPhase::Queued,
            "existing target junction conservatively prevents lexical-footprint bypass");
        Check(Wait(guarded, primary_junction) && Wait(guarded, after) &&
            std::filesystem::exists(root / "outside/through-junction.txt") && std::filesystem::exists(root / "outside/after-copy"),
            "junction primary and later operation retain FIFO filesystem behavior");
        guarded.Stop();
        Check(RemoveDirectoryW(junction.c_str()) != FALSE, "junction link removed without traversing target");
    }
    std::printf("RESULT failures=%d fixture=%ls\n", failures, root.c_str());
    CoUninitialize();
    return failures ? 1 : 0;
}

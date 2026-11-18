#include "../ops/failed_retry_template.h"
#include <psapi.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <chrono>
#include <algorithm>

namespace pulse::ops {
struct OpsManagerAuditAccess {
    static void Seed(OpsManager& manager, const OpRequest& request, const std::vector<OpFailure>& failures) {
        manager.running_ = true; // No worker: inspect real retry admission without executing synthetic paths.
        manager.failed_retries_.push_back({41, MakeFailedRetryTemplate(request), failures});
    }
    static std::vector<OpRequest> Take(OpsManager& manager) {
        std::vector<OpRequest> out;
        for (auto& item : manager.queue_) out.push_back(std::move(item.req));
        manager.queue_.clear();
        return out;
    }
    static size_t RetainedSources(OpsManager& manager) {
        std::lock_guard lock(manager.mutex_);
        size_t count = 0;
        for (const auto& retry : manager.failed_retries_) count += retry.req.sources.size();
        return count;
    }
    static void Finish(OpsManager& manager) { manager.running_ = false; }
};
}
namespace {
using namespace pulse::ops;
int failed = 0;
void Check(bool value, const char* text) { printf("[%s] %s\n", value ? "PASS" : "FAIL", text); if (!value) ++failed; }
size_t SourceBytes(const OpRequest& request) {
    size_t bytes = request.sources.capacity() * sizeof(std::wstring);
    for (const auto& path : request.sources) if (path.capacity() >= 8) bytes += (path.capacity() + 1) * sizeof(wchar_t);
    return bytes;
}
void Memory(const char* phase) {
    static std::vector<ULONG_PTR> pages(262144);
    PROCESS_MEMORY_COUNTERS_EX m{}; m.cb = sizeof(m);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m), sizeof(m));
    size_t resident = 0;
    if (QueryWorkingSet(GetCurrentProcess(), pages.data(), static_cast<DWORD>(pages.size() * sizeof(ULONG_PTR))) && pages[0] < pages.size()) {
        SYSTEM_INFO info{}; GetSystemInfo(&info);
        for (size_t i = 1; i <= pages[0]; ++i) if (!(pages[i] & 256)) resident += info.dwPageSize;
    }
    Check(resident != 0, "private resident sample is valid");
    printf("[MEM] %s private_commit=%zu private_resident=%zu\n", phase, m.PrivateUsage, resident);
}
bool Await(OpsManager& manager, uint64_t task) {
    const auto until = GetTickCount64() + 15000;
    while (GetTickCount64() < until) {
        if (const auto status = manager.FinalStatus(task); status && !status->active) return true;
        Sleep(5);
    }
    return false;
}
void RealRetry() {
    wchar_t exe[32768]{};
    GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    const auto parent = std::filesystem::path(exe).parent_path().parent_path() / L"bench_data";
    const auto root = parent / (L"retry_template_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root / L"src");
    std::filesystem::create_directories(root / L"dest/src");
    const auto leaf = [](int i) { return L"file" + std::to_wstring(i) + L".txt"; };
    for (int i = 0; i < 20; ++i) { std::ofstream f(root / L"src" / leaf(i)); f << "retry test"; }
    HANDLE held_a = CreateFileW((root / L"src" / leaf(3)).c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    HANDLE held_b = CreateFileW((root / L"src" / leaf(7)).c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(held_a != INVALID_HANDLE_VALUE && held_b != INVALID_HANDLE_VALUE, "hold two isolated files to reproduce partial failure");
    OpsManager manager;
    manager.Start({}, false);
    OpRequest request; request.type = OpType::Move; request.collision_policy = CollisionPolicy::Replace;
    request.sources = {(root / L"src").wstring()}; request.dest_dir = (root / L"dest").wstring();
    const auto task = manager.Submit(request);
    Check(task && Await(manager, task), "real partial move completes in isolated fixture");
    const auto status = manager.FinalStatus(task);
    if (status) printf("[DETAIL] phase=%d failures=%zu\n", static_cast<int>(status->phase), status->failures.size());
    Check(status && status->phase == OpPhase::Failed && status->failures.size() == 2 &&
        std::filesystem::exists(root / L"dest/src" / leaf(0)), "successful source moves and held sources remain retryable");
    Check(OpsManagerAuditAccess::RetainedSources(manager) == 0, "real stored retry retains no original source list");
    if (held_a != INVALID_HANDLE_VALUE) CloseHandle(held_a);
    if (held_b != INVALID_HANDLE_VALUE) CloseHandle(held_b);
    const auto spawned = manager.RetryFailedItems(task);
    Check(spawned.size() == 1 && Await(manager, spawned.front().task_id) &&
        std::filesystem::exists(root / L"dest/src" / leaf(3)) && std::filesystem::exists(root / L"dest/src" / leaf(7)) &&
        !std::filesystem::exists(root / L"src" / leaf(3)), "retry moves only the now-unlocked failed items");
    const auto original = manager.FinalStatus(task);
    Check(original && original->phase == OpPhase::Failed && manager.RetryFailedItems(task).empty(), "original outcome remains and consumed retry cannot run again");
    manager.Stop();
    // Delete only the files this test created; retain its uniquely named evidence directory.
    for (int i = 0; i < 20; ++i) {
        std::filesystem::remove(root / L"dest/src" / leaf(i));
        std::filesystem::remove(root / L"src" / leaf(i));
    }
}
}
int RunRetryTemplateTest(bool memory_only) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    OpRequest request; request.type = OpType::Move;
    request.dest_dir = L"C:\\target"; request.new_name = L"renamed.txt";
    request.collision_policy = CollisionPolicy::KeepBoth; request.refresh_directories = {L"C:\\target"};
    request.template_data = {1, 2, 3}; request.display_name = L"display"; request.undo_group = 17;
    request.only_if_empty = true; request.lock_retry = true; request.is_undo = true;
    request.close_first.push_back({123, {}, L"test owner", L"test.exe", true});
    if (memory_only) {
        Memory("before");
        request.sources.reserve(100000);
        for (size_t i = 0; i < 100000; ++i) request.sources.push_back(L"C:\\fixture\\" + std::wstring(170, L'x') + std::to_wstring(i));
        std::vector<OpRequest> retained;
        for (int i = 0; i < 4; ++i) retained.push_back(MakeFailedRetryTemplate(request));
        request = {};
        size_t bytes = 0;
        for (const auto& saved : retained) bytes += SourceBytes(saved);
        Memory("four_retries");
        printf("[CAPACITY] retained_source_bytes=%zu\n", bytes);
        const auto start = std::chrono::steady_clock::now();
        size_t checksum = 0;
        for (int i = 0; i < 20; ++i) { auto copy = retained[i % 4]; copy.sources.clear(); copy.sources.push_back(L"C:\\failed.txt"); checksum += copy.sources.front().size(); }
        printf("[TIME] twenty_retry_copies_ms=%.6f checksum=%zu\n", std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count(), checksum);
        return failed ? 1 : 0;
    }
    request.sources = {L"C:\\source\\success.txt", L"C:\\source\\a.txt", L"C:\\source\\b.txt"};
    const auto saved = MakeFailedRetryTemplate(request);
    Check(saved.sources.empty() && saved.sources.capacity() == 0, "template releases original source strings and vector storage");
    Check(saved.close_first.empty() && saved.close_first.capacity() == 0, "template releases obsolete lock-owner input");
    Check(request.sources.size() == 3 && request.close_first.size() == 1, "live operation request remains unchanged");
    Check(saved.dest_dir == request.dest_dir && saved.new_name == request.new_name && saved.type == request.type &&
        saved.template_data == request.template_data && saved.refresh_directories == request.refresh_directories &&
        saved.collision_policy == request.collision_policy && saved.undo_group == request.undo_group &&
        saved.only_if_empty == request.only_if_empty && saved.display_name == request.display_name, "retry preserves unrelated operation options");
    const std::vector<OpFailure> failures = {{L"C:\\source\\a.txt", L"C:\\target\\a.txt", L"failed", E_FAIL},
        {L"C:\\source\\b.txt", L"C:\\other\\renamed.txt", L"failed", E_FAIL}};
    OpsManager manager; OpsManagerAuditAccess::Seed(manager, request, failures);
    const auto selected = manager.RetryFailedItems(41, {1}); const auto selected_requests = OpsManagerAuditAccess::Take(manager);
    Check(selected.size() == 1 && selected_requests.size() == 1 && selected_requests[0].sources == std::vector<std::wstring>{failures[1].source} &&
        selected_requests[0].dest_dir == L"C:\\other" && selected_requests[0].new_name == L"renamed.txt", "selected retry preserves destination and renamed leaf");
    Check(selected_requests.size() == 1 && selected_requests[0].close_first.empty() && !selected_requests[0].lock_retry && !selected_requests[0].is_undo &&
        selected_requests[0].undo_group == 0 && selected_requests[0].collision_policy == CollisionPolicy::KeepBoth, "retry does not reuse lock termination or undo context");
    Check(manager.RetryFailedItems(41, {99}).empty(), "invalid failure index queues nothing");
    const auto all = manager.RetryFailedItems(41); const auto all_requests = OpsManagerAuditAccess::Take(manager);
    Check(all.size() == 2 && all_requests.size() == 2 && manager.RetryFailedItems(41).empty(), "all-items retry groups destinations and consumes its record");
    request.type = OpType::RealDelete;
    OpsManagerAuditAccess::Seed(manager, request, failures);
    const auto deleted = manager.RetryFailedItems(41); const auto delete_requests = OpsManagerAuditAccess::Take(manager);
    Check(deleted.size() == 1 && delete_requests.size() == 1 && delete_requests[0].sources ==
        std::vector<std::wstring>{failures[0].source, failures[1].source}, "delete retry reconstructs only failed sources");
    OpsManagerAuditAccess::Finish(manager);
    RealRetry();
    return failed ? 1 : 0;
}

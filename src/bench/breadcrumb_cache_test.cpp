#include "../app/breadcrumb_cache.h"
#include "../app/breadcrumb_revision.h"
#include <cstdio>
#include <thread>

int main() {
    using namespace pulse::app;
    BreadcrumbCache cache;
    bool ok = true;
    const auto check = [&](bool pass, const char* message) {
        std::printf("[%s] %s\n", pass ? "PASS" : "FAIL", message); ok &= pass;
    };
    auto first = cache.Acquire(L"Z:\\folder");
    auto second = cache.Acquire(L"Z:/folder/");
    check(first.start && !second.start && first.job == second.job, "same mapped-drive path shares in-flight provider");
    first.job.reset();
    auto third = cache.Acquire(L"Z:\\folder");
    check(!third.start && third.job == second.job, "closing one subscriber does not cancel or release provider");
    cache.Retire(L"Z:\\folder", second.job);
    auto replacement = cache.Acquire(L"Z:\\folder");
    cache.Retire(L"Z:\\folder", second.job);
    check(replacement.start && cache.Acquire(L"Z:\\folder").job == replacement.job,
        "late retirement cannot erase replacement provider");
    BreadcrumbFolders folders{{L"child", L"\\\\server\\share\\child"}}, out;
    cache.Store(L"\\\\server\\share", 42, 1000, folders);
    check(cache.Load(L"\\\\server\\share", 42, 1499, out) && out == folders, "UNC completion reused with matching revision and TTL");
    check(!cache.Load(L"\\\\server\\share", 43, 1499, out), "directory revision invalidates recent completion");
    cache.Store(L"\\\\server\\share", 42, 1000, folders);
    check(!cache.Load(L"\\\\server\\share", 42, 1501, out), "TTL expires even without revision change");
    cache.Store(L"\\\\server\\share", 42, 1000, folders);
    cache.Load(L"\\\\server\\share", 42, 1400, out);
    check(!cache.Load(L"\\\\server\\share", 42, 1501, out), "cache hits do not extend enumeration TTL");
    cache.Store(L"bad", 1, 1000, folders); cache.Invalidate(L"bad");
    check(!cache.Load(L"bad", 1, 1001, out), "provider error cannot leave reusable old completion");
    for (int n = 0; n < 100; ++n) cache.Store(std::to_wstring(n), 1, 1000, folders);
    check(cache.Size().first <= 32 && cache.Size().second <= 2 * 1024 * 1024, "completed cache has count and byte limits");
    BreadcrumbFolders huge{{std::wstring(1100000, L'x'), L"large"}};
    cache.Store(L"huge", 1, 1000, huge);
    check(!cache.Load(L"huge", 1, 1001, out) && cache.Size().second <= 2 * 1024 * 1024,
        "oversize result bypasses completed cache");
    std::shared_ptr<BreadcrumbJob> concurrent[8];
    std::thread workers[8];
    for (int n = 0; n < 8; ++n) workers[n] = std::thread([&, n] { concurrent[n] = cache.Acquire(L"parallel").job; });
    for (auto& worker : workers) worker.join();
    for (const auto& job : concurrent) ok &= job == concurrent[0];
    check(ok, "concurrent windows share one provider job");
    CreateDirectoryW(L"bench_data", nullptr);
    const auto fixture = L"bench_data\\breadcrumb-revision-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64());
    if (!CreateDirectoryW(fixture.c_str(), nullptr)) {
        check(false, "create isolated directory revision fixture");
    } else {
        uint64_t before = 0, after = 0;
        check(ReadBreadcrumbRevision(fixture, before), "read real directory revision");
        cache.Store(fixture, before, 1000, {});
        Sleep(25);
        const auto child = fixture + L"\\new-child";
        const bool made_child = CreateDirectoryW(child.c_str(), nullptr) != FALSE;
        const bool changed = made_child && ReadBreadcrumbRevision(fixture, after) && after != before;
        check(changed && !cache.Load(fixture, after, 1001, out),
            "actual directory child creation invalidates completion before TTL");
        if (made_child) RemoveDirectoryW(child.c_str());
        check(RemoveDirectoryW(fixture.c_str()) != FALSE, "remove isolated directory fixture");
        check(!ReadBreadcrumbRevision(fixture, after), "missing actual directory fails revision validation");
    }
    return ok ? 0 : 1;
}

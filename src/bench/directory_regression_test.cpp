#include <windows.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <vector>
#include <string>
static std::atomic<DWORD> next_error{0};
static BOOL WINAPI ProbeFindNext(HANDLE h, LPWIN32_FIND_DATAW data) {
    if (const DWORD error = next_error.load()) { SetLastError(error); return FALSE; }
    return FindNextFileW(h, data);
}
#define FindNextFileW ProbeFindNext
#include "../fs/fs_enum.cpp"
#undef FindNextFileW
static HANDLE open_entered, open_release;
static std::atomic<bool> block_open{false};
static HANDLE WINAPI ProbeOpen(LPCWSTR p, DWORD a, DWORD share, LPSECURITY_ATTRIBUTES sa,
                              DWORD create, DWORD flags, HANDLE tmpl) {
    if (block_open.exchange(false)) {
        SetEvent(open_entered);
        WaitForSingleObject(open_release, INFINITE);
    }
    return CreateFileW(p, a, share, sa, create, flags, tmpl);
}
#define CreateFileW ProbeOpen
#include "../fs/fs_watch.cpp"
#undef CreateFileW
static int failures;
static void Check(bool ok, const char* label) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); failures += !ok; }
int main() {
    const auto dir = std::filesystem::absolute(L"bench_data/directory-regression-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(dir);
    HANDLE file = CreateFileW((dir / L"entry.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    CloseHandle(file);
    std::atomic<int> success{0};
    std::vector<std::thread> readers;
    for (int i = 0; i < 16; ++i) readers.emplace_back([&] {
        try { std::vector<pulse::fs::DirEntry> out; pulse::fs::EnumerateDirectory(dir.wstring(), out); if(out.size()==1) ++success; } catch (...) {}
    });
    for(auto& t : readers) t.join();
    Check(success == 16, "DIR-02 concurrent cold NT initialization");
    std::vector<pulse::fs::DirEntry> out;
    next_error = ERROR_ACCESS_DENIED;
    bool threw = false;
    try { pulse::fs::EnumerateFindFirstFileEx(dir.wstring(), out, {}); } catch (...) { threw = true; }
    Check(threw && out.empty(), "DIR-03 fallback enumeration rejects non-EOF and partial snapshot");
    next_error = 0;
    pulse::fs::EnumerateFindFirstFileEx(dir.wstring(), out, {});
    Check(out.size() == 1, "DIR-03 normal EOF succeeds");
    open_entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    open_release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    block_open = true;
    std::atomic<int> calls{0};
    {
        pulse::fs::DirWatch watch;
        Check(watch.Start(dir.wstring(), [&](bool, auto) { ++calls; }), "DIR-01 watcher starts");
        Check(WaitForSingleObject(open_entered, 3000)==WAIT_OBJECT_0, "DIR-01 blocked open reached");
        const auto start = std::chrono::steady_clock::now();
        watch.Stop();
        Check(std::chrono::steady_clock::now()-start < std::chrono::milliseconds(200), "DIR-01 Stop does not join blocked provider");
    }
    SetEvent(open_release);
    for(int i=0;i<300 && pulse::fs::active_watchers.load();++i) Sleep(10);
    Check(calls==0 && pulse::fs::active_watchers.load()==0, "DIR-01 late worker drains without callback after destruction");
    HANDLE armed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    {
        pulse::fs::DirWatch watch;
        watch.Start(dir.wstring(), [&](bool overflow, auto) { if(overflow) SetEvent(armed); });
        Check(WaitForSingleObject(armed, 3000)==WAIT_OBJECT_0 && watch.Armed(), "DIR-04 nonrecursive initial arm requests reconciliation");
        watch.Stop();
    }
    for(int i=0;i<300 && pulse::fs::active_watchers.load();++i) Sleep(10);
    Check(pulse::fs::active_watchers.load()==0, "watcher resources drain after cancellation");
    CloseHandle(armed); CloseHandle(open_entered); CloseHandle(open_release);
    std::filesystem::remove_all(dir);
    return failures ? 1 : 0;
}

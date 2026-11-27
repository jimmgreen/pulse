#include "../ui/picker_save_draft_probe.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace {
using Probe = pulse::ui::PickerSaveDraftProbe;
using Clock = std::chrono::steady_clock;
std::atomic<int> failures = 0;
void Check(bool ok, const char* text) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", text);
    std::fflush(stdout);
    failures += !ok;
}
template<class Predicate> bool Wait(Predicate ready) {
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    do { if (ready()) return true; Sleep(1); } while (Clock::now() < deadline);
    return ready();
}
std::optional<Probe::Result> Read(Probe& probe, uint64_t generation, const std::wstring& path) {
    std::optional<Probe::Result> result;
    if (!probe.Start(generation, path)) return {};
    Wait([&] { result = probe.Take(generation, path); return result.has_value(); });
    return result;
}
bool Touch(const std::wstring& path) {
    const HANDLE file = CreateFileW(pulse::fs::NormalizePath(path).c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    CloseHandle(file); return true;
}
struct Gate {
    std::mutex mutex;
    std::condition_variable wake;
    bool release = false;
    std::atomic<unsigned> entered = 0, returned = 0;
    std::vector<std::wstring> paths;
    void Block(Probe::Result& result) {
        std::unique_lock lock(mutex);
        paths.push_back(result.path);
        ++entered;
        wake.wait(lock, [&] { return release; });
        result.attributes = FILE_ATTRIBUTE_NORMAL;
        ++returned;
    }
    void Release() {
        { std::lock_guard lock(mutex); release = true; }
        wake.notify_all();
    }
};
}

int wmain() {
    const auto root = std::filesystem::current_path() / L"bench_data" / L"picker-save-followup-20261125" /
        (L"probe-fixture-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root);
    const auto file = (root / L"normal.txt").wstring();
    const auto readonly = (root / L"readonly.txt").wstring();
    const auto missing = (root / L"missing.txt").wstring();
    Check(Touch(file) && Touch(readonly), "create isolated ordinary/readonly test fixtures");
    Check(SetFileAttributesW(readonly.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE, "set fixture readonly attribute");
    {
        Probe probe;
        auto result = Read(probe, 1, file);
        Check(result && result->error == ERROR_SUCCESS && result->attributes != INVALID_FILE_ATTRIBUTES &&
            !(result->attributes & FILE_ATTRIBUTE_DIRECTORY), "real regular file returns attributes");
        result = Read(probe, 2, root.wstring());
        Check(result && result->error == ERROR_SUCCESS && (result->attributes & FILE_ATTRIBUTE_DIRECTORY), "real directory remains a directory result");
        result = Read(probe, 3, missing);
        Check(result && result->attributes == INVALID_FILE_ATTRIBUTES && result->error == ERROR_FILE_NOT_FOUND,
            "nonexistent target returns exact not-found error");
        const DWORD parent_attributes = GetFileAttributesW(root.c_str());
        ULARGE_INTEGER available{}, total{};
        const bool disk_known = GetDiskFreeSpaceExW(root.c_str(), &available, &total, nullptr) != FALSE;
        Check(result && result->parent_attributes == parent_attributes && (result->parent_attributes & FILE_ATTRIBUTE_DIRECTORY),
            "new save target reports actual parent directory attributes");
        Check(disk_known && result && result->space_known && result->free_bytes <= total.QuadPart,
            "new save target reports real caller-available disk space despite not-found target");
        if (result) std::printf("SPACE probe=%llu direct=%llu total=%llu\n",
            static_cast<unsigned long long>(result->free_bytes), static_cast<unsigned long long>(available.QuadPart),
            static_cast<unsigned long long>(total.QuadPart));
        Check(GetFileAttributesW(missing.c_str()) == INVALID_FILE_ATTRIBUTES, "probe does not create nonexistent save target");
        result = Read(probe, 4, readonly);
        Check(result && result->error == ERROR_SUCCESS && (result->attributes & FILE_ATTRIBUTE_READONLY), "real readonly attribute retained without guessing write permission");
        Check(result && result->space_known && result->parent_attributes == parent_attributes,
            "existing readonly file retains parent and space advisory metadata");
        Check(SetFileAttributesW(root.c_str(), parent_attributes | FILE_ATTRIBUTE_READONLY) != FALSE,
            "set isolated parent directory readonly attribute");
        result = Read(probe, 7, missing);
        Check(result && result->error == ERROR_FILE_NOT_FOUND && result->space_known &&
            (result->parent_attributes & FILE_ATTRIBUTE_READONLY) && (result->parent_attributes & FILE_ATTRIBUTE_DIRECTORY),
            "directory readonly does not suppress space metadata or become a target access error");
        Check(SetFileAttributesW(root.c_str(), parent_attributes) != FALSE, "restore parent directory attributes");
        result = Read(probe, 8, (root / L"missing-parent" / L"save.txt").wstring());
        Check(result && result->error == ERROR_PATH_NOT_FOUND && result->parent_attributes == INVALID_FILE_ATTRIBUTES &&
            !result->space_known && result->free_bytes == 0, "missing parent keeps space unknown and preserves target error");
        result = Read(probe, 9, file + L"\\save.txt");
        Check(result && result->parent_attributes != INVALID_FILE_ATTRIBUTES &&
            !(result->parent_attributes & FILE_ATTRIBUTE_DIRECTORY) && !result->space_known && result->free_bytes == 0,
            "regular-file parent never supplies directory space metadata");
        auto deep = root.wstring();
        for (int i = 0; i < 5; ++i) {
            deep += L"\\Unicode-目录-" + std::to_wstring(i) + std::wstring(48, L'x');
            Check(CreateDirectoryW(pulse::fs::NormalizePath(deep).c_str(), nullptr) != FALSE, "create isolated Unicode long-path segment");
        }
        const auto long_file = deep + L"\\保存文件.txt";
        Check(long_file.size() > MAX_PATH && Touch(long_file), "create actual Unicode file beyond MAX_PATH");
        result = Read(probe, 5, long_file);
        Check(result && result->path == long_file && result->error == ERROR_SUCCESS && result->attributes != INVALID_FILE_ATTRIBUTES,
            "long-path attribute probe preserves caller identity and succeeds");
        Check(result && result->space_known && (result->parent_attributes & FILE_ATTRIBUTE_DIRECTORY),
            "actual Unicode long-path parent supplies disk space and attributes");
        result = Read(probe, 6, L"");
        Check(result && result->attributes == INVALID_FILE_ATTRIBUTES && result->error == ERROR_INVALID_NAME, "empty draft reports invalid name without I/O");
        Check(result && !result->space_known && result->free_bytes == 0 && result->parent_attributes == INVALID_FILE_ATTRIBUTES,
            "empty draft keeps advisory metadata unknown");
    }
    {
        const auto gate = std::make_shared<Gate>();
        Probe probe([gate](Probe::Result& result) { gate->Block(result); });
        Check(Wait([&] { return probe.Start(10, L"slow"); }) && Wait([&] { return gate->entered == 1; }), "injected slow request enters single worker");
        const auto begin = Clock::now();
        probe.Cancel();
        Check(Clock::now() - begin < std::chrono::milliseconds(250), "Cancel does not wait for slow provider");
        Check(!probe.Take(10, L"slow"), "cancelled request is not exposed");
        Check(probe.Start(11, L"superseded") && probe.Start(12, L"latest"), "queued request can be replaced while old I/O is blocked");
        gate->Release();
        std::optional<Probe::Result> result;
        Check(Wait([&] { result = probe.Take(12, L"latest"); return result.has_value(); }) && result->attributes == FILE_ATTRIBUTE_NORMAL,
            "latest generation/path receives result after old completion");
        {
            std::lock_guard lock(gate->mutex);
            Check(gate->paths == std::vector<std::wstring>{L"slow", L"latest"}, "single pending slot never runs superseded middle request");
        }
        Check(!probe.Take(10, L"slow") && !probe.Take(12, L"latest"), "stale result discarded and accepted result consumed once");
    }
    {
        const auto gate = std::make_shared<Gate>();
        Probe probe([gate](Probe::Result& result) { gate->Block(result); });
        Check(Wait([&] { return probe.Start(15, L"cancel-only"); }) && Wait([&] { return gate->entered == 1; }),
            "cancel-only fixture enters worker");
        probe.Cancel();
        gate->Release();
        Check(Wait([&] { return gate->returned == 1; }), "cancelled provider is allowed to finish after cancellation");
        bool surfaced = false;
        const auto until = Clock::now() + std::chrono::milliseconds(100);
        do { surfaced |= probe.Take(15, L"cancel-only").has_value(); Sleep(1); } while (Clock::now() < until);
        Check(!surfaced, "cancelled completion never surfaces even to its original generation/path");
    }
    {
        std::atomic<unsigned> calls = 0;
        Probe probe([&](Probe::Result& result) {
            Check((GetThreadErrorMode() & SEM_FAILCRITICALERRORS) != 0, "worker suppresses critical-error UI");
            result.attributes = FILE_ATTRIBUTE_NORMAL; ++calls;
        });
        Check(Wait([&] { return probe.Start(20, L"identity"); }) && Wait([&] { return calls == 1; }), "identity fixture probe completes");
        Sleep(10);
        Check(!probe.Take(19, L"identity"), "wrong generation is never accepted");
        Check(Read(probe, 21, L"identity").has_value(), "probe remains reusable after rejected stale lookup");
        Check(probe.Start(22, L"identity") && Wait([&] { return calls == 3; }), "path mismatch fixture completes");
        Sleep(10);
        Check(!probe.Take(22, L"different"), "wrong path is never accepted");
    }
    {
        const auto gate = std::make_shared<Gate>();
        std::vector<std::unique_ptr<Probe>> probes;
        for (int i = 0; i < 4; ++i) {
            auto probe = std::make_unique<Probe>([gate](Probe::Result& result) { gate->Block(result); });
            Check(Wait([&] { return probe->Start(30 + i, L"held"); }), "bounded worker slot acquired");
            probes.push_back(std::move(probe));
        }
        Check(Wait([&] { return gate->entered == 4; }), "four actual workers are blocked in provider");
        Probe extra([](Probe::Result& result) { result.attributes = FILE_ATTRIBUTE_NORMAL; });
        Check(!extra.Start(40, L"extra"), "fifth worker denied by process-wide resource budget");
        const auto begin = Clock::now();
        probes.clear();
        Check(Clock::now() - begin < std::chrono::milliseconds(250), "destructors detach rather than waiting for blocked I/O");
        Check(!extra.Start(41, L"extra"), "detached unfinished workers retain resource leases");
        gate->Release();
        Check(Wait([&] { return gate->returned == 4; }) && Wait([&] { return extra.Start(42, L"released"); }),
            "worker completion releases leases for a new probe");
        std::optional<Probe::Result> result;
        Check(Wait([&] { result = extra.Take(42, L"released"); return result.has_value(); }), "replacement probe works after detached workers exit");
    }
    {
        Probe probe([calls = 0](Probe::Result& result) mutable {
            if (++calls == 1) {
                result.space_known = true;
                result.free_bytes = 123;
                result.parent_attributes = FILE_ATTRIBUTE_DIRECTORY;
                throw 1;
            }
            result.attributes = FILE_ATTRIBUTE_NORMAL;
        });
        const auto result = Read(probe, 50, L"throws");
        Check(result && result->attributes == INVALID_FILE_ATTRIBUTES && result->error == ERROR_GEN_FAILURE, "provider exception yields error without terminating worker");
        Check(result && !result->space_known && result->free_bytes == 0 && result->parent_attributes == INVALID_FILE_ATTRIBUTES,
            "provider exception discards partially populated advisory metadata");
        const auto recovered = Read(probe, 51, L"after-throw");
        Check(recovered && recovered->error == ERROR_SUCCESS && recovered->attributes == FILE_ATTRIBUTE_NORMAL,
            "same worker successfully processes request after provider exception");
    }
    Check(SetFileAttributesW(readonly.c_str(), FILE_ATTRIBUTE_NORMAL) != FALSE, "restore isolated readonly fixture attribute");
    std::printf("RESULT failures=%d\n", failures.load());
    return failures.load() ? 1 : 0;
}

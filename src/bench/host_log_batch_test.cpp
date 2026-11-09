#define PULSE_HOST_LOG_BATCH_TEST 1
#include "../shell_host/host_log_batch.h"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <thread>
#include <atomic>
#include <sstream>

std::string Read(const std::wstring& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
int main() {
    using namespace pulse::shell;
    CreateDirectoryW(L"bench_data", nullptr);
    const auto root = L"bench_data\\host-log-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    if (!CreateDirectoryW(root.c_str(), nullptr)) return 2;
    bool ok = true;
    auto check = [&](bool pass, const char* name) { std::printf("[%s] %s\n", pass ? "PASS" : "FAIL", name); ok &= pass; };
    const auto run = [&](const wchar_t* name, size_t batch, LONGLONG limit, int count) {
        HostLogStats stats;
        const auto started = std::chrono::steady_clock::now();
        {
            HostLogBatch writer(root + L"\\" + name, &stats, batch, limit);
            for (int n = 0; n < count; ++n) {
                const auto line = L"phone ctx: native row " + std::to_wstring(n) + L" 中文 😀 &Open";
                writer.Add(line.c_str());
                if (batch == 1) writer.Flush();
            }
            check(writer.Flush(), "file batch flush succeeds");
        }
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
        std::printf("log_file=%ls batch=%zu rows=%d opens=%u writes=%u rotations=%u errors=%u elapsed_us=%lld peak_rows=%zu peak_bytes=%zu\n",
            name, batch, count, stats.opens, stats.writes, stats.rotations, stats.failures, us, stats.peak_rows, stats.peak_bytes);
        check(stats.peak_rows <= 64 && stats.peak_bytes <= 32768, "batch limits enforced");
        return stats;
    };
    const auto baseline = run(L"baseline.log", 1, 1024 * 1024, 1000);
    const auto batched = run(L"batched.log", 64, 1024 * 1024, 1000);
    check(Read(root + L"\\baseline.log") == Read(root + L"\\batched.log"), "UTF-8 row content and order unchanged");
    check(baseline.opens == 1000 && baseline.writes == 1000 && batched.opens == 16 && batched.writes == 16,
        "physical file operation count drops from one per line to one per bounded batch");
    run(L"rotate-baseline.log", 1, 4096, 1000);
    run(L"rotate-batched.log", 64, 4096, 1000);
    check(Read(root + L"\\rotate-baseline.log") == Read(root + L"\\rotate-batched.log") &&
        Read(root + L"\\rotate-baseline.log.1") == Read(root + L"\\rotate-batched.log.1"), "per-row rotation boundary preserves both generations");
    const auto close_path = root + L"\\close.log";
    { HostLogBatch writer(close_path); writer.Add(L"close flush"); }
    check(Read(close_path) == HostLogBatch::Encode(L"close flush"), "scope exit flushes pending lines");
    HostLogStats failed;
    HostLogBatch bad(root + L"\\missing\\bad.log", &failed);
    bad.Add(L"must report failure");
    check(!bad.Flush() && failed.failures > 0 && failed.error != ERROR_SUCCESS, "open failure reaches flush result and summary");
    const auto locked_path = root + L"\\locked.log";
    { HostLogBatch writer(locked_path); writer.Add(L"initial"); }
    HANDLE reader = CreateFileW(locked_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    HostLogStats locked_stats;
    {
        HostLogBatch writer(locked_path, &locked_stats, 64, 1);
        writer.Add(L"append after rotation error"); writer.Add(L"second retained row");
        check(reader != INVALID_HANDLE_VALUE && !writer.Flush() && locked_stats.rotations == 2,
            "failed rotation remains observable and retries at each row");
    }
    if (reader != INVALID_HANDLE_VALUE) CloseHandle(reader);
    check(Read(locked_path) == HostLogBatch::Encode(L"initial") +
        HostLogBatch::Encode(L"append after rotation error") + HostLogBatch::Encode(L"second retained row"),
        "rotation failure retains every row by appending");
    const auto allocation_path = root + L"\\allocation-failure.log";
    {
        HostLogBatch writer(allocation_path);
        writer.Add(L"retry after allocation failure");
        HostLogBatch::fail_after_open_for_test = true;
        bool threw = false;
        try { writer.Flush(); } catch (const std::bad_alloc&) { threw = true; }
        check(threw && DeleteFileW(allocation_path.c_str()) != FALSE,
            "allocation exception after open releases handle so file can be deleted");
        check(writer.Flush(), "one-shot allocation fault permits reopening and flushing");
    }
    check(Read(allocation_path) == HostLogBatch::Encode(L"retry after allocation failure"),
        "allocation retry preserves pending row");
    const auto committed_path = root + L"\\committed-failure.log";
    {
        HostLogBatch writer(committed_path, nullptr, 64, 1);
        writer.Add(L"committed first row"); writer.Add(L"pending second row");
        HostLogBatch::fail_after_committed_block_for_test = true;
        bool threw = false;
        try { writer.Flush(); } catch (const std::bad_alloc&) { threw = true; }
        check(threw && writer.Flush(), "retry after committed block allocation fault succeeds");
    }
    check(Read(committed_path + L".1") + Read(committed_path) ==
        HostLogBatch::Encode(L"committed first row") + HostLogBatch::Encode(L"pending second row"),
        "retry across rotation writes each committed and pending row exactly once");
    const auto concurrent_path = root + L"\\concurrent.log";
    std::mutex shared_mutex;
    std::atomic<int> ready{0};
    std::atomic_bool start{false}, concurrent_ok{true};
    auto producer = [&](int id, size_t batch) {
        HostLogBatch writer(concurrent_path, nullptr, batch, 1024 * 1024, &shared_mutex);
        ++ready;
        while (!start.load()) std::this_thread::yield();
        for (int n = 0; n < 200; ++n) {
            const auto text = L"writer " + std::to_wstring(id) + L" row " + std::to_wstring(n) + L" 中文";
            if (!writer.Add(text.c_str())) concurrent_ok = false;
            if (batch == 1 && !writer.Flush()) concurrent_ok = false;
        }
        if (!writer.Flush()) concurrent_ok = false;
    };
    std::thread immediate(producer, 0, 1), batched_writer(producer, 1, 64);
    while (ready.load() != 2) std::this_thread::yield();
    start = true;
    immediate.join(); batched_writer.join();
    std::vector<std::string> actual_rows, expected_rows;
    std::istringstream input(Read(concurrent_path));
    for (std::string line; std::getline(input, line);) actual_rows.push_back(std::move(line));
    for (int id = 0; id < 2; ++id) for (int n = 0; n < 200; ++n) {
        const auto text = L"writer " + std::to_wstring(id) + L" row " + std::to_wstring(n) + L" 中文";
        auto encoded = HostLogBatch::Encode(text.c_str()); encoded.pop_back();
        expected_rows.push_back(std::move(encoded));
    }
    std::sort(actual_rows.begin(), actual_rows.end());
    std::sort(expected_rows.begin(), expected_rows.end());
    check(concurrent_ok && actual_rows == expected_rows,
        "shared mutex preserves all 400 concurrent immediate and batched rows without interleaving");
    std::wstring boundary(2047, L'x'); boundary += L'\xD83D'; boundary += L'\xDE00';
    check(HostLogBatch::Encode(boundary.c_str()) == HostLogBatch::Encode(std::wstring(2047, L'x').c_str()),
        "bounded UTF-16 entry never splits a surrogate pair");
    std::printf("artifact_directory=%ls\n", root.c_str());
    return ok ? 0 : 1;
}

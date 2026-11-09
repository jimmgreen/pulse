#include "../ops/recycle_empty.h"
#include "../app/folder_statistics.h"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <cstdio>
#include <algorithm>

using Clock = std::chrono::steady_clock;
double Ms(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
int main() {
    namespace fs = std::filesystem;
    const auto root = fs::current_path() / "bench_data" / ("reuse-perf-" + std::to_string(GetCurrentProcessId()));
    if (fs::exists(root)) return 2;
    fs::create_directories(root);
    int failures = 0;
    for (int count : {1, 64, 512}) {
        std::vector<double> samples;
        for (int run = 0; run < 7; ++run) {
            const auto bin = root / ("bin-" + std::to_string(count) + "-" + std::to_string(run));
            fs::create_directories(bin / "$Rfixture");
            std::ofstream(bin / "$Ifixture") << "fixture";
            for (int i = 0; i < count; ++i) std::ofstream(bin / "$Rfixture" / std::to_string(i)) << "payload";
            const auto start = Clock::now();
            const auto result = pulse::recycle_empty::EmptyFolders({bin.wstring()}, {}, {}, 4);
            samples.push_back(Ms(start));
            failures += result.deleted_files != static_cast<uint64_t>(count) || result.failed_entries || !fs::is_empty(bin);
        }
        std::sort(samples.begin(), samples.end());
        std::printf("delete files=%d runs=7 min_ms=%.3f median_ms=%.3f max_ms=%.3f\n", count, samples.front(), samples[3], samples.back());
    }
    const auto a = root / "A", b = root / "B";
    fs::create_directories(a); fs::create_directories(b);
    for (int i = 0; i < 3000; ++i) std::ofstream(a / std::to_string(i)) << "payload";
    std::ofstream(b / "file") << "payload";
    pulse::app::FolderStatisticsService service;
    auto read = [&](const fs::path& path) {
        const auto until = GetTickCount64() + 15000;
        auto stop = [until] { return GetTickCount64() >= until; };
        auto value = service.Read(path.wstring(), stop);
        while (!value.verified && !stop()) value = service.Read(path.wstring(), stop, value.version);
        if (!value.verified) ++failures;
        return value;
    };
    auto start = Clock::now();
    auto first = read(a);
    std::printf("statistics cold files=%llu ms=%.3f scans=%llu\n", first.files, Ms(start), service.ScansStarted());
    read(b);
    const auto before = service.ScansStarted();
    start = Clock::now();
    for (int i = 0; i < 100; ++i) { read(a); read(b); }
    std::printf("statistics 200_switches ms=%.3f extra_scans=%llu\n", Ms(start), service.ScansStarted() - before);
    pulse::app::FolderStatisticsService shared;
    pulse::app::FolderStatistics left, right;
    std::atomic_bool go{false};
    const auto until = GetTickCount64() + 15000;
    auto concurrent = [&](auto& output) { while (!go) std::this_thread::yield(); output = shared.Read(a.wstring(), [until] { return GetTickCount64() >= until; }); };
    std::thread one([&] { concurrent(left); }), two([&] { concurrent(right); });
    start = Clock::now(); go = true; one.join(); two.join();
    std::printf("statistics two_subscribers ms=%.3f scans=%llu files=%llu/%llu\n", Ms(start), shared.ScansStarted(), left.files, right.files);
    // Watcher arming may invalidate the initial scan; report that work rather
    // than treating a legitimate revision change as a failed shared request.
    failures += left.files != 3000 || right.files != 3000;
    pulse::app::FolderStatisticsService independent_one, independent_two;
    start = Clock::now();
    auto l = independent_one.Read(a.wstring(), [until] { return GetTickCount64() >= until; });
    auto r = independent_two.Read(a.wstring(), [until] { return GetTickCount64() >= until; });
    std::printf("statistics independent_services ms=%.3f scans=%llu files=%llu/%llu\n", Ms(start),
        independent_one.ScansStarted() + independent_two.ScansStarted(), l.files, r.files);
    failures += l.files != 3000 || r.files != 3000;
    std::printf("failures=%d fixture=%ls\n", failures, root.c_str());
    return failures ? 1 : 0;
}

#include "../ops/recycle_empty.h"
#include <filesystem>
#include <fstream>
#include <cstdio>

int main() {
    namespace fs = std::filesystem;
    const auto root = fs::current_path() / "bench_data" /
        ("recycle-batches-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    fs::create_directories(root);
    size_t baseline_growths = 0;
    int failures = 0;
    for (bool reuse : {false, true}) {
        const auto bin = root / (reuse ? "after0" : "before");
        fs::create_directories(bin);
        for (int item = 0; item < 6; ++item) {
            auto payload = bin / ("$R" + std::to_string(item));
            fs::create_directories(payload);
            std::ofstream(bin / ("$I" + std::to_string(item))) << "fixture";
            for (int i = 0; i < 4096; ++i) std::ofstream(payload / std::to_string(i)) << "x";
        }
        pulse::recycle_empty::detail::reuse_scratch = reuse;
        pulse::recycle_empty::detail::file_capacity_growths = 0;
        pulse::worker_starts = 0;
        pulse::worker_start_ns = 0;
        const auto start = std::chrono::steady_clock::now();
        const auto result = pulse::recycle_empty::EmptyFolders({bin.wstring()}, {}, {}, 4);
        const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        const auto growths = pulse::recycle_empty::detail::file_capacity_growths;
        const bool ok = result.deleted_files == 24576 && !result.failed_entries && !result.cancelled && fs::is_empty(bin);
        std::printf("[%s] reuse=%d files=%llu elapsed_ms=%.3f growths=%zu thread_starts=%llu start_ms=%.3f\n",
            ok ? "PASS" : "FAIL", reuse, result.deleted_files, elapsed, growths,
            pulse::worker_starts.load(), pulse::worker_start_ns.load() / 1e6);
        if (!ok) ++failures;
        if (!reuse) baseline_growths = growths;
        else if (growths >= baseline_growths) ++failures;
    }
    pulse::recycle_empty::detail::reuse_scratch = true;
    std::printf("failures=%d\n", failures);
    // Only this test's unique fixture root is removed.
    fs::remove_all(root);
    return failures ? 1 : 0;
}

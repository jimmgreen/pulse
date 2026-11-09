#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>

namespace {
unsigned work_attempts = 0, work_created = 0, work_closed = 0;
bool fail_work_creation = false;
PTP_WORK WINAPI CountCreate(PTP_WORK_CALLBACK callback, PVOID context, PTP_CALLBACK_ENVIRON environment) {
    ++work_attempts;
    if (fail_work_creation) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
    auto work = CreateThreadpoolWork(callback, context, environment);
    work_created += work != nullptr;
    return work;
}
void WINAPI CountClose(PTP_WORK work) { ++work_closed; CloseThreadpoolWork(work); }
}
#define CreateThreadpoolWork CountCreate
#define CloseThreadpoolWork CountClose
#if __has_include("../../build/native_delete_stream_baseline.h")
#define PULSE_HAS_DELETE_WORK_BASELINE
#define native_delete baseline_delete
#include "../../build/native_delete_stream_baseline.h"
#undef native_delete
#endif
#include "../ops/native_delete_stream.h"
#undef CreateThreadpoolWork
#undef CloseThreadpoolWork
#ifndef PULSE_HAS_DELETE_WORK_BASELINE
namespace pulse::ops { namespace baseline_delete = native_delete; }
#endif

namespace fs = std::filesystem;
namespace {
struct Shape { const char* name; unsigned folders, files_per_folder; };
void Fixture(const fs::path& root, const Shape& shape) {
    fs::create_directories(root);
    for (unsigned directory = 0; directory < shape.folders; ++directory) {
        const auto target = shape.folders == 1 ? root : root / std::to_wstring(directory);
        fs::create_directories(target);
        for (unsigned i = 0; i < shape.files_per_folder; ++i) {
            const auto path = target / std::to_wstring(i);
            HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("fixture creation failed");
            CloseHandle(file);
        }
    }
}
void ResetCounters() { work_attempts = work_created = work_closed = 0; }
}
int main(int argc, char** argv) {
    const bool baseline_only = argc > 1 && !std::strcmp(argv[1], "--baseline-only");
    const bool verify_only = argc > 1 && !std::strcmp(argv[1], "--verify-only");
#ifndef PULSE_HAS_DELETE_WORK_BASELINE
    if (!verify_only) {
        std::puts("Baseline snapshot missing: save the pre-change header as build/native_delete_stream_baseline.h, or use --verify-only.");
        return 2;
    }
#endif
    const auto root = fs::absolute(L"bench_data") / (L"delete_work_reuse_" +
        std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
    if (fs::exists(root) || !fs::create_directories(root)) return 2;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); failures += !ok;
    };
    std::atomic_bool cancel{false};
    try {
        if (!verify_only) {
            const Shape shapes[] = {{"single", 1, 1}, {"flat4096", 1, 4096},
                {"dirs128x4", 128, 4}, {"dirs128x1", 128, 1}};
            for (unsigned round = 0; round < (baseline_only ? 1u : 3u); ++round) {
                for (const auto& shape : shapes) {
                    for (unsigned pass = 0; pass < (baseline_only ? 1u : 2u); ++pass) {
                        const bool baseline = baseline_only || ((pass + round) % 2 == 0);
                        const auto tree = root / L"fixture";
                        Fixture(tree, shape);
                        ResetCounters();
                        const auto start = std::chrono::steady_clock::now();
                        bool done = false;
                        if (baseline) done = pulse::ops::baseline_delete::DeleteNative(tree.wstring(), cancel, [](const auto&, float) {}, 4).done;
                        else done = pulse::ops::native_delete::DeleteNative(tree.wstring(), cancel, [](const auto&, float) {}, 4).done;
                        const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - start).count();
                        std::printf("BENCH variant=%s round=%u shape=%s work_attempts=%u work_created=%u work_closed=%u delete_us=%lld done=%d\n",
                            baseline ? "baseline" : "current", round + 1, shape.name,
                            work_attempts, work_created, work_closed, micros, done);
                        if (!done || fs::exists(tree) || work_created != work_closed) ++failures;
                    }
                }
            }
        }
        if (!baseline_only) {
            const auto tree = root / L"verify";
            Fixture(tree, {"failure", 1, 96});
            ResetCounters(); fail_work_creation = true;
            const auto fallback = pulse::ops::native_delete::DeleteNative(tree.wstring(), cancel, [](const auto&, float) {}, 4);
            fail_work_creation = false;
            check(fallback.done && work_attempts == 1 && work_created == 0 && !fs::exists(tree),
                "work creation failure falls back synchronously without retrying each batch");
            Fixture(tree, {"throw", 1, 96});
            ResetCounters();
            bool threw = false;
            try {
                pulse::ops::native_delete::DeleteNative(tree.wstring(), cancel,
                    [](const auto&, float) { throw std::runtime_error("fixture progress failure"); }, 4);
            } catch (const std::runtime_error&) { threw = true; }
            check(threw && work_created == 1 && work_closed == 1,
                "progress exception closes persistent work after all callbacks finish");
            pulse::ops::native_delete::DeleteNative(tree.wstring(), cancel, [](const auto&, float) {});
            Fixture(tree, {"cancel", 1, 96});
            ResetCounters();
            const auto stopped = pulse::ops::native_delete::DeleteNative(tree.wstring(), cancel,
                [&](const auto&, float) { cancel = true; }, 4);
            check(stopped.cancelled && stopped.mutated && !stopped.done && work_created == 1 && work_closed == 1,
                "cancellation preserves partial outcome and closes reusable work");
            cancel = false;
            pulse::ops::native_delete::DeleteNative(tree.wstring(), cancel, [](const auto&, float) {});
            Fixture(tree, {"single", 1, 1});
            ResetCounters();
            check(pulse::ops::native_delete::DeleteNative(tree.wstring(), cancel, [](const auto&, float) {}, 4).done && work_attempts == 0,
                "single ready item never allocates threadpool work");
            Fixture(tree, {"mixed", 1, 4});
            Fixture(tree / L"child", {"single", 1, 1});
            ResetCounters();
            check(pulse::ops::native_delete::DeleteNative(tree.wstring(), cancel, [](const auto&, float) {}, 4).done &&
                work_created == 1 && work_closed == 1,
                "reusable work remains safe across parallel and singleton drains");
        }
    } catch (const std::exception& error) { std::printf("[FAIL] %s\n", error.what()); ++failures; }
    // Only this unique process-created fixture tree is cleaned up.
    cancel = false;
    const auto cleaned = pulse::ops::native_delete::DeleteNative(root.wstring(), cancel, [](const auto&, float) {});
    check(cleaned.done && !fs::exists(root), "isolated benchmark fixture cleanup");
    return failures ? 1 : 0;
}

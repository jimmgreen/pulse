#include <pulse/worker.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace pulse::worker;

int g_passed = 0;
int g_failed = 0;

void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    std::fflush(stdout);
    if (ok) ++g_passed;
    else ++g_failed;
}

uint32_t PULSE_CALL ReadCancelFlag(void* ctx) noexcept {
    const auto* flag = static_cast<const std::atomic<bool>*>(ctx);
    return (flag && flag->load(std::memory_order_acquire)) ? 1u : 0u;
}

} // namespace

int main() {
    const std::wstring worker_exe = ResolveSiblingExecutable(L"pulse_sdk_worker_echo_helper.exe");
    const DWORD attrs = GetFileAttributesW(worker_exe.c_str());
    Check(!worker_exe.empty() && attrs != INVALID_FILE_ATTRIBUTES,
          "ResolveSiblingExecutable finds companion worker EXE");

    // 1. Missing executable fails closed with SpawnFailed.
    {
        WorkerProcess proc;
        WorkerError err;
        const WorkerStatus st = proc.Start(worker_exe + L".missing.exe", {}, {}, &err);
        Check(st == WorkerStatus::SpawnFailed && err.status == WorkerStatus::SpawnFailed,
              "Missing worker executable returns WorkerStatus::SpawnFailed");
    }

    // 2. Persistent multi-frame session + progress frames + child-process restriction + clean Shutdown.
    {
        WorkerProcess proc;
        WorkerLimits limits;
        limits.memory_limit_bytes = 128u * 1024u * 1024u;
        limits.timeout_ms = 5000u;
        WorkerError err;
        Check(proc.Start(worker_exe, {}, limits, &err) == WorkerStatus::Ok,
              "Start governed persistent worker session");
        Check(proc.IsRunning(), "Worker process is running after Start");

        for (uint32_t tag = 1; tag <= 3; ++tag) {
            std::vector<std::string> progress;
            std::string reply;
            const std::string payload = "echo:hello-" + std::to_string(tag);
            const WorkerStatus st = proc.Call(
                tag,
                payload,
                reply,
                [&](uint32_t ptag, std::string_view pdata) {
                    progress.push_back(std::to_string(ptag) + ":" + std::string(pdata));
                },
                {},
                0,
                &err);
            Check(st == WorkerStatus::Ok &&
                  reply == ("hello-" + std::to_string(tag)) &&
                  progress.size() == 2 &&
                  progress[0] == "1:p1" &&
                  progress[1] == "2:p2",
                  "Persistent Call receives ordered Progress frames + matching Reply");
        }

        // Child process policy + ACTIVE_PROCESS=1 restriction.
        {
            std::string reply;
            const WorkerStatus st = proc.Call(4u, "spawn_child", reply, {}, {}, 0, &err);
            Check(st == WorkerStatus::Ok && reply == "blocked",
                  "CHILD_PROCESS_POLICY + ACTIVE_PROCESS=1 blocks child process spawning");
        }

        // Structured Error frame propagation.
        {
            std::string reply;
            const WorkerStatus st = proc.Call(5u, "error:bad_archive", reply, {}, {}, 0, &err);
            Check(st == WorkerStatus::IoError && err.message == "bad_archive",
                  "Error frame propagates status IoError + diagnostic message");
        }

        Check(proc.Shutdown(1000u) == WorkerStatus::Ok, "Clean Shutdown waits for exit");
        Check(!proc.IsRunning(), "Worker process no longer running after Shutdown");
    }

    // 3. Cooperative cancellation via PulseTaskContext terminates hung worker quickly.
    {
        WorkerProcess proc;
        WorkerLimits limits;
        limits.timeout_ms = 10000u;
        limits.cancel_poll_ms = 10u;
        Check(proc.Start(worker_exe, {}, limits) == WorkerStatus::Ok, "Start worker for cancellation test");

        std::atomic<bool> cancel_flag{false};
        PulseTaskContext task_ctx{};
        task_ctx.id = 77;
        task_ctx.context = &cancel_flag;
        task_ctx.cancelled = &ReadCancelFlag;

        std::thread cancel_thread([&] {
            Sleep(50);
            cancel_flag.store(true, std::memory_order_release);
        });

        const auto t0 = std::chrono::steady_clock::now();
        std::string reply;
        WorkerError err;
        const WorkerStatus st = proc.Call(10u, "hang", reply, &task_ctx, {}, 0, &err);
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        cancel_thread.join();

        Check(st == WorkerStatus::Cancelled && elapsed_ms < 2000 && !proc.IsRunning(),
              "PulseTaskContext cancellation aborts hung worker and kills job");
    }

    // 4. Timeout enforcement terminates hung worker quickly.
    {
        WorkerProcess proc;
        WorkerLimits limits;
        limits.timeout_ms = 120u;
        limits.cancel_poll_ms = 10u;
        Check(proc.Start(worker_exe, {}, limits) == WorkerStatus::Ok, "Start worker for timeout test");

        const auto t0 = std::chrono::steady_clock::now();
        std::string reply;
        WorkerError err;
        const WorkerStatus st = proc.Call(11u, "hang", reply, {}, {}, 0, &err);
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

        Check(st == WorkerStatus::TimedOut && elapsed_ms < 2000 && !proc.IsRunning(),
              "Timeout terminates hung worker within deadline");
    }

    // 5. Unexpected crash / non-zero exit code detected without hanging.
    {
        std::string reply;
        WorkerError err;
        const WorkerStatus st = WorkerProcess::RunOneShot(worker_exe, "crash", reply, {}, {}, {}, &err);
        Check(st == WorkerStatus::WorkerExited && err.exit_code == 42u,
              "Crash exit detected promptly with exact exit code 42");
    }

    // 6. Oversized wire frame rejected cleanly with FrameTooLarge.
    {
        WorkerProcess proc;
        WorkerLimits limits;
        limits.max_frame_bytes = 1024u;
        limits.timeout_ms = 5000u;
        Check(proc.Start(worker_exe, {}, limits) == WorkerStatus::Ok, "Start worker for oversize frame test");

        std::string reply;
        WorkerError err;
        const WorkerStatus st = proc.Call(12u, "oversize:4096", reply, {}, {}, 0, &err);
        Check(st == WorkerStatus::FrameTooLarge, "Oversized reply rejected with WorkerStatus::FrameTooLarge");
    }

    std::printf("Summary: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

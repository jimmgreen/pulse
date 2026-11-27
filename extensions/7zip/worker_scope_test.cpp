// Real worker IPC coverage for picker-local executable selection.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "worker_client.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

namespace fs = std::filesystem;

namespace {
int failures = 0;
void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("[%s] %s%s%s\n", ok ? "PASS" : "FAIL", name,
                detail.empty() ? "" : " -- ", detail.c_str());
    if (!ok) ++failures;
}

// Compile against the untouched client to demonstrate pre-change behavior.
#ifdef PULSE_WORKER_SCOPE_BASELINE
struct Scope { explicit Scope(const std::wstring&) {} };
#else
using Scope = worker_client::ScopedWorkerExecutable;
#endif

int Run(const fs::path& worker, const fs::path& root) {
    fs::create_directories(root);
    const auto source = root / L"scope.txt";
    { std::ofstream out(source); out << "picker scoped worker\n"; }
    const auto archive = root / L"scope.zip";
    fs::remove(archive);
    engine::CreateOptions options;
    options.format = engine::Format::Zip;
    const auto created = engine::Create({source.wstring()}, root.wstring(),
                                       L"scope.zip", options, {}, {});
    Check(created.status == engine::Status::Ok && created.files == 1,
          "default worker creates real ZIP", created.error);
    const auto list = [&] { return engine::List(archive.wstring(), {}, {}); };
    const auto good = [&](const char* label) {
        const auto result = list();
        Check(result.status == engine::Status::Ok && result.entries.size() == 1 &&
              result.entries[0].path == L"scope.txt", label, result.error);
    };
    const auto bad = [&](const char* label) {
        const auto result = list();
        Check(result.status == engine::Status::WorkerCrashed && result.entries.empty() &&
              result.error.find("not found") != std::string::npos, label, result.error);
    };
    good("default List opens real ZIP");
    const auto warm_pid = worker_client::WarmWorkerPidForTests();
    Check(warm_pid != 0 && warm_pid != GetCurrentProcessId(), "default worker has separate warm PID");
    Check(!engine::SupportedFormats().empty(), "default formats cache is primed");
    const auto missing = root / L"missing" / L"pulse_7zip_worker.exe";
    {
        Scope scope(missing.wstring());
        bad("missing explicit worker fails without warm fallback");
        Check(engine::SupportedFormats().empty(), "missing explicit worker does not reuse cached formats");
        {
            Scope nested(worker.wstring());
            good("valid nested explicit worker lists real ZIP");
            Check(worker_client::WarmWorkerPidForTests() == warm_pid,
                  "scoped List leaves default warm PID untouched");
        }
        bad("nested scope restores missing outer worker");
        engine::Listing other_thread;
        std::thread thread([&] { other_thread = list(); });
        thread.join();
        Check(other_thread.status == engine::Status::Ok && other_thread.entries.size() == 1,
              "another thread retains default worker while caller scope is invalid", other_thread.error);
    }
    good("outer scope restores default worker");
    Check(worker_client::WarmWorkerPidForTests() == warm_pid, "default worker remains reusable after scope");
    {
        Scope scope(worker.wstring());
        size_t cancellation_checks = 0;
        engine::Listing concurrent;
        const auto result = engine::List(archive.wstring(), [&] {
            if (++cancellation_checks == 2) {
                std::thread thread([&] { concurrent = list(); });
                thread.join();
            }
            return false;
        }, {});
        Check(cancellation_checks >= 2 && concurrent.status == engine::Status::Ok &&
              result.status == engine::Status::Ok && result.entries.size() == 1,
              "default preview generation does not cancel in-flight scoped List", result.error);
        const auto cancelled = engine::List(archive.wstring(), [] { return true; }, {});
        Check(cancelled.status == engine::Status::Cancelled, "scoped List honors caller cancellation");
    }
    const std::wstring invalid_paths[] = {
        L"pulse_7zip_worker.exe", L"C:pulse_7zip_worker.exe", L"\\pulse_7zip_worker.exe",
        (worker.parent_path() / L"other.exe").wstring(), L"",
        worker.wstring() + std::wstring(1, L'\0') + L"tail"
    };
    for (const auto& path : invalid_paths) {
        Scope invalid(path);
        bad("invalid explicit path fails closed");
    }
    engine::ShutdownWorkers();
    {
        Scope scope(worker.wstring());
        good("explicit worker lists ZIP with empty default pool");
        Check(worker_client::WarmWorkerPidForTests() == 0, "scoped worker does not populate default pool");
        Check(!engine::SupportedFormats().empty(), "explicit worker enumerates real formats");
        Check(worker_client::WarmWorkerPidForTests() == 0, "scoped formats do not populate default pool");
    }
    good("default worker recovers after scoped sessions are destroyed");
    Check(GetModuleHandleW(L"7z.dll") == nullptr, "host never loads 7z.dll");
    engine::ShutdownWorkers();
    Check(worker_client::WarmWorkerPidForTests() == 0, "all default workers are reaped");
    std::printf("worker_scope_test: %d failed\n", failures);
    return failures ? 1 : 0;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc != 3) return 2;
    try { return Run(fs::path(argv[1]), fs::path(argv[2])); }
    catch (const std::exception& ex) {
        std::printf("[FAIL] exception: %s\n", ex.what());
        engine::ShutdownWorkers();
        return 2;
    }
}

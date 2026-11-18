// pulse_ops_bench: drives OpsManager the way the app does and reports items or
// bytes per second for copy and delete workloads. Synthetic files only, under
// --root (default: <exe dir>\opsbench); everything it creates is removed again.
//
//   pulse_ops_bench [--root DIR] [--files N] [--kb K] [--repeat R]
//                   [--only copy,delete-multi,delete-tree,large,cross,recycle]
//                   [--cross DIR] [--large-mb M] [--recycle-files N]
//                   [--diagnostics-root DIR outside --root] [--verify]
//
// "recycle" is opt-in: it sends a few files to the Recycle Bin, then undoes the
// operation so the bin is left as it was. Machines with on-access scanners give
// low absolute numbers; compare runs on the same machine.
#include "../ops/ops_manager.h"
#include "../common/localization.h"
#include "../common/runtime_log.h"
#include <windows.h>
#include <objbase.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace pulse;
namespace fsys = std::filesystem;

namespace {
ops::OpsManager g_ops;
bool g_failed = false;
std::atomic<uint64_t> g_notifications{0};
std::mutex g_ui_mutex;
std::condition_variable g_ui_cv;
bool g_ui_pending = false;
bool g_ui_stop = false;

// Stands in for the UI thread: a notification is a posted message the UI
// handles later by reading Status() and draining finished tasks.
void UiThread() {
    for (;;) {
        std::unique_lock lock(g_ui_mutex);
        g_ui_cv.wait(lock, [] { return g_ui_pending || g_ui_stop; });
        if (g_ui_stop) return;
        g_ui_pending = false;
        lock.unlock();
        (void)g_ops.Status();
    }
}

void Notify() {
    ++g_notifications;
    std::lock_guard lock(g_ui_mutex);
    g_ui_pending = true;
    g_ui_cv.notify_one();
}

double Seconds(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
}

bool WriteFixture(const std::wstring& path, const std::vector<char>& data) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = data.empty() || WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
    CloseHandle(file);
    return ok != FALSE;
}

// N files spread over ten folders, as a copied project tree would be.
std::vector<std::wstring> MakeTree(const fsys::path& root, int files, int kb, bool flat) {
    std::vector<char> data(static_cast<size_t>(kb) * 1024);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<char>('a' + i % 26);
    std::vector<std::wstring> made;
    made.reserve(static_cast<size_t>(files));
    fsys::create_directories(root);
    for (int i = 0; i < files; ++i) {
        fsys::path folder = flat ? root : root / (L"d" + std::to_wstring(i % 10));
        if (!flat && i < 10) fsys::create_directories(folder);
        const auto path = (folder / (L"f" + std::to_wstring(i) + L".bin")).wstring();
        if (!WriteFixture(path, data)) {
            fprintf(stderr, "[bench] could not create %ls (%lu)\n", path.c_str(), GetLastError());
            break;
        }
        made.push_back(path);
    }
    return made;
}

void RemoveTree(const fsys::path& root) {
    std::error_code ignored;
    fsys::remove_all(root, ignored);
}

struct RunResult {
    double seconds = 0.0;
    ops::OpStatus status;
    uint64_t notifications = 0;
    bool finished = false;
};

RunResult Run(ops::OpRequest request) {
    g_ops.DrainFinishedTasks();
    const uint64_t before = g_notifications.load();
    const auto started = std::chrono::steady_clock::now();
    const uint64_t task = g_ops.Submit(std::move(request));
    RunResult result;
    uint64_t polls = 0;
    while (task != 0 && Seconds(started) < 1800.0) {
        for (const auto& finished : g_ops.DrainFinishedTasks())
            if (finished.task_id == task) result.finished = true;
        if (result.finished) break;
        Sleep(1);
        if (++polls % 500 == 0) {
            const auto now = g_ops.Status();
            fprintf(stderr, "[bench] t=%.0fs phase=%d auth=%d done=%llu/%llu item=%ls summary=%ls\n",
                    Seconds(started), static_cast<int>(now.phase), static_cast<int>(now.authorization),
                    static_cast<unsigned long long>(now.completed_items),
                    static_cast<unsigned long long>(now.total_items), now.current_item.c_str(),
                    now.summary.c_str());
        }
    }
    result.seconds = Seconds(started);
    result.status = g_ops.Status();
    result.notifications = g_notifications.load() - before;
    return result;
}

void Report(const char* name, const RunResult& run, double units, const char* unit) {
    const bool ok = run.finished && run.status.phase == ops::OpPhase::Completed && run.status.failed_items == 0;
    if (!ok) g_failed = true;
    printf("%-14s %s %8.0f %s in %7.3f s = %9.1f %s/s  notify=%llu%s%ls\n", name, ok ? "ok  " : "FAIL",
           units, unit, run.seconds, run.seconds > 0 ? units / run.seconds : 0.0, unit,
           static_cast<unsigned long long>(run.notifications), ok ? "" : "  error=",
           ok ? L"" : run.status.last_error.c_str());
}

std::wstring Arg(int argc, wchar_t** argv, const wchar_t* name, const std::wstring& fallback) {
    for (int i = 1; i + 1 < argc; ++i)
        if (_wcsicmp(argv[i], name) == 0) return argv[i + 1];
    return fallback;
}

bool Wants(const std::wstring& only, const wchar_t* scenario) {
    if (only.empty()) return wcscmp(scenario, L"recycle") != 0 && wcscmp(scenario, L"cross") != 0 &&
                             wcscmp(scenario, L"large") != 0 && wcscmp(scenario, L"move-cross") != 0 &&
                             wcscmp(scenario, L"large-move-cross") != 0;
    const std::wstring needle = L"," + std::wstring(scenario) + L",";
    return (L"," + only + L",").find(needle) != std::wstring::npos;
}
}

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    l10n::SetLanguage(L"en-US");
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const fsys::path root = Arg(argc, argv, L"--root", (fsys::path(exe).parent_path() / L"opsbench").wstring());
    const int files = _wtoi(Arg(argc, argv, L"--files", L"2000").c_str());
    const int kb = _wtoi(Arg(argc, argv, L"--kb", L"4").c_str());
    const int repeat = (std::max)(1, _wtoi(Arg(argc, argv, L"--repeat", L"1").c_str()));
    const std::wstring only = Arg(argc, argv, L"--only", L"");
    const std::wstring cross = Arg(argc, argv, L"--cross", L"");
    const int large_mb = _wtoi(Arg(argc, argv, L"--large-mb", L"1024").c_str());
    const int recycle_files = _wtoi(Arg(argc, argv, L"--recycle-files", L"30").c_str());
    if (files <= 0 || kb < 0) return 2;
    const std::wstring diagnostics_root = Arg(argc, argv, L"--diagnostics-root", L"");
    const bool diagnostics_enabled = !diagnostics_root.empty();
    if (diagnostics_enabled) {
        const auto fixture = fsys::weakly_canonical(root);
        const auto logs = fsys::weakly_canonical(diagnostics_root);
        const auto relative = logs.lexically_relative(fixture);
        if (relative.empty() || (relative.begin() != relative.end() && *relative.begin() != L"..")) {
            fprintf(stderr, "[bench] diagnostics root must be outside fixture root\n");
            CoUninitialize();
            return 2;
        }
        if (!diagnostics::runtime::Initialize(logs.wstring(), "ops_bench")) {
            fprintf(stderr, "[bench] diagnostic initialization failed\n");
            diagnostics::runtime::Shutdown();
            CoUninitialize();
            return 2;
        }
    }
    for (int i = 1; i < argc; ++i)
        if (_wcsicmp(argv[i], L"--verify") == 0) g_ops.SetVerifyCopies(true);
    RemoveTree(root);
    fsys::create_directories(root);
    std::thread ui(UiThread);
    g_ops.Start(Notify, true);  // deletes need the shell client, as in the app
    printf("pulse_ops_bench root=%ls files=%d kb=%d repeat=%d\n", root.c_str(), files, kb, repeat);

    for (int pass = 0; pass < repeat; ++pass) {
        if (Wants(only, L"copy")) {
            const auto source = root / L"copy_src";
            const auto target = root / L"copy_dst";
            const auto made = MakeTree(source, files, kb, false);
            if (made.size() != static_cast<size_t>(files)) g_failed = true;
            fsys::create_directories(target);
            ops::OpRequest request;
            request.type = ops::OpType::Copy;
            request.sources = {source.wstring()};
            request.dest_dir = target.wstring();
            Report("copy", Run(request), static_cast<double>(made.size()), "files");
            RemoveTree(target);
            if (Wants(only, L"cross") && !cross.empty()) {
                const auto remote_dir = fsys::path(cross) / L"pulse_opsbench_cross";
                RemoveTree(remote_dir);
                fsys::create_directories(remote_dir);
                request.dest_dir = remote_dir.wstring();
                Report("copy-cross", Run(request), static_cast<double>(made.size()), "files");
                RemoveTree(remote_dir);
            }
            RemoveTree(source);
        }
        // Move to another volume: copy, then the source is deleted.
        if (Wants(only, L"move-cross") && !cross.empty()) {
            const auto source = root / L"move_src";
            const auto made = MakeTree(source, files, kb, false);
            if (made.size() != static_cast<size_t>(files)) g_failed = true;
            const auto remote_dir = fsys::path(cross) / L"pulse_opsbench_cross";
            RemoveTree(remote_dir);
            fsys::create_directories(remote_dir);
            ops::OpRequest request;
            request.type = ops::OpType::Move;
            request.sources = {source.wstring()};
            request.dest_dir = remote_dir.wstring();
            if (!made.empty()) SetFileAttributesW(made.front().c_str(), FILE_ATTRIBUTE_READONLY);
            Report("move-cross", Run(request), static_cast<double>(made.size()), "files");
            {
                // Every file arrived whole, the read-only one stays read-only,
                // and nothing is left at the source.
                size_t arrived = 0, whole = 0;
                std::error_code error;
                for (const auto& item : fsys::recursive_directory_iterator(remote_dir / L"move_src", error)) {
                    if (!item.is_regular_file()) continue;
                    ++arrived;
                    if (item.file_size() == static_cast<uintmax_t>(kb) * 1024) ++whole;
                }
                const fsys::path first = made.empty() ? fsys::path{} :
                    remote_dir / fsys::relative(made.front(), root);
                const DWORD first_attributes = GetFileAttributesW(first.c_str());
                const bool read_only = first_attributes != INVALID_FILE_ATTRIBUTES &&
                                       (first_attributes & FILE_ATTRIBUTE_READONLY);
                if (read_only) SetFileAttributesW(first.c_str(), FILE_ATTRIBUTE_NORMAL);
                const bool ok = arrived == made.size() && whole == made.size() && read_only &&
                                !fsys::exists(source);
                printf("move-verify    %s arrived=%zu whole=%zu read_only=%d source_left=%d\n", ok ? "ok  " : "FAIL",
                       arrived, whole, read_only ? 1 : 0, fsys::exists(source) ? 1 : 0);
            }
            RemoveTree(remote_dir);
            RemoveTree(source);
        }
        if (Wants(only, L"large-move-cross") && !cross.empty()) {
            const auto source = root / L"large_move_src";
            fsys::create_directories(source);
            const auto file = (source / L"large.bin").wstring();
            std::vector<char> chunk(8u << 20);
            for (size_t i = 0; i < chunk.size(); ++i) chunk[i] = static_cast<char>(i * 2654435761u >> 24);
            HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            for (int written = 0; handle != INVALID_HANDLE_VALUE && written < large_mb; written += 8) {
                DWORD done = 0;
                WriteFile(handle, chunk.data(), static_cast<DWORD>(chunk.size()), &done, nullptr);
            }
            if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
            const auto remote_dir = fsys::path(cross) / L"pulse_opsbench_cross";
            RemoveTree(remote_dir);
            fsys::create_directories(remote_dir);
            ops::OpRequest request;
            request.type = ops::OpType::Move;
            request.sources = {file};
            request.dest_dir = remote_dir.wstring();
            Report("large-move-x", Run(request), static_cast<double>(large_mb), "MB");
            RemoveTree(remote_dir);
            RemoveTree(source);
        }
        if (Wants(only, L"delete-multi")) {
            const auto folder = root / L"del_multi";
            ops::OpRequest request;
            request.type = ops::OpType::RealDelete;
            request.sources = MakeTree(folder, files, kb, true);
            const double count = static_cast<double>(request.sources.size());
            Report("delete-multi", Run(request), count, "items");
            RemoveTree(folder);
        }
        if (Wants(only, L"delete-tree")) {
            const auto folder = root / L"del_tree";
            const auto made = MakeTree(folder, files, kb, false);
            ops::OpRequest request;
            request.type = ops::OpType::RealDelete;
            request.sources = {folder.wstring()};
            Report("delete-tree", Run(request), static_cast<double>(made.size()), "items");
            RemoveTree(folder);
        }
        if (Wants(only, L"large")) {
            const auto source = root / L"large_src";
            const auto target = root / L"large_dst";
            fsys::create_directories(source);
            fsys::create_directories(target);
            const auto file = (source / L"large.bin").wstring();
            std::vector<char> chunk(8u << 20);
            for (size_t i = 0; i < chunk.size(); ++i) chunk[i] = static_cast<char>(i * 2654435761u >> 24);
            HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            for (int written = 0; handle != INVALID_HANDLE_VALUE && written < large_mb; written += 8) {
                DWORD done = 0;
                WriteFile(handle, chunk.data(), static_cast<DWORD>(chunk.size()), &done, nullptr);
            }
            if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
            ops::OpRequest request;
            request.type = ops::OpType::Copy;
            request.sources = {file};
            request.dest_dir = target.wstring();
            Report("large", Run(request), static_cast<double>(large_mb), "MB");
            if (!cross.empty()) {
                const auto remote_dir = fsys::path(cross) / L"pulse_opsbench_cross";
                RemoveTree(remote_dir);
                fsys::create_directories(remote_dir);
                request.dest_dir = remote_dir.wstring();
                Report("large-cross", Run(request), static_cast<double>(large_mb), "MB");
                RemoveTree(remote_dir);
            }
            RemoveTree(source);
            RemoveTree(target);
        }
        if (Wants(only, L"recycle")) {
            const auto folder = root / L"recycle";
            ops::OpRequest request;
            request.type = ops::OpType::RecycleDelete;
            request.sources = MakeTree(folder, recycle_files, kb, true);
            const auto made = request.sources;
            Report("recycle", Run(request), static_cast<double>(made.size()), "items");
            // Put everything back so the user's Recycle Bin is unchanged.
            if (g_ops.CanUndo()) {
                const uint64_t before = g_ops.Status().completed_ops;
                g_ops.Undo();
                for (int wait = 0; wait < 60000 && g_ops.Status().completed_ops == before; ++wait) Sleep(1);
            }
            size_t restored = 0;
            for (const auto& path : made) restored += GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
            printf("recycle-undo   restored %zu of %zu\n", restored, made.size());
            RemoveTree(folder);
        }
    }

    g_ops.Stop();
    {
        std::lock_guard lock(g_ui_mutex);
        g_ui_stop = true;
        g_ui_cv.notify_one();
    }
    ui.join();
    RemoveTree(root);
    if (diagnostics_enabled) {
        if (!diagnostics::runtime::Flush()) {
            fprintf(stderr, "[bench] diagnostic flush failed\n");
            g_failed = true;
        }
        diagnostics::runtime::Shutdown();
    }
    CoUninitialize();
    return g_failed ? 1 : 0;
}

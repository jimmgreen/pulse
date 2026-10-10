// ops_test_main.cpp — Stage 1B-1 ops-layer self test (console).
//
// Drives the full stack: OpsManager -> ShellClient -> pipe -> pulse_shell.exe
// -> IFileOperation. All file operations are confined to
// bench_data/opstest (created/cleaned by this test). Prints one PASS/FAIL
// line per check; exit code 0 iff all checks pass.
#include "../ops/ops_manager.h"
#include "../app/update_shutdown.h"
#include "../ops/clipboard.h"
#include "../ipc/shell_client.h"
#include "../common/localization.h"
#include "../common/current_user_security.h"
#include <windows.h>
#include <oleidl.h>
#include <winioctl.h>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <memory>
#include <atomic>
#include <thread>
#include <string>
#include <vector>

using namespace pulse;

namespace {

std::wstring SandboxRoot() {
    std::vector<wchar_t> exe(32768);
    const DWORD length = GetModuleFileNameW(nullptr, exe.data(),
                                            static_cast<DWORD>(exe.size()));
    if (length == 0 || length == exe.size()) return {};
    const std::filesystem::path build_dir =
        std::filesystem::path(std::wstring(exe.data(), length)).parent_path();
    return (build_dir.parent_path() / L"bench_data" / L"opstest").wstring();
}

int g_pass = 0;
int g_fail = 0;

void Check(bool cond, const wchar_t* name) {
    if (cond) {
        ++g_pass;
        wprintf(L"[PASS] %s\n", name);
    } else {
        ++g_fail;
        wprintf(L"[FAIL] %s\n", name);
    }
}

bool Exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool MakeFile(const std::wstring& path, const void* data, DWORD size) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    BOOL ok = WriteFile(f, data, size, &w, nullptr);
    CloseHandle(f);
    return ok && w == size;
}

void MakeDir(const std::wstring& path) {
    CreateDirectoryW(path.c_str(), nullptr);
}

bool MakeJunction(const std::wstring& path, const std::wstring& target) {
    struct JunctionData {
        DWORD tag;
        WORD length, reserved;
        WORD substitute_offset, substitute_length, print_offset, print_length;
        wchar_t paths[32768];
    };
    const std::wstring substitute = L"\\??\\" + target;
    if (substitute.size() + target.size() + 2 >= 32768) return false;
    auto data = std::make_unique<JunctionData>();
    data->tag = IO_REPARSE_TAG_MOUNT_POINT;
    data->substitute_length = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    data->print_offset = static_cast<WORD>(data->substitute_length + sizeof(wchar_t));
    data->print_length = static_cast<WORD>(target.size() * sizeof(wchar_t));
    data->length = static_cast<WORD>(8 + data->print_offset + data->print_length + sizeof(wchar_t));
    memcpy(data->paths, substitute.c_str(), data->substitute_length + sizeof(wchar_t));
    memcpy(reinterpret_cast<BYTE*>(data->paths) + data->print_offset, target.c_str(), data->print_length + sizeof(wchar_t));
    if (!CreateDirectoryW(path.c_str(), nullptr)) return false;
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) { RemoveDirectoryW(path.c_str()); return false; }
    DWORD returned = 0;
    const bool ok = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, data.get(),
        data->length + 8, nullptr, 0, &returned, nullptr) != FALSE;
    CloseHandle(handle);
    if (!ok) RemoveDirectoryW(path.c_str());
    return ok;
}

uint64_t FileSize(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return ~0ull;
    ULARGE_INTEGER s{ fad.nFileSizeLow, fad.nFileSizeHigh };
    return s.QuadPart;
}

ops::OpsManager g_ops;
std::atomic<bool> g_pause_when_active{false};
std::atomic<bool> g_pause_when_verifying{false};
std::mutex g_status_mutex;
std::vector<ops::OpStatus> g_status_history;

bool MakePatternFile(const std::wstring& path, uint64_t bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    std::vector<unsigned char> block(1024 * 1024);
    for (size_t i = 0; i < block.size(); ++i) block[i] = static_cast<unsigned char>((i * 131u + 17u) & 0xFFu);
    bool ok = true;
    while (bytes > 0) {
        const DWORD chunk = static_cast<DWORD>((std::min<uint64_t>)(bytes, block.size()));
        DWORD written = 0;
        if (!WriteFile(file, block.data(), chunk, &written, nullptr) || written != chunk) {
            ok = false;
            break;
        }
        bytes -= chunk;
    }
    CloseHandle(file);
    return ok;
}

uint64_t FileHash(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 0;
    uint64_t hash = 1469598103934665603ull;
    std::vector<unsigned char> block(1024 * 1024);
    DWORD read = 0;
    while (ReadFile(file, block.data(), static_cast<DWORD>(block.size()), &read, nullptr) && read) {
        for (DWORD i = 0; i < read; ++i) {
            hash ^= block[i];
            hash *= 1099511628211ull;
        }
    }
    CloseHandle(file);
    return hash;
}

bool WaitOpDone(uint64_t prev_completed, int timeout_ms = 90000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (g_ops.Status().completed_ops > prev_completed) return true;
        Sleep(10);
    }
    return false;
}

// Submit and wait; returns Status snapshot after completion.
ops::OpStatus RunOp(ops::OpRequest req) {
    uint64_t prev = g_ops.Status().completed_ops;
    g_ops.Submit(std::move(req));
    bool ok = WaitOpDone(prev);
    if (!ok) fprintf(stderr, "[test] WaitOpDone TIMEOUT\n");
    return g_ops.Status();
}

ops::OpStatus RunConflictOp(ops::OpRequest req, ops::ConflictChoice choice,
                            bool apply_to_all, size_t* resolved = nullptr) {
    const uint64_t previous = g_ops.Status().completed_ops;
    g_ops.Submit(std::move(req));
    uint64_t token = 0;
    size_t count = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
        if (g_ops.Status().completed_ops > previous) break;
        if (const auto conflict = g_ops.PendingConflict(); conflict && conflict->token != token) {
            token = conflict->token;
            ++count;
            g_ops.ResolveConflict(token, choice, apply_to_all);
        }
        Sleep(1);
    }
    if (resolved) *resolved = count;
    return g_ops.Status();
}

ops::OpRequest SimpleOp(ops::OpType type, std::initializer_list<const wchar_t*> srcs,
                        const wchar_t* dest = nullptr, const wchar_t* name = nullptr) {
    ops::OpRequest r;
    r.type = type;
    for (auto s : srcs) r.sources.push_back(s);
    if (dest) r.dest_dir = dest;
    if (name) r.new_name = name;
    return r;
}

uint64_t FileId(const std::wstring& path) {
    HANDLE handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return 0;
    BY_HANDLE_FILE_INFORMATION info{};
    const BOOL ok = GetFileInformationByHandle(handle, &info);
    CloseHandle(handle);
    return ok ? (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow : 0;
}

// Records every name created in a folder (including temporary names that are
// renamed away again) while an operation runs.
class NameWatcher {
public:
    explicit NameWatcher(const std::wstring& folder) {
        dir_ = CreateFileW(folder.c_str(), FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (dir_ == INVALID_HANDLE_VALUE) return;
        thread_ = std::thread([this] { Run(); });
        Sleep(50);
    }
    ~NameWatcher() { Stop(); }
    std::vector<std::wstring> Stop() {
        if (thread_.joinable()) {
            Sleep(300);  // let the last notifications arrive
            stop_ = true;
            thread_.join();
        }
        if (dir_ != INVALID_HANDLE_VALUE) { CloseHandle(dir_); dir_ = INVALID_HANDLE_VALUE; }
        std::lock_guard lock(mu_);
        return names_;
    }
private:
    void Run() {
        std::vector<BYTE> buffer(512 * 1024);
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        while (!stop_) {
            ResetEvent(ov.hEvent);
            if (!ReadDirectoryChangesW(dir_, buffer.data(), static_cast<DWORD>(buffer.size()), FALSE,
                                       FILE_NOTIFY_CHANGE_FILE_NAME, nullptr, &ov, nullptr))
                break;
            while (!stop_ && WaitForSingleObject(ov.hEvent, 20) == WAIT_TIMEOUT) {}
            DWORD bytes = 0;
            if (stop_) CancelIoEx(dir_, &ov);
            if (!GetOverlappedResult(dir_, &ov, &bytes, TRUE) || bytes == 0) continue;
            std::lock_guard lock(mu_);
            for (auto* info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer.data());;
                 info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(reinterpret_cast<BYTE*>(info) + info->NextEntryOffset)) {
                if (info->Action == FILE_ACTION_ADDED || info->Action == FILE_ACTION_RENAMED_OLD_NAME)
                    names_.emplace_back(info->FileName, info->FileNameLength / sizeof(wchar_t));
                if (!info->NextEntryOffset) break;
            }
        }
        CloseHandle(ov.hEvent);
    }
    HANDLE dir_ = INVALID_HANDLE_VALUE;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::mutex mu_;
    std::vector<std::wstring> names_;
};

} // namespace

static void CheckDeleteProgress(const std::wstring& srcDir)
{
    Check(ops::SpeedBasisFor(ops::OpType::Copy) == ops::OpSpeedBasis::Bytes &&
          ops::SpeedBasisFor(ops::OpType::Move) == ops::OpSpeedBasis::Bytes &&
          ops::SpeedBasisFor(ops::OpType::EmptyRecycle) == ops::OpSpeedBasis::Bytes &&
          ops::SpeedBasisFor(ops::OpType::RecycleDelete) == ops::OpSpeedBasis::Items &&
          ops::SpeedBasisFor(ops::OpType::RealDelete) == ops::OpSpeedBasis::Items &&
          ops::SpeedBasisFor(ops::OpType::RestoreRecycle) == ops::OpSpeedBasis::Items &&
          ops::SpeedBasisFor(ops::OpType::Rename) == ops::OpSpeedBasis::None,
          L"speed basis: byte transfers, item operations, and neither");

    // The dialog feeds the speed graph from item counts for delete, so a delete that
    // runs past the estimator's warm-up has to report a live item speed.
    const std::wstring speed_dir = srcDir + L"\\speed-delete";
    MakeDir(speed_dir);
    std::vector<std::wstring> victims;
    for (int i = 0; i < 400; ++i) {
        wchar_t name[64];
        swprintf_s(name, L"s%04d.tmp", i);
        const std::wstring path = speed_dir + L"\\" + name;
        char buf[512] = {};
        MakeFile(path, buf, sizeof(buf));
        victims.push_back(path);
    }
    {
        std::lock_guard<std::mutex> lock(g_status_mutex);
        g_status_history.clear();
    }
    ops::OpRequest request;
    request.type = ops::OpType::RealDelete;
    for (const auto& victim : victims) request.sources.push_back(victim);
    const uint64_t previous = g_ops.Status().completed_ops;
    const uint64_t task_id = g_ops.Submit(std::move(request));
    const auto started_at = std::chrono::steady_clock::now();
    Check(WaitOpDone(previous), L"bulk realdelete completes");
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_at).count();

    bool saw_item_basis = false;
    bool saw_live_speed = false;
    bool saw_eta = false;
    bool peak_consistent = true;
    {
        std::lock_guard<std::mutex> lock(g_status_mutex);
        for (const auto& status : g_status_history) {
            if (status.task_id != task_id || !status.active) continue;
            saw_item_basis = saw_item_basis || status.speed_basis == ops::OpSpeedBasis::Items;
            saw_live_speed = saw_live_speed || status.items_per_second > 0.0;
            saw_eta = saw_eta || status.eta_seconds > 0;
            peak_consistent = peak_consistent &&
                status.peak_items_per_second + 1e-9 >= status.items_per_second;
        }
    }
    Check(saw_item_basis, L"delete task reports its speed in items");
    Check(peak_consistent, L"delete peak never drops below the live item speed");
    Check(saw_live_speed || elapsed_ms < 1200,
          L"delete reports a live item speed once it runs past the warm-up");
    // Item rates sit below the 1/s floor byte transfers use, so a slow delete has to
    // show a countdown instead of "estimating" forever.
    Check(saw_eta || elapsed_ms < 1200,
          L"delete reports a remaining-time estimate once it runs past the warm-up");
    wprintf(L"[INFO] bulk realdelete: %lld ms, live_speed=%d\n",
            static_cast<long long>(elapsed_ms), saw_live_speed ? 1 : 0);
}

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    // Assertions below use the Simplified texts (" - 副本", "已取消"); the
    // English forms are checked separately.
    l10n::SetLanguage(L"zh-CN");
    fprintf(stderr, "[test] start\n");

    if (argc > 1 && std::wstring_view(argv[1]) == L"--transfer-routing") {
        const auto root = std::filesystem::path(SandboxRoot()).parent_path() /
            (L"transfer-routing-" + std::to_wstring(GetCurrentProcessId()));
        const auto source = root / L"source";
        const auto target = root / L"target";
        std::filesystem::create_directories(source);
        std::filesystem::create_directories(target);
        const auto file = source / L"sample.txt";
        const auto copy = target / L"sample.txt";
        const char content[] = "permission routing regression";
        Check(MakeFile(file.wstring(), content, sizeof(content)), L"create isolated transfer fixture");
        g_ops.Start([] {});
        auto status = RunOp(SimpleOp(ops::OpType::Copy, {file.c_str()}, target.c_str()));
        Check(status.phase == ops::OpPhase::Completed && Exists(copy.wstring()) &&
            Exists(file.wstring()) && status.can_pause, L"ordinary copy retains fast path and pause capability");
        const auto moved_dir = root / L"moved";
        std::filesystem::create_directories(moved_dir);
        status = RunOp(SimpleOp(ops::OpType::Move, {copy.c_str()}, moved_dir.c_str()));
        Check(status.phase == ops::OpPhase::Completed && !Exists(copy.wstring()) &&
            Exists((moved_dir / L"sample.txt").wstring()), L"ordinary move retains fast path");
        const auto completed = g_ops.Status().completed_ops;
        Check(g_ops.CanUndo(), L"ordinary move remains undoable");
        g_ops.Undo();
        Check(WaitOpDone(completed) && Exists(copy.wstring()) &&
            !Exists((moved_dir / L"sample.txt").wstring()), L"move undo restores original location");
        g_ops.Stop();
        std::filesystem::remove(file);
        std::filesystem::remove(copy);
        std::filesystem::remove(source);
        std::filesystem::remove(target);
        std::filesystem::remove(moved_dir);
        std::filesystem::remove(root);
        return g_fail ? 1 : 0;
    }

    if (argc > 1 && std::wstring(argv[1]) == L"--duplicate-cleanup-audit") {
        const auto base = std::filesystem::path(SandboxRoot()).parent_path();
        const auto fixture = base / (L"duplicate-cleanup-ops-" + std::to_wstring(GetCurrentProcessId()) +
                                    L"-" + std::to_wstring(GetTickCount64()));
        std::error_code error;
        std::filesystem::create_directories(base, error);
        const bool created = std::filesystem::create_directory(fixture, error);
        fwprintf(stderr, L"[duplicate cleanup fixture] %s\n", fixture.c_str());
        Check(created, L"duplicate cleanup uses an isolated new fixture");
        if (!created) return 1;
        const auto candidate = (fixture / L"candidate.txt").wstring();
        const auto journal = (fixture / L"recovery.json").wstring();
        const std::string payload = "surviving duplicate";
        Check(MakeFile(candidate, payload.data(), static_cast<DWORD>(payload.size())),
              L"duplicate cleanup candidate fixture created");
        std::wstring portable = candidate;
        for (auto& ch : portable) if (ch == L'\\') ch = L'/';
        const int count = WideCharToMultiByte(CP_UTF8, 0, portable.c_str(), static_cast<int>(portable.size()),
                                             nullptr, 0, nullptr, nullptr);
        std::string utf8(count, '\0');
        WideCharToMultiByte(CP_UTF8, 0, portable.c_str(), static_cast<int>(portable.size()),
                            utf8.data(), count, nullptr, nullptr);
        const auto json = std::string("{\"schema\":1,\"entries\":[{\"seq\":\"1\",\"type\":2,\"policy\":0,") +
            "\"duplicate_cleanup\":true,\"sources\":[\"" + utf8 + "\"]}]}";
        Check(MakeFile(journal, json.data(), static_cast<DWORD>(json.size())), L"duplicate recovery marker fixture created");
        g_ops.SetJournalPath(journal);
        const auto recovery = g_ops.PendingRecovery();
        Check(recovery.entries.size() == 1 && recovery.entries[0].request.duplicate_cleanup,
              L"recovery retains the duplicate cleanup safety marker");
        Check(!g_ops.RetryRecovery() && g_ops.PendingRecovery().entries.empty(),
              L"duplicate cleanup cannot resume without a new scan");
        g_ops.Start([] {});
        ops::OpRequest request;
        request.type = ops::OpType::RecycleDelete;
        request.sources = {candidate};
        request.duplicate_cleanup = true;
        request.duplicate_groups = {{{(fixture / L"missing-keeper.txt").wstring(), payload.size(), 0},
                                     {{candidate, payload.size(), 0}}}};
        const auto status = RunOp(request);
        Check(status.phase == ops::OpPhase::Failed &&
                  status.last_error.find(L"重新扫描") != std::wstring::npos && Exists(candidate),
              L"worker rejects stale keeper before invoking Shell deletion");
        Check(status.completed_ops == 1, L"recovered duplicate cleanup was never enqueued");
        const auto keeper = (fixture / L"keeper.txt").wstring();
        Check(MakeFile(keeper, payload.data(), static_cast<DWORD>(payload.size())),
              L"successful cleanup keeper fixture created");
        auto snapshot = [](const std::wstring& file) {
            WIN32_FILE_ATTRIBUTE_DATA data{};
            Check(GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &data) != FALSE,
                  L"cleanup operation fixture metadata readable");
            return ops::DuplicateCleanupFile{file,
                (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow,
                (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                    data.ftLastWriteTime.dwLowDateTime};
        };
        const auto expected_hash = FileHash(candidate);
        request.duplicate_groups = {{snapshot(keeper), {snapshot(candidate)}}};
        const auto cleaned = RunOp(request);
        const bool recycled = cleaned.phase == ops::OpPhase::Completed && !Exists(candidate);
        Check(recycled && Exists(keeper) && FileHash(keeper) == expected_hash,
              L"guarded Shell cleanup recycles only the extra and preserves keeper content");
        if (!recycled) fwprintf(stderr, L"[duplicate cleanup] %s\n", cleaned.last_error.c_str());
        if (recycled) {
            const auto completed = g_ops.Status().completed_ops;
            Check(g_ops.CanUndo(), L"isolated duplicate cleanup has its own undo entry");
            g_ops.Undo();
            const bool undo_done = WaitOpDone(completed);
            const auto undo_status = g_ops.Status();
            const bool restored = undo_done && undo_status.phase == ops::OpPhase::Completed &&
                      Exists(candidate) && FileHash(candidate) == expected_hash && FileHash(keeper) == expected_hash;
            Check(restored,
                  L"undo restores the exact isolated extra without changing the keeper");
            if (!restored) {
                const int error_bytes = WideCharToMultiByte(CP_UTF8, 0, undo_status.last_error.c_str(),
                    static_cast<int>(undo_status.last_error.size()), nullptr, 0, nullptr, nullptr);
                std::string utf8_error(error_bytes, '\0');
                WideCharToMultiByte(CP_UTF8, 0, undo_status.last_error.c_str(),
                    static_cast<int>(undo_status.last_error.size()), utf8_error.data(), error_bytes, nullptr, nullptr);
                fprintf(stderr, "[duplicate cleanup undo error] %s\n", utf8_error.c_str());
                fwprintf(stderr, L"[duplicate cleanup undo] done=%d phase=%d error=%s candidate=%s exists=%d hash=%llu expected=%llu keeper_exists=%d keeper_hash=%llu sid=%s\n",
                    undo_done, static_cast<int>(undo_status.phase), undo_status.last_error.c_str(), candidate.c_str(),
                    Exists(candidate), FileHash(candidate), expected_hash, Exists(keeper), FileHash(keeper),
                    CurrentUserSidString().c_str());
                g_ops.Stop();
                g_ops.SetJournalPath(L"");
                return 1;
            }
        }
        g_ops.Stop();
        g_ops.SetJournalPath(L"");
        Check(DeleteFileW(candidate.c_str()) != FALSE, L"duplicate worker fixture candidate remains removable");
        Check(DeleteFileW(keeper.c_str()) != FALSE, L"duplicate worker fixture keeper remains removable");
        if (Exists(journal)) DeleteFileW(journal.c_str());
        Check(RemoveDirectoryW(fixture.c_str()) != FALSE, L"duplicate worker fixture cleaned up");
        wprintf(L"%d passed, %d failed\n", g_pass, g_fail);
        return g_fail ? 1 : 0;
    }

    const bool update_shutdown_only = argc > 1 && std::wstring_view(argv[1]) == L"--update-shutdown";
    if (update_shutdown_only) {
        {
            struct WindowFixture { ops::OpsManager operations; bool migration_pending = true; } fixture;
            WNDCLASSW window_class{};
            window_class.hInstance = GetModuleHandleW(nullptr);
            window_class.lpszClassName = L"PulseUpdateShutdownTest";
            window_class.lpfnWndProc = [](HWND window, UINT message, WPARAM wp, LPARAM lp) -> LRESULT {
                auto* state = reinterpret_cast<WindowFixture*>(GetWindowLongPtrW(window, GWLP_USERDATA));
                if (state && message == app::UpdateShutdownMessage()) {
                    return app::RequestUpdateShutdown(state->operations, state->migration_pending,
                        [window] { return DestroyWindow(window) != FALSE; });
                }
                return DefWindowProcW(window, message, wp, lp);
            };
            RegisterClassW(&window_class);
            const HWND window = CreateWindowExW(0, window_class.lpszClassName, L"", 0, 0, 0, 0, 0,
                HWND_MESSAGE, nullptr, window_class.hInstance, nullptr);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&fixture));
            DWORD_PTR reply = 0;
            Check(window && SendMessageTimeoutW(window, app::UpdateShutdownMessage(), 0, 0,
                      SMTO_ABORTIFHUNG, 1000, &reply) && reply == app::kUpdateShutdownBusy && IsWindow(window),
                  L"registered update message reports busy without destroying the isolated window");
            fixture.migration_pending = false;
            reply = 0;
            Check(window && SendMessageTimeoutW(window, app::UpdateShutdownMessage(), 0, 0,
                      SMTO_ABORTIFHUNG, 1000, &reply) && reply == app::kUpdateShutdownAccepted && !IsWindow(window),
                  L"registered update message acknowledges a synchronous orderly window close");
            if (IsWindow(window)) DestroyWindow(window);
            UnregisterClassW(window_class.lpszClassName, window_class.hInstance);
        }
        {
            ops::OpsManager queued;
            queued.Submit({});
            bool closed = false;
            Check(app::RequestUpdateShutdown(queued, false, [&] { closed = true; return true; }) ==
                      app::kUpdateShutdownBusy && !closed,
                  L"update handshake rejects queued operations before status becomes active");
            Check(!app::RequestUpdateLaunch(queued, false, [&] { closed = true; }) && !closed,
                  L"verified installer waits for queued work without launching or failing the download");
        }
        {
            ops::OpsManager idle;
            bool closed = false;
            Check(app::RequestUpdateShutdown(idle, true, [&] { closed = true; return true; }) ==
                      app::kUpdateShutdownBusy && !closed,
                  L"index migration blocks update exit");
            Check(app::RequestUpdateShutdown(idle, false, [&] {
                      Check(idle.Submit({}) == 0, L"shutdown callback cannot enqueue new work");
                      return false;
                  }) == app::kUpdateShutdownBusy,
                  L"failed window close rejects update installation");
            Check(app::RequestUpdateShutdown(idle, false, [] { return true; }) ==
                      app::kUpdateShutdownAccepted,
                  L"failed close restores admission and allows a later update retry");
            bool rejected_all = true;
            for (const auto type : {ops::OpType::Copy, ops::OpType::Move, ops::OpType::RecycleDelete,
                    ops::OpType::RealDelete, ops::OpType::Rename, ops::OpType::CreateFolder,
                    ops::OpType::CreateTextFile, ops::OpType::RestoreRecycle, ops::OpType::EmptyRecycle,
                    ops::OpType::BatchRename}) {
                ops::OpRequest request;
                request.type = type;
                rejected_all &= idle.Submit(std::move(request)) == 0;
            }
            Check(rejected_all, L"accepted update exit blocks every file-operation type");
            idle.CancelUpdatePreparation();
            Check(idle.Submit({}) != 0, L"cancelled preparation restores normal submissions");
        }
        bool atomic_admission = true;
        for (int i = 0; i < 100; ++i) {
            ops::OpsManager racing;
            std::atomic<bool> start{false};
            uint64_t submitted = 0;
            std::thread producer([&] {
                while (!start.load()) std::this_thread::yield();
                submitted = racing.Submit({});
            });
            start = true;
            const bool accepted = racing.TryPrepareForUpdate();
            producer.join();
            atomic_admission &= accepted ? submitted == 0 : submitted != 0;
        }
        Check(atomic_admission, L"concurrent submission and update exit cannot both be accepted");
    }

    Check(ops::TerminalCommandLine(L"C:\\A\\B") == L"-d \"C:\\A\\B\"",
          L"terminal command quotes a directory");
    Check(ops::TerminalCommandLine(L"C:\\") == L"-d \"C:\\\\\"",
          L"terminal command preserves a drive root");
    Check(ops::TerminalCommandLine(L"C:\\quoted\" folder\\") ==
              L"-d \"C:\\quoted\\\" folder\\\\\"",
          L"terminal command escapes quotes and trailing slashes");

    // --- Setup sandbox ------------------------------------------------------
    const std::wstring root = SandboxRoot();
    if (root.empty()) {
        fprintf(stderr, "[test] cannot locate bench_data/opstest\n");
        return 2;
    }
    std::wstring srcDir = root + L"\\src";
    std::wstring dstDir = root + L"\\dst";
    // Exact, workspace-contained sandbox path.
    std::error_code cleanup_error;
    std::filesystem::remove_all(std::filesystem::path(root), cleanup_error);
    MakeDir(root);
    MakeDir(srcDir);
    MakeDir(dstDir);

    const char payload[] = "pulse ops self-test payload";
    MakeFile(srcDir + L"\\a.txt", payload, sizeof(payload) - 1);
    MakeFile(srcDir + L"\\b.txt", payload, sizeof(payload) - 1);
    MakeFile(srcDir + L"\\d.txt", payload, sizeof(payload) - 1);

    std::atomic<bool> notified{false};
    g_ops.Start([&] {
        notified = true;
        const auto status = g_ops.Status();
        {
            std::lock_guard<std::mutex> lock(g_status_mutex);
            g_status_history.push_back(status);
        }
        if (status.phase == ops::OpPhase::Verifying && g_pause_when_verifying.exchange(false))
            g_ops.PauseCurrent();
        if (g_pause_when_active.load() && status.active &&
            status.phase == ops::OpPhase::Scanning) {
            g_pause_when_active.store(false);
            g_ops.PauseCurrent();
        }
    });
    Sleep(800); // let the ops worker bring up ShellClient before direct IPC calls
    fprintf(stderr, "[test] ops started\n");

    if (argc > 1 && std::wstring_view(argv[1]) == L"--delete-progress-only") {
        CheckDeleteProgress(srcDir);
        g_ops.Stop();
        wprintf(L"\n== delete progress tests: %d passed, %d failed ==\n", g_pass, g_fail);
        return g_fail == 0 ? 0 : 1;
    }
    if (update_shutdown_only) {
        const std::wstring source = srcDir + L"\\update-active.bin";
        Check(MakePatternFile(source, 8ull * 1024 * 1024), L"create isolated update-operation fixture");
        g_pause_when_active = true;
        const auto previous = g_ops.Status().completed_ops;
        g_ops.Submit(SimpleOp(ops::OpType::Copy, {source.c_str()}, dstDir.c_str()));
        const auto deadline = GetTickCount64() + 10000;
        while (GetTickCount64() < deadline && g_ops.Status().phase != ops::OpPhase::Paused) Sleep(1);
        Check(g_ops.Status().phase == ops::OpPhase::Paused, L"hold an actual transfer active for update handshake");
        unsigned launches = 0;
        Check(!app::RequestUpdateLaunch(g_ops, false, [&] { ++launches; }) && launches == 0,
              L"paused transfer keeps verified update waiting without an installer prompt");
        bool closed = false;
        Check(app::RequestUpdateShutdown(g_ops, false, [&] { closed = true; return true; }) ==
                  app::kUpdateShutdownBusy && !closed,
              L"busy update handshake never closes a running transfer");
        g_ops.ResumeCurrent();
        Check(WaitOpDone(previous), L"waiting update allows isolated transfer to finish normally");
        bool accepted = false;
        const auto idle_deadline = GetTickCount64() + 3000;
        while (!accepted && GetTickCount64() < idle_deadline) {
            accepted = app::RequestUpdateShutdown(g_ops, false, [] { return true; }) ==
                       app::kUpdateShutdownAccepted;
            if (!accepted) Sleep(1);
        }
        Check(accepted, L"update retry succeeds after the active transfer has finished");
        g_ops.CancelUpdatePreparation();
        Check(app::RequestUpdateLaunch(g_ops, false, [&] { ++launches; }) && launches == 1,
              L"next update tick launches automatically after the transfer completes");
        Check(g_ops.TryPrepareForUpdate(), L"launch gate releases admission for the final installer shutdown handshake");
        g_ops.CancelUpdatePreparation();
        g_ops.Stop();
        wprintf(L"\n== update shutdown tests: %d passed, %d failed ==\n", g_pass, g_fail);
        return g_fail == 0 ? 0 : 1;
    }

    if (argc > 1 && (std::wstring(argv[1]) == L"--audit" || std::wstring(argv[1]) == L"--recycle-audit")) {
        const bool recycle_only = std::wstring(argv[1]) == L"--recycle-audit";
        if (!recycle_only) {
            // OPS-01: neither ordinary names nor ancestor names prove ownership.
            for (bool retry : { false, true }) {
                const auto dir = root + (retry ? L"\\retry.pulse-copy-parent" : L"\\discard.pulse-backup-parent");
                MakeDir(dir);
                const auto keep = dir + L"\\user.pulse-copy-123";
                MakeDir(keep);
                MakeFile(keep + L"\\keep.txt", payload, sizeof(payload));
                const auto journal = root + L"\\recovery.json";
                auto narrow = [](std::wstring value) {
                    for (auto& c : value) if (c == L'\\') c = L'/';
                    const int bytes = WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
                        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
                    std::string result(bytes, '\0');
                    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                        result.data(), bytes, nullptr, nullptr);
                    return result;
                };
                const auto json = std::string("{\"schema\":1,\"entries\":[{\"seq\":\"1\",\"type\":0,\"policy\":0,\"sources\":[\"")
                    + narrow(srcDir + L"\\a.txt") + "\"],\"dest\":\"" + narrow(dir) + "\"}]}";
                g_ops.Stop();
                MakeFile(journal, json.data(), static_cast<DWORD>(json.size()));
                g_ops.SetJournalPath(journal);
                Check(g_ops.PendingRecovery().entries.size() == 1, L"OPS-01 recovery fixture loaded");
                if (retry) g_ops.RetryRecovery(); else g_ops.DiscardRecovery();
                const auto before = g_ops.Status().completed_ops;
                g_ops.Start([] {});
                RunOp(SimpleOp(ops::OpType::Copy, { (srcDir + L"\\b.txt").c_str() }, dstDir.c_str()));
                Check(WaitOpDone(before + (retry ? 1 : 0)) && Exists(keep + L"\\keep.txt"),
                      retry ? L"OPS-01 retry preserves unrelated marked paths" : L"OPS-01 discard preserves unrelated marked paths");
                DeleteFileW((dstDir + L"\\b.txt").c_str());
            }
            g_ops.Stop();
            g_ops.SetJournalPath(L"");
            g_ops.Start([] {
                if (g_ops.Status().phase == ops::OpPhase::Verifying && g_pause_when_verifying.exchange(false))
                    g_ops.PauseCurrent();
            });
            // A hard-link alias exercises the same volume/file identity as a junction alias.
            const auto alias = dstDir + L"\\a.txt";
            Check(CreateHardLinkW(alias.c_str(), (srcDir + L"\\a.txt").c_str(), nullptr) != FALSE,
                  L"OPS-02 create same-object alias");
            const auto hash = FileHash(alias);
            auto move = SimpleOp(ops::OpType::Move, { (srcDir + L"\\a.txt").c_str() }, dstDir.c_str());
            move.collision_policy = ops::CollisionPolicy::Replace;
            const auto alias_status = RunOp(move);
            Check(alias_status.last_error.empty() && FileHash(alias) == hash && FileHash(srcDir + L"\\a.txt") == hash,
                  L"OPS-02 move replace preserves same-object aliases");
            DeleteFileW(alias.c_str());
            const auto junction = root + L"\\junction";
            const bool have_junction = MakeJunction(junction, srcDir);
            Check(have_junction, L"OPS-02 create controlled directory junction");
            if (have_junction) {
                for (bool reverse : { false, true }) {
                    auto request = SimpleOp(ops::OpType::Move,
                        { ((reverse ? junction : srcDir) + L"\\a.txt").c_str() },
                        (reverse ? srcDir : junction).c_str());
                    request.collision_policy = ops::CollisionPolicy::Replace;
                    const auto result = RunOp(request);
                    Check(result.last_error.empty() && FileHash(srcDir + L"\\a.txt") == hash &&
                          FileHash(junction + L"\\a.txt") == hash,
                          L"OPS-02 junction move replace preserves the unique file in both directions");
                }
                RemoveDirectoryW(junction.c_str());
            }
            const auto empty = srcDir + L"\\empty";
            MakeDir(empty);
            MakeFile(dstDir + L"\\empty", payload, sizeof(payload));
            auto skipped = SimpleOp(ops::OpType::Move, { empty.c_str() }, dstDir.c_str());
            const auto skip_status = RunConflictOp(skipped, ops::ConflictChoice::Skip, true);
            Check(skip_status.last_error.empty() && Exists(empty), L"OPS-03 skipped empty directory survives move cleanup");
            const auto skipped_link = srcDir + L"\\skipped-link";
            const bool have_skipped_link = MakeJunction(skipped_link, empty);
            Check(have_skipped_link, L"OPS-03 create controlled skipped junction");
            if (have_skipped_link) {
                MakeFile(dstDir + L"\\skipped-link", payload, sizeof(payload));
                const auto link_status = RunConflictOp(SimpleOp(ops::OpType::Move,
                    { skipped_link.c_str() }, dstDir.c_str()), ops::ConflictChoice::Skip, true);
                Check(link_status.last_error.empty() && Exists(skipped_link) && Exists(empty),
                      L"OPS-03 skipped junction and its target survive move cleanup");
                RemoveDirectoryW(skipped_link.c_str());
            }
            for (int no_op_position = 0; no_op_position < 3; ++no_op_position) {
                const auto stem = L"merge" + std::to_wstring(no_op_position);
                const auto source = srcDir + L"\\" + stem;
                const auto target = dstDir + L"\\" + stem;
                const auto same = dstDir + L"\\noop" + std::to_wstring(no_op_position);
                const auto extra = srcDir + L"\\extra" + std::to_wstring(no_op_position);
                MakeDir(source); MakeDir(target); MakeDir(same); MakeDir(extra);
                MakeFile(source + L"\\new.txt", payload, sizeof(payload));
                MakeFile(target + L"\\old.txt", payload, sizeof(payload));
                const auto old_hash = FileHash(target + L"\\old.txt");
                ops::OpRequest req;
                req.type = ops::OpType::Move; req.dest_dir = dstDir;
                req.sources = { source, extra };
                req.sources.insert(req.sources.begin() + no_op_position, same);
                const auto status = RunOp(req);
                auto previous = g_ops.Status().completed_ops;
                g_ops.Undo();
                Check(WaitOpDone(previous + 1) && status.last_error.empty(), L"OPS-04 multi-root move and undo complete");
                // Undo queues more than one operation; wait for its final directory restoration.
                const auto deadline = GetTickCount64() + 5000;
                while (!Exists(source + L"\\new.txt") && GetTickCount64() < deadline) Sleep(10);
                Check(FileHash(target + L"\\old.txt") == old_hash && Exists(source + L"\\new.txt") && !Exists(source + L"\\old.txt"),
                      L"OPS-04 undo preserves preexisting merge content for every no-op position");
            }
        }
        // OPS-05: recycle only our isolated files, then restore exact versions.
        const auto versioned = srcDir + L"\\versions.txt";
        const auto bin = versioned.substr(0, 2) + L"\\$Recycle.Bin\\" + CurrentUserSidString();
        auto version_paths = [&] {
            std::vector<std::wstring> result;
            WIN32_FIND_DATAW data{};
            HANDLE find = FindFirstFileW((bin + L"\\$I*").c_str(), &data);
            if (find == INVALID_HANDLE_VALUE) return result;
            do {
                const auto index = bin + L"\\" + data.cFileName;
                HANDLE file = CreateFileW(index.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file == INVALID_HANDLE_VALUE) continue;
                std::vector<unsigned char> bytes(65536);
                DWORD read = 0;
                const bool ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) != FALSE;
                CloseHandle(file);
                if (!ok || read < 30) continue;
                uint64_t schema = 0; memcpy(&schema, bytes.data(), 8);
                const size_t offset = schema == 2 ? 28 : 24;
                const auto text = reinterpret_cast<const wchar_t*>(bytes.data() + offset);
                const std::wstring original(text, wcsnlen(text, (read - offset) / sizeof(wchar_t)));
                if (_wcsicmp(original.c_str(), versioned.c_str()) != 0) continue;
                auto content = index; content[content.find_last_of(L'\\') + 2] = L'R';
                result.push_back(content);
            } while (FindNextFileW(find, &data));
            FindClose(find);
            return result;
        };
        MakeFile(versioned, "first", 5);
        auto first_status = RunOp(SimpleOp(ops::OpType::RecycleDelete, { versioned.c_str() }));
        const auto first_versions = version_paths();
        MakeFile(versioned, "second", 6);
        auto second_status = RunOp(SimpleOp(ops::OpType::RecycleDelete, { versioned.c_str() }));
        auto versions = version_paths();
        if (!first_status.last_error.empty() || !second_status.last_error.empty() || first_versions.size() != 1 || versions.size() != 2) {
            wprintf(L"OPS-05 diagnostics: first=%s; second=%s; versions=%zu/%zu; bin=%s\n",
                first_status.last_error.c_str(), second_status.last_error.c_str(),
                first_versions.size(), versions.size(), bin.c_str());
        }
        Check(first_status.last_error.empty() && second_status.last_error.empty() && first_versions.size() == 1 && versions.size() == 2,
              L"OPS-05 two controlled recycle versions exist");
        if (first_versions.size() == 1 && versions.size() == 2) {
            const auto ambiguous = RunOp(SimpleOp(ops::OpType::RestoreRecycle, { versioned.c_str() }));
            Check(!ambiguous.last_error.empty() && !Exists(versioned) && version_paths().size() == 2,
                  L"OPS-05 ambiguous legacy identity does not restore a guessed version");
            for (const auto& selected : versions) {
                const auto selected_hash = FileHash(selected);
                const auto restored = RunOp(SimpleOp(ops::OpType::RestoreRecycle, { selected.c_str() }));
                Check(restored.last_error.empty() && FileHash(versioned) == selected_hash && !Exists(selected),
                      L"OPS-05 exact selected recycle version restored");
                DeleteFileW(versioned.c_str());
            }
            Check(version_paths().empty(), L"OPS-05 controlled recycle fixtures cleaned");
        }
        if (recycle_only) {
            g_ops.Stop();
            wprintf(L"Recycle audit: %d passed, %d failed\n", g_pass, g_fail);
            return g_fail ? 1 : 0;
        }
        const auto verify = srcDir + L"\\verify.bin";
        Check(MakePatternFile(verify, 8 * 1024 * 1024), L"OPS-06 create verification fixture");
        g_ops.SetVerifyCopies(true);
        for (bool cancel : { false, true }) {
            g_pause_when_verifying = true;
            const auto previous = g_ops.Status().completed_ops;
            g_ops.Submit(SimpleOp(ops::OpType::Copy, { verify.c_str() }, dstDir.c_str()));
            const auto deadline = GetTickCount64() + 10000;
            while (g_ops.Status().phase != ops::OpPhase::Paused && GetTickCount64() < deadline) Sleep(1);
            Check(g_ops.Status().phase == ops::OpPhase::Paused, L"OPS-06 SHA pause is observable");
            if (cancel) g_ops.CancelCurrent(); else g_ops.ResumeCurrent();
            Check(WaitOpDone(previous), cancel ? L"OPS-06 SHA pause cancellation completes" : L"OPS-06 SHA resumes and completes");
            Check(cancel ? !Exists(dstDir + L"\\verify.bin") : FileHash(verify) == FileHash(dstDir + L"\\verify.bin"),
                  L"OPS-06 verification outcome preserves data");
            DeleteFileW((dstDir + L"\\verify.bin").c_str());
        }
        g_ops.Stop();
        wprintf(L"Audit: %d passed, %d failed\n", g_pass, g_fail);
        return g_fail ? 1 : 0;
    }

    // --- 1. Ping ------------------------------------------------------------
    {
        Check(ipc::ShellClient::Instance().Ping(), L"IPC ping pulse_shell.exe");
    }

    // --- 2. Copy ------------------------------------------------------------
    {
        auto st = RunOp(SimpleOp(ops::OpType::Copy, { (srcDir + L"\\a.txt").c_str() }, dstDir.c_str()));
        Check(Exists(dstDir + L"\\a.txt") && FileSize(dstDir + L"\\a.txt") == sizeof(payload) - 1,
              L"copy src\\a.txt -> dst");
        if (!st.last_error.empty()) wprintf(L"       error: %s\n", st.last_error.c_str());
        Check(st.last_error.empty(), L"copy reported no error");

        MakeFile(srcDir + L"\\display-path.txt", payload, sizeof(payload) - 1);
        const std::wstring prefixed_dst = L"\\\\?\\" + dstDir;
        auto prefixed_st = RunOp(SimpleOp(ops::OpType::Copy,
            { (srcDir + L"\\display-path.txt").c_str() }, prefixed_dst.c_str()));
        Check(prefixed_st.last_error.empty() &&
              prefixed_st.summary.find(L"\\\\?\\") == std::wstring::npos,
              L"copy summary hides extended-length path prefix");
    }

    {
        MakeFile(srcDir + L"\\same.txt", payload, sizeof(payload) - 1);
        auto st = RunOp(SimpleOp(ops::OpType::Copy,
            { (srcDir + L"\\same.txt").c_str() }, srcDir.c_str()));
        Check(Exists(srcDir + L"\\same.txt") && Exists(srcDir + L"\\same - 副本.txt"),
              L"copy into same folder creates 副本");
        if (!st.last_error.empty()) wprintf(L"       error: %s\n", st.last_error.c_str());
        Check(st.last_error.empty(), L"same-folder copy reported no error");

        l10n::SetLanguage(L"en-US");
        MakeFile(srcDir + L"\\same-en.txt", payload, sizeof(payload) - 1);
        auto en_st = RunOp(SimpleOp(ops::OpType::Copy,
            { (srcDir + L"\\same-en.txt").c_str() }, srcDir.c_str()));
        l10n::SetLanguage(L"zh-CN");
        Check(en_st.last_error.empty() && Exists(srcDir + L"\\same-en - Copy.txt"),
              L"English UI names a same-folder copy \" - Copy\"");
        Check(en_st.summary.find(L"Copy") == 0 && en_st.summary.find(L" completed") != std::wstring::npos,
              L"English UI operation summary");

        const uint64_t before_noop = g_ops.Status().completed_ops;
        const auto noop_started = std::chrono::steady_clock::now();
        auto move_st = RunOp(SimpleOp(ops::OpType::Move,
            { (srcDir + L"\\same.txt").c_str() }, srcDir.c_str()));
        Check(move_st.completed_ops == before_noop + 1 &&
              std::chrono::steady_clock::now() - noop_started < std::chrono::seconds(2),
              L"same-folder move reports completion promptly");
        Check(Exists(srcDir + L"\\same.txt"), L"move into same folder is a no-op");
        Check(move_st.last_error.empty(), L"same-folder move reported no error");
        Check(move_st.summary.empty(), L"same-folder move stays silent in status");
    }

    // --- 3. Move ------------------------------------------------------------
    {
        const char oldPayload[] = "old";
        const char newPayload[] = "new collision payload";
        MakeFile(srcDir + L"\\collision.txt", newPayload, sizeof(newPayload) - 1);
        MakeFile(dstDir + L"\\collision.txt", oldPayload, sizeof(oldPayload) - 1);
        auto replace = SimpleOp(ops::OpType::Copy,
            { (srcDir + L"\\collision.txt").c_str() }, dstDir.c_str());
        replace.collision_policy = ops::CollisionPolicy::Replace;
        replace.is_undo = true;
        RunOp(std::move(replace));
        Check(FileSize(dstDir + L"\\collision.txt") == sizeof(newPayload) - 1,
            L"collision policy: replace target");

        auto keepBoth = SimpleOp(ops::OpType::Copy,
            { (srcDir + L"\\collision.txt").c_str() }, dstDir.c_str());
        keepBoth.collision_policy = ops::CollisionPolicy::KeepBoth;
        keepBoth.is_undo = true;
        RunOp(std::move(keepBoth));
        int collisionCopies = 0;
        WIN32_FIND_DATAW collisionData{};
        HANDLE collisionFind = FindFirstFileW((dstDir + L"\\collision*.txt").c_str(), &collisionData);
        if (collisionFind != INVALID_HANDLE_VALUE) {
            do { ++collisionCopies; } while (FindNextFileW(collisionFind, &collisionData));
            FindClose(collisionFind);
        }
        Check(collisionCopies >= 2, L"collision policy: keep both with automatic rename");
    }

    // --- 4. Move ------------------------------------------------------------
    {
        RunOp(SimpleOp(ops::OpType::Move, { (srcDir + L"\\b.txt").c_str() }, dstDir.c_str()));
        Check(!Exists(srcDir + L"\\b.txt") && Exists(dstDir + L"\\b.txt"), L"move src\\b.txt -> dst");
    }

    // --- 4. Rename ----------------------------------------------------------
    {
        auto rename_st = RunOp(SimpleOp(ops::OpType::Rename,
            { (dstDir + L"\\a.txt").c_str() }, nullptr, L"a2.txt"));
        Check(!Exists(dstDir + L"\\a.txt") && Exists(dstDir + L"\\a2.txt"), L"rename a.txt -> a2.txt");
        Check(rename_st.last_error.empty(), L"rename reported no error");

        MakeFile(srcDir + L"\\prefixed-rename.txt", payload, sizeof(payload) - 1);
        const std::wstring prefixed_rename = L"\\\\?\\" + srcDir + L"\\prefixed-rename.txt";
        auto prefixed_st = RunOp(SimpleOp(ops::OpType::Rename,
            { prefixed_rename.c_str() }, nullptr, L"prefixed-renamed.txt"));
        Check(!Exists(srcDir + L"\\prefixed-rename.txt") &&
              Exists(srcDir + L"\\prefixed-renamed.txt"),
              L"rename accepts \\\\?\\ prefixed path");
        Check(prefixed_st.last_error.empty(), L"prefixed rename reported no error");

        MakeFile(srcDir + L"\\locked-rename.txt", payload, sizeof(payload) - 1);
        HANDLE locked = CreateFileW((srcDir + L"\\locked-rename.txt").c_str(), GENERIC_READ,
                                    FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        auto failed_st = RunOp(SimpleOp(ops::OpType::Rename,
            { (srcDir + L"\\locked-rename.txt").c_str() }, nullptr, L"locked-renamed.txt"));
        if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
        Check(Exists(srcDir + L"\\locked-rename.txt") &&
              !Exists(srcDir + L"\\locked-renamed.txt"),
              L"failed rename preserves source");
        Check(failed_st.phase == ops::OpPhase::Failed && !failed_st.last_error.empty(),
              L"failed rename reports failure");
    }

    // --- 4b. Folder targeted at itself ---------------------------------------
    {
        const std::wstring self = srcDir + L"\\self-target";
        CreateDirectoryW(self.c_str(), nullptr);
        CreateDirectoryW((self + L"\\child").c_str(), nullptr);
        MakeFile(self + L"\\inside.txt", payload, sizeof(payload) - 1);
        for (const auto type : { ops::OpType::Move, ops::OpType::Copy }) {
            for (const std::wstring& dest : { self, self + L"\\child", self + L"\\" }) {
                const auto st = RunOp(SimpleOp(type, { self.c_str() }, dest.c_str()));
                Check(st.phase == ops::OpPhase::Failed &&
                      (st.last_error.find(L"自身内部") != std::wstring::npos ||
                       st.last_error.find(L"into itself") != std::wstring::npos) &&
                      Exists(self + L"\\inside.txt") && !Exists(self + L"\\self-target") &&
                      !Exists(self + L"\\child\\self-target"),
                      type == ops::OpType::Move
                          ? L"moving a folder onto itself or its subfolder is refused with a clear reason"
                          : L"copying a folder onto itself or its subfolder is refused with a clear reason");
                if (st.last_error.find(L"拒绝访问") != std::wstring::npos)
                    wprintf(L"       error: %s\n", st.last_error.c_str());
            }
        }
        DeleteFileW((self + L"\\inside.txt").c_str());
        RemoveDirectoryW((self + L"\\child").c_str());
        RemoveDirectoryW(self.c_str());
    }

    // --- 5. Recycle delete --------------------------------------------------
    {
        RunOp(SimpleOp(ops::OpType::RecycleDelete, { (dstDir + L"\\b.txt").c_str() }));
        Check(!Exists(dstDir + L"\\b.txt"), L"recycle-delete dst\\b.txt (bin check via PowerShell)");

        MakeFile(srcDir + L"\\20260817_145219.mp4", payload, sizeof(payload) - 1);
        const std::wstring prefixed = L"\\\\?\\" + srcDir + L"\\20260817_145219.mp4";
        auto st = RunOp(SimpleOp(ops::OpType::RecycleDelete, { prefixed.c_str() }));
        Check(!Exists(srcDir + L"\\20260817_145219.mp4"),
              L"recycle-delete accepts screenshot .mp4 \\\\?\\ path");
        if (!st.last_error.empty()) wprintf(L"       error: %s\n", st.last_error.c_str());
        Check(st.last_error.empty(), L"prefixed .mp4 recycle-delete reported no error");

        const std::wstring missing = L"\\\\?\\" + srcDir + L"\\already-missing.mp4";
        auto missing_st = RunOp(SimpleOp(ops::OpType::RecycleDelete, { missing.c_str() }));
        Check(missing_st.phase == ops::OpPhase::Failed && !missing_st.last_error.empty(),
              L"missing recycle-delete remains a failure");
    }

    // --- 6. Real delete -----------------------------------------------------
    {
        MakeFile(srcDir + L"\\c.txt", payload, sizeof(payload) - 1);
        RunOp(SimpleOp(ops::OpType::RealDelete, { (srcDir + L"\\c.txt").c_str() }));
        Check(!Exists(srcDir + L"\\c.txt"), L"realdelete src\\c.txt");

        MakeFile(srcDir + L"\\prefixed-realdelete.txt", payload, sizeof(payload) - 1);
        const std::wstring prefixed = L"\\\\?\\" + srcDir + L"\\prefixed-realdelete.txt";
        auto st = RunOp(SimpleOp(ops::OpType::RealDelete, { prefixed.c_str() }));
        Check(!Exists(srcDir + L"\\prefixed-realdelete.txt"),
              L"realdelete accepts \\\\?\\ prefixed path");
        Check(st.last_error.empty(), L"prefixed realdelete reported no error");

        MakeFile(srcDir + L"\\locked-realdelete.txt", payload, sizeof(payload) - 1);
        HANDLE locked = CreateFileW((srcDir + L"\\locked-realdelete.txt").c_str(), GENERIC_READ,
                                    FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        auto locked_st = RunOp(SimpleOp(ops::OpType::RealDelete,
            { (srcDir + L"\\locked-realdelete.txt").c_str() }));
        if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
        Check(Exists(srcDir + L"\\locked-realdelete.txt"),
              L"failed locked realdelete preserves source");
        Check(locked_st.phase == ops::OpPhase::Failed && !locked_st.last_error.empty(),
              L"failed locked realdelete reports failure");

        const std::wstring delete_a = srcDir + L"\\delete-a.tmp";
        const std::wstring delete_b = srcDir + L"\\delete-b.tmp";
        const std::wstring delete_c = srcDir + L"\\delete-c.tmp";
        MakeFile(delete_a, payload, sizeof(payload) - 1);
        MakeFile(delete_b, payload, sizeof(payload) - 1);
        MakeFile(delete_c, payload, sizeof(payload) - 1);
        {
            std::lock_guard<std::mutex> lock(g_status_mutex);
            g_status_history.clear();
        }
        const uint64_t previous = g_ops.Status().completed_ops;
        const uint64_t task_id = g_ops.Submit(SimpleOp(ops::OpType::RealDelete,
            { delete_a.c_str(), delete_b.c_str(), delete_c.c_str() }));
        Check(WaitOpDone(previous) && !Exists(delete_a) && !Exists(delete_b) && !Exists(delete_c),
              L"multi-item realdelete completes");

        uint64_t last_items = 0;
        bool monotonic_items = true;
        bool saw_shell_total = false;
        {
            std::lock_guard<std::mutex> lock(g_status_mutex);
            for (const auto& status : g_status_history) {
                if (status.task_id != task_id || !status.active) continue;
                monotonic_items = monotonic_items && status.completed_items >= last_items;
                last_items = status.completed_items;
                saw_shell_total = saw_shell_total ||
                    (status.total_items == 3 && status.completed_items == 3);
            }
        }
        Check(monotonic_items && saw_shell_total,
              L"Shell delete reports monotonic completed item counts");
    }

    CheckDeleteProgress(srcDir);

    // --- 7. Progress + cancel (bulk dir copy, cancel on first progress) -----
    {
        std::wstring bulk = srcDir + L"\\bulk";
        MakeDir(bulk);
        for (int i = 0; i < 400; ++i) {
            wchar_t name[64];
            swprintf_s(name, L"f%04d.bin", i);
            char buf[512] = {};
            MakeFile(bulk + L"\\" + name, buf, sizeof(buf));
        }

        uint64_t prev = g_ops.Status().completed_ops;
        g_ops.Submit(SimpleOp(ops::OpType::Copy, { bulk.c_str() }, dstDir.c_str()));

        // Wait for the op to become active, then cancel.
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            if (g_ops.Status().active) break;
            Sleep(5);
        }
        g_ops.CancelCurrent();
        bool finished = WaitOpDone(prev);
        auto st = g_ops.Status();
        wprintf(L"[INFO] cancel test: finished=%d last_error=%s bulk_at_dst=%d\n",
                finished ? 1 : 0, st.last_error.c_str(), Exists(dstDir + L"\\bulk") ? 1 : 0);
        Check(finished, L"cancel: op finished after cancel");
        Check(st.last_error == L"已取消" || !Exists(dstDir + L"\\bulk"),
              L"cancel: op cancelled or partial copy cleaned");
    }

    // --- 8. Undo: move / rename / copy --------------------------------------
    {
        // move dst\a2.txt -> src, then undo
        RunOp(SimpleOp(ops::OpType::Move, { (dstDir + L"\\a2.txt").c_str() }, srcDir.c_str()));
        Check(Exists(srcDir + L"\\a2.txt"), L"undo-move setup: a2.txt moved to src");
        Check(g_ops.CanUndo(), L"undo available after move");
        uint64_t prev = g_ops.Status().completed_ops;
        g_ops.Undo();
        WaitOpDone(prev);
        Check(Exists(dstDir + L"\\a2.txt") && !Exists(srcDir + L"\\a2.txt"),
              L"undo move: a2.txt back in dst");

        // rename src\d.txt -> d2.txt, then undo
        RunOp(SimpleOp(ops::OpType::Rename, { (srcDir + L"\\d.txt").c_str() }, nullptr, L"d2.txt"));
        Check(Exists(srcDir + L"\\d2.txt"), L"undo-rename setup: d.txt renamed");
        prev = g_ops.Status().completed_ops;
        g_ops.Undo();
        WaitOpDone(prev);
        Check(Exists(srcDir + L"\\d.txt") && !Exists(srcDir + L"\\d2.txt"),
              L"undo rename: d2.txt back to d.txt");

        MakeFile(srcDir + L"\\batch-a.txt", payload, sizeof(payload) - 1);
        MakeFile(srcDir + L"\\batch-b.txt", payload, sizeof(payload) - 1);
        ops::OpRequest batch;
        batch.type = ops::OpType::BatchRename;
        batch.sources = { srcDir + L"\\batch-a.txt", srcDir + L"\\batch-b.txt" };
        batch.new_names = { L"batch-a2.txt", L"batch-b2.txt" };
        RunOp(std::move(batch));
        Check(Exists(srcDir + L"\\batch-a2.txt") && Exists(srcDir + L"\\batch-b2.txt") &&
              !Exists(srcDir + L"\\batch-a.txt") && !Exists(srcDir + L"\\batch-b.txt"),
              L"batch-rename: two files renamed");
        prev = g_ops.Status().completed_ops;
        g_ops.Undo();
        const auto undo_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (std::chrono::steady_clock::now() < undo_deadline) {
            if (g_ops.Status().completed_ops >= prev + 2 &&
                Exists(srcDir + L"\\batch-a.txt") && Exists(srcDir + L"\\batch-b.txt") &&
                !Exists(srcDir + L"\\batch-a2.txt") && !Exists(srcDir + L"\\batch-b2.txt"))
                break;
            Sleep(10);
        }
        Check(Exists(srcDir + L"\\batch-a.txt") && Exists(srcDir + L"\\batch-b.txt") &&
              !Exists(srcDir + L"\\batch-a2.txt") && !Exists(srcDir + L"\\batch-b2.txt"),
              L"batch-rename: undo restores original names");
        Check(g_ops.Status().completed_ops == prev + 2,
              L"batch-rename: both undo operations finish before the next request");

        // copy src\d.txt -> dst, then undo (deletes the copy, to recycle bin)
        RunOp(SimpleOp(ops::OpType::Copy, { (srcDir + L"\\d.txt").c_str() }, dstDir.c_str()));
        Check(Exists(dstDir + L"\\d.txt"), L"undo-copy setup: d.txt copied");
        prev = g_ops.Status().completed_ops;
        g_ops.Undo();
        WaitOpDone(prev);
        Check(!Exists(dstDir + L"\\d.txt") && Exists(srcDir + L"\\d.txt"),
              L"undo copy: dst copy removed, source intact");
    }

    // --- 10. Undo stack persistence round-trip ------------------------------
    {
        RunOp(SimpleOp(ops::OpType::Copy, { (srcDir + L"\\d.txt").c_str() }, dstDir.c_str()));
        std::wstring json = g_ops.UndoToJson();
        ops::OpsManager other;
        Check(other.UndoFromJson(json), L"undo stack JSON round-trip parses");
        Check(other.CanUndo(), L"restored undo stack is usable");
        const std::wstring legacy = L"[{\"type\":0,\"sup\":true,\"dest\":\"C:\\\\tmp\","
            L"\"name\":\"\",\"src\":[\"C:\\\\tmp\\\\legacy.txt\"]}]";
        ops::OpsManager legacy_manager;
        Check(legacy_manager.UndoFromJson(legacy) && legacy_manager.CanUndo(),
              L"legacy undo JSON without destination mappings remains compatible");
    }

    // --- 11. Clipboard round-trip -------------------------------------------
    {
        bool copy_round_trips = true;
        bool cut_round_trips = true;
        for (int i = 0; i < 20; ++i) {
            ops::ClipboardData copy;
            const bool copy_write = ops::WriteClipboard({ srcDir + L"\\d.txt" }, false);
            const bool copy_read = ops::ReadClipboard(copy);
            const bool copy_match = copy.paths.size() == 1 &&
                copy.paths[0] == srcDir + L"\\d.txt" && !copy.cut;
            copy_round_trips = copy_round_trips && copy_write && copy_read && copy_match;
            ops::ClipboardData cut;
            const bool cut_write = ops::WriteClipboard({ srcDir + L"\\d.txt" }, true);
            const bool cut_read = ops::ReadClipboard(cut);
            const bool cut_match = cut.paths.size() == 1 &&
                cut.paths[0] == srcDir + L"\\d.txt" && cut.cut;
            cut_round_trips = cut_round_trips && cut_write && cut_read && cut_match;
        }
        Check(copy_round_trips, L"clipboard CF_HDROP + drop-effect round-trip (20x)");
        Check(cut_round_trips, L"clipboard cut effect round-trip (20x)");
        // Only an exact MOVE is a cut: COPY|MOVE means "either is fine" and
        // must paste as a copy, or a plain copy deletes the user's originals.
        Check(ops::PreferredEffectIsCut(DROPEFFECT_MOVE), L"clipboard: exact MOVE is a cut");
        Check(!ops::PreferredEffectIsCut(DROPEFFECT_COPY | DROPEFFECT_MOVE),
              L"clipboard: COPY|MOVE pastes as a copy");
        Check(!ops::PreferredEffectIsCut(DROPEFFECT_COPY), L"clipboard: COPY is not a cut");
        Check(!ops::PreferredEffectIsCut(0), L"clipboard: no effect is not a cut");
    }

    // --- 12. CopyFile2 bytes, pause/resume, cancel and atomic replacement ----
    {
        const std::wstring large_source = srcDir + L"\\large.bin";
        const std::wstring large_target = dstDir + L"\\large.bin";
        constexpr uint64_t large_bytes = 32ull * 1024ull * 1024ull;
        Check(MakePatternFile(large_source, large_bytes), L"create large transfer fixture");
        const uint64_t source_hash = FileHash(large_source);

        g_pause_when_active.store(true);
        const uint64_t previous = g_ops.Status().completed_ops;
        g_ops.Submit(SimpleOp(ops::OpType::Copy, { large_source.c_str() }, dstDir.c_str()));
        bool paused = false;
        const auto pause_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < pause_deadline) {
            if (g_ops.Status().phase == ops::OpPhase::Paused) { paused = true; break; }
            if (g_ops.Status().completed_ops > previous) break;
            Sleep(1);
        }
        const uint64_t paused_bytes = g_ops.Status().transferred_bytes;
        Sleep(120);
        Check(paused, L"CopyFile2 acknowledges pause");
        Check(!paused || g_ops.Status().transferred_bytes == paused_bytes,
              L"transferred bytes remain stable while paused");
        g_ops.ResumeCurrent();
        Check(WaitOpDone(previous), L"paused copy resumes and completes");
        const auto completed = g_ops.Status();
        Check(completed.total_bytes == large_bytes && completed.transferred_bytes == large_bytes &&
              completed.total_items == 1 && completed.completed_items == 1,
              L"CopyFile2 publishes real byte and item totals");
        Check(FileHash(large_target) == source_hash, L"copy source and destination hashes match");

        DeleteFileW(large_target.c_str());
        g_pause_when_active.store(true);
        const uint64_t cancel_previous = g_ops.Status().completed_ops;
        g_ops.Submit(SimpleOp(ops::OpType::Copy, { large_source.c_str() }, dstDir.c_str()));
        const auto cancel_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < cancel_deadline &&
               g_ops.Status().phase != ops::OpPhase::Paused) Sleep(1);
        g_ops.CancelCurrent();
        Check(WaitOpDone(cancel_previous), L"paused copy cancellation completes");
        Check(!Exists(large_target), L"cancel removes incomplete destination file");

        const char original[] = "original target must survive";
        MakeFile(large_target, original, sizeof(original) - 1);
        const uint64_t original_hash = FileHash(large_target);
        g_pause_when_active.store(true);
        auto replace = SimpleOp(ops::OpType::Copy, { large_source.c_str() }, dstDir.c_str());
        replace.collision_policy = ops::CollisionPolicy::Replace;
        const uint64_t replace_previous = g_ops.Status().completed_ops;
        g_ops.Submit(std::move(replace));
        const auto replace_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < replace_deadline &&
               g_ops.Status().phase != ops::OpPhase::Paused) Sleep(1);
        g_ops.CancelCurrent();
        Check(WaitOpDone(replace_previous), L"replacement cancellation completes");
        Check(FileHash(large_target) == original_hash,
              L"cancelled atomic replacement preserves original target");
    }

    // --- 13. Nested conflicts and apply-all ---------------------------------
    {
        const std::wstring multi_target = root + L"\\multi-dst";
        MakeDir(multi_target);
        constexpr uint64_t file_bytes = 8ull * 1024ull * 1024ull;
        const std::wstring source_a = srcDir + L"\\multi-a.bin";
        const std::wstring source_b = srcDir + L"\\multi-b.bin";
        const std::wstring source_c = srcDir + L"\\multi-c.bin";
        Check(MakePatternFile(source_a, file_bytes) &&
              MakePatternFile(source_b, file_bytes) &&
              MakePatternFile(source_c, file_bytes),
              L"create multi-file transfer fixtures");

        {
            std::lock_guard<std::mutex> lock(g_status_mutex);
            g_status_history.clear();
        }
        const uint64_t previous = g_ops.Status().completed_ops;
        const uint64_t task_id = g_ops.Submit(SimpleOp(ops::OpType::Copy,
            { source_a.c_str(), source_b.c_str(), source_c.c_str() }, multi_target.c_str()));
        Check(WaitOpDone(previous), L"multi-file copy completes");

        std::vector<ops::OpStatus> samples;
        {
            std::lock_guard<std::mutex> lock(g_status_mutex);
            for (const auto& status : g_status_history) {
                if (status.task_id == task_id && status.total_bytes > 0)
                    samples.push_back(status);
            }
        }
        bool bytes_monotonic = true;
        bool items_monotonic = true;
        bool bounded = true;
        bool saw_file_boundary = false;
        uint64_t previous_bytes = 0;
        uint64_t previous_items = 0;
        for (const auto& status : samples) {
            bytes_monotonic = bytes_monotonic && status.transferred_bytes >= previous_bytes;
            items_monotonic = items_monotonic && status.completed_items >= previous_items;
            bounded = bounded && status.transferred_bytes <= status.total_bytes;
            saw_file_boundary = saw_file_boundary ||
                (status.completed_items > 0 && status.completed_items < status.total_items);
            previous_bytes = status.transferred_bytes;
            previous_items = status.completed_items;
        }
        const auto completed = g_ops.Status();
        Check(!samples.empty() && bytes_monotonic && bounded,
              L"multi-file byte progress is monotonic and bounded");
        Check(items_monotonic && saw_file_boundary,
              L"multi-file item progress is monotonic across file boundaries");
        Check(completed.total_bytes == file_bytes * 3 &&
              completed.transferred_bytes == completed.total_bytes &&
              completed.total_items == 3 && completed.completed_items == 3,
              L"multi-file copy publishes exact final totals");
    }

    // --- 14. Nested conflicts and apply-all ---------------------------------
    {
        const std::wstring nested_source = srcDir + L"\\nested";
        const std::wstring nested_sub = nested_source + L"\\sub";
        const std::wstring nested_target = dstDir + L"\\nested";
        const std::wstring target_sub = nested_target + L"\\sub";
        MakeDir(nested_source);
        MakeDir(nested_sub);
        MakeDir(nested_target);
        MakeDir(target_sub);
        const char source_a[] = "source-a";
        const char source_b[] = "source-b";
        const char target_old[] = "target-old";
        MakeFile(nested_source + L"\\a.txt", source_a, sizeof(source_a) - 1);
        MakeFile(nested_sub + L"\\b.txt", source_b, sizeof(source_b) - 1);
        MakeFile(nested_target + L"\\a.txt", target_old, sizeof(target_old) - 1);
        MakeFile(target_sub + L"\\b.txt", target_old, sizeof(target_old) - 1);

        size_t resolved = 0;
        auto replace_status = RunConflictOp(
            SimpleOp(ops::OpType::Copy, { nested_source.c_str() }, dstDir.c_str()),
            ops::ConflictChoice::Replace, true, &resolved);
        Check(replace_status.last_error.empty() && resolved == 1,
              L"nested conflict replace apply-all resolves once");
        Check(FileHash(nested_source + L"\\a.txt") == FileHash(nested_target + L"\\a.txt") &&
              FileHash(nested_sub + L"\\b.txt") == FileHash(target_sub + L"\\b.txt"),
              L"nested conflict replace commits all source versions");

        MakeFile(nested_target + L"\\a.txt", target_old, sizeof(target_old) - 1);
        MakeFile(target_sub + L"\\b.txt", target_old, sizeof(target_old) - 1);
        resolved = 0;
        auto skip_status = RunConflictOp(
            SimpleOp(ops::OpType::Copy, { nested_source.c_str() }, dstDir.c_str()),
            ops::ConflictChoice::Skip, true, &resolved);
        Check(skip_status.last_error.empty() && resolved == 1 &&
              FileSize(nested_target + L"\\a.txt") == sizeof(target_old) - 1,
              L"nested conflict skip apply-all preserves existing files");

        resolved = 0;
        auto keep_status = RunConflictOp(
            SimpleOp(ops::OpType::Copy, { nested_source.c_str() }, dstDir.c_str()),
            ops::ConflictChoice::KeepBoth, true, &resolved);
        Check(keep_status.last_error.empty() && resolved == 1 &&
              Exists(nested_target + L"\\a - 副本.txt") &&
              Exists(target_sub + L"\\b - 副本.txt"),
              L"nested conflict keep-both apply-all creates incremented copies");
    }

    // --- Open: a batch file runs in its own folder (#49) --------------------
    {
        // The script writes a relative file, so it lands in whatever working
        // directory ShellExecuteEx gave it; only the item's folder is correct.
        const std::wstring run_dir = root + L"\\run dir";
        MakeDir(run_dir);
        const char script[] = "@echo off\r\ncd> cwd.txt\r\n";
        MakeFile(run_dir + L"\\where.bat", script, sizeof(script) - 1);
        const std::wstring marker = run_dir + L"\\cwd.txt";
        g_ops.OpenWith(run_dir + L"\\where.bat");
        for (int i = 0; i < 200 && (FileSize(marker) == ~0ull || FileSize(marker) == 0); ++i)
            Sleep(50);
        Check(FileSize(marker) != ~0ull && FileSize(marker) > 0,
              L"double-click open runs a batch file in its own folder");
    }

    // --- Open: a \\?\ path reaches the handler in plain form (#55) --------
    {
        // Pulse lists folders as \\?\C:\...; association handlers (and the
        // shell itself) reject that form and answer "Windows cannot find".
        const std::wstring run_dir = root + L"\\long form";
        MakeDir(run_dir);
        const char script[] = "@echo off\r\necho %~f0> self.txt\r\n";
        MakeFile(run_dir + L"\\self.bat", script, sizeof(script) - 1);
        const std::wstring marker = run_dir + L"\\self.txt";
        const std::wstring plain = run_dir + L"\\self.bat";
        g_ops.OpenWith(plain.rfind(L"\\\\?\\", 0) == 0 ? plain : L"\\\\?\\" + plain);
        for (int i = 0; i < 200 && (FileSize(marker) == ~0ull || FileSize(marker) == 0); ++i)
            Sleep(50);
        std::string seen;
        HANDLE file = CreateFileW(marker.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_EXISTING, 0, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            char buffer[1024]{};
            DWORD read = 0;
            ReadFile(file, buffer, sizeof(buffer) - 1, &read, nullptr);
            seen.assign(buffer, read);
            CloseHandle(file);
        }
        Check(!seen.empty(), L"opening a \\\\?\\ path runs the item");
        Check(!seen.empty() && seen.find("\\\\?\\") == std::string::npos,
              L"the opened item receives its path without the \\\\?\\ prefix");
    }

    // --- Transfer fast paths (#60) --------------------------------------------
    {
        const std::wstring src = root + L"\\fast-src";
        const std::wstring dst = root + L"\\fast-dst";
        MakeDir(src);
        MakeDir(dst);
        ops::OpRequest copy;
        copy.type = ops::OpType::Copy;
        copy.dest_dir = dst;
        for (int i = 0; i < 20; ++i) {
            const std::string body = "small file " + std::to_string(i);
            const std::wstring path = src + L"\\s" + std::to_wstring(i) + L".txt";
            MakeFile(path, body.data(), static_cast<DWORD>(body.size()));
            copy.sources.push_back(path);
        }
        const std::wstring big = src + L"\\big.bin";
        MakePatternFile(big, (64ull << 20) + 4096);
        copy.sources.push_back(big);
        NameWatcher watch(dst);
        const auto copied = RunOp(copy);
        const auto names = watch.Stop();
        bool all_there = copied.phase == ops::OpPhase::Completed && FileHash(dst + L"\\big.bin") == FileHash(big);
        for (int i = 0; i < 20; ++i) {
            const std::wstring name = L"\\s" + std::to_wstring(i) + L".txt";
            all_there = all_there && FileHash(dst + name) == FileHash(src + name);
        }
        size_t small_temporaries = 0, big_temporaries = 0;
        for (const auto& name : names) {
            if (name.find(L".pulse-copy-") == std::wstring::npos) continue;
            if (name.rfind(L"big.bin", 0) == 0) ++big_temporaries; else ++small_temporaries;
        }
        wprintf(L"[INFO] fast copy names seen=%zu small_tmp=%zu big_tmp=%zu\n", names.size(),
                small_temporaries, big_temporaries);
        Check(all_there, L"copy of 20 small files and one 64 MB+ file completes intact");
        Check(!names.empty() && small_temporaries == 0,
              L"unobstructed small files are written under their final name");
        Check(big_temporaries > 0, L"a large file still goes through a temporary name");
        bool leftovers = false;
        for (const auto& item : std::filesystem::directory_iterator(dst))
            leftovers = leftovers || item.path().filename().wstring().find(L".pulse-") != std::wstring::npos;
        Check(!leftovers, L"no .pulse- temporaries remain after the copy");

        // Replacing an existing file keeps the temporary + ReplaceFileW path.
        const std::string newer = "replacement body";
        MakeFile(src + L"\\s0.txt", newer.data(), static_cast<DWORD>(newer.size()));
        ops::OpRequest replace;
        replace.type = ops::OpType::Copy;
        replace.dest_dir = dst;
        replace.sources = {src + L"\\s0.txt"};
        NameWatcher watch_replace(dst);
        const auto replaced = RunConflictOp(replace, ops::ConflictChoice::Replace, true);
        const auto replace_names = watch_replace.Stop();
        const bool replace_temp = std::any_of(replace_names.begin(), replace_names.end(),
            [](const std::wstring& name) { return name.find(L"s0.txt.pulse-copy-") == 0; });
        Check(replaced.phase == ops::OpPhase::Completed && FileHash(dst + L"\\s0.txt") == FileHash(src + L"\\s0.txt"),
              L"replacing an existing file still succeeds");
        Check(replace_temp, L"replacement keeps the atomic temporary-file path");

        // A multi-item move on one volume renames every root (file ids survive).
        const std::wstring msrc = root + L"\\move-src";
        const std::wstring mdst = root + L"\\move-dst";
        MakeDir(msrc);
        MakeDir(mdst);
        MakeDir(msrc + L"\\sub");
        MakeFile(msrc + L"\\a.txt", "a", 1);
        MakeFile(msrc + L"\\b.txt", "b", 1);
        MakeFile(msrc + L"\\sub\\c.txt", "c", 1);
        const uint64_t id_a = FileId(msrc + L"\\a.txt"), id_b = FileId(msrc + L"\\b.txt");
        const uint64_t id_sub = FileId(msrc + L"\\sub"), id_c = FileId(msrc + L"\\sub\\c.txt");
        ops::OpRequest move;
        move.type = ops::OpType::Move;
        move.dest_dir = mdst;
        move.sources = {msrc + L"\\a.txt", msrc + L"\\b.txt", msrc + L"\\sub"};
        const auto moved = RunOp(move);
        Check(moved.phase == ops::OpPhase::Completed && !Exists(msrc + L"\\a.txt") && !Exists(msrc + L"\\sub") &&
                  Exists(mdst + L"\\sub\\c.txt"),
              L"multi-item same-volume move completes");
        Check(id_a && FileId(mdst + L"\\a.txt") == id_a && FileId(mdst + L"\\b.txt") == id_b &&
                  FileId(mdst + L"\\sub") == id_sub && FileId(mdst + L"\\sub\\c.txt") == id_c,
              L"multi-item same-volume move renames instead of copying");
        const uint64_t before_undo = g_ops.Status().completed_ops;
        g_ops.Undo();
        Check(WaitOpDone(before_undo) && FileId(msrc + L"\\a.txt") == id_a && FileId(msrc + L"\\b.txt") == id_b &&
                  FileId(msrc + L"\\sub\\c.txt") == id_c && !Exists(mdst + L"\\a.txt"),
              L"undo moves every renamed item back");

        // One conflicting root: it is asked about once, the others still rename.
        MakeFile(mdst + L"\\b.txt", "existing", 8);
        size_t prompts = 0;
        const auto mixed = RunConflictOp(move, ops::ConflictChoice::Skip, false, &prompts);
        Check(mixed.phase == ops::OpPhase::Completed && prompts == 1 && FileId(mdst + L"\\a.txt") == id_a &&
                  FileId(mdst + L"\\sub") == id_sub && Exists(msrc + L"\\b.txt") && FileSize(mdst + L"\\b.txt") == 8,
              L"a skipped conflict leaves the other roots renamed");
    }

    g_ops.Stop();

    wprintf(L"\n== ops self test: %d passed, %d failed ==\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

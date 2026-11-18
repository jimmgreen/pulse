#include "../app/delete_departure.h"
// ops_test_main.cpp — Stage 1B-1 ops-layer self test (console).
//
// Drives the full stack: OpsManager -> ShellClient -> pipe -> pulse_shell.exe
// -> IFileOperation. All file operations are confined to
// bench_data/opstest (created/cleaned by this test). Prints one PASS/FAIL
// line per check; exit code 0 iff all checks pass.
#include "../ops/ops_manager.h"
#include "../common/runtime_log.h"
#include "../ops/transfer_rate_estimator.h"
#include "../ops/verification_worker.h"
#include "../app/update_shutdown.h"
#include "../ops/clipboard.h"
#include "../ipc/shell_client.h"
#include "../common/localization.h"
#include "../common/current_user_security.h"
#include "../common/shell_id.h"
#include "../ops/recycle_empty.h"
#include <windows.h>
#include <oleidl.h>
#include <shlobj.h>
#include <winioctl.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
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
    bool fixtures_ready = true;
    for (int i = 0; i < 400; ++i) {
        wchar_t name[64];
        swprintf_s(name, L"s%04d.tmp", i);
        const std::wstring path = speed_dir + L"\\" + name;
        char buf[512] = {};
        fixtures_ready = MakeFile(path, buf, sizeof(buf)) && fixtures_ready;
        victims.push_back(path);
    }
    Check(fixtures_ready, L"all bulk delete fixtures were created");
    if (!fixtures_ready) return;
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
    const auto completed = g_ops.Status();
    Check(completed.phase == ops::OpPhase::Completed && completed.completed_items == victims.size(),
          L"bulk realdelete confirms every fixture, without failures");
    Check(std::all_of(victims.begin(), victims.end(), [](const auto& path) { return !Exists(path); }),
          L"bulk realdelete removed every fixture");
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

int RunCopyAliasOnly() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) return 2;
    l10n::SetLanguage(L"en-US");
    GUID guid{}; wchar_t guid_text[40]{};
    if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, guid_text, 40)) { CoUninitialize(); return 2; }
    const auto sandbox = SandboxRoot();
    if (sandbox.empty()) { CoUninitialize(); return 2; }
    const auto root = std::filesystem::path(sandbox).parent_path() / (std::wstring(L"copy_alias_") + guid_text);
    std::error_code ec;
    if (!std::filesystem::create_directories(root, ec) || ec) { CoUninitialize(); return 2; }
    const auto source = root / L"source", alias_dir = root / L"aliases", output = root / L"output", outside = root / L"junction-target";
    for (const auto& dir : {source,alias_dir,output,outside,outside/L"nested"})
        if (!std::filesystem::create_directory(dir,ec) || ec) { CoUninitialize(); return 2; }
    const auto file = source / L"payload.bin", expected = root / L"expected.bin";
    const char payload[] = "controlled alias payload: preserve every byte";
    for (const auto& path : {file,expected,outside/L"payload.bin",outside/L"nested"/L"leaf.bin"})
        if (!MakeFile(path.wstring(),payload,sizeof(payload))) { CoUninitialize(); return 2; }
    const auto target_id = FileId((outside/L"payload.bin").wstring());
    const auto leaf_id = FileId((outside/L"nested"/L"leaf.bin").wstring());
    Check(target_id!=0 && leaf_id!=0,L"alias fixture target identities available");
    std::atomic<bool> cancel{false};
    ops::SHA256VerificationWorker verifier;
    const auto sha_matches = [&](const std::filesystem::path& path) {
        return verifier.Verify(path.wstring(),expected.wstring(),cancel,[] {}).Success();
    };
    const auto reparse_bytes = [](const std::filesystem::path& path) {
        std::vector<BYTE> bytes(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
        HANDLE handle = CreateFileW(path.c_str(),0,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);
        if (handle == INVALID_HANDLE_VALUE) return std::vector<BYTE>{};
        DWORD got=0;
        const BOOL ok=DeviceIoControl(handle,FSCTL_GET_REPARSE_POINT,nullptr,0,bytes.data(),static_cast<DWORD>(bytes.size()),&got,nullptr);
        CloseHandle(handle);
        if (!ok) return std::vector<BYTE>{};
        bytes.resize(got); return bytes;
    };
    g_ops.SetJournalPath(L"");
    g_ops.Start([] {},false);
    const auto finish = [&] {
        g_ops.Stop(); CoUninitialize();
        wprintf(L"[INFO] retained alias fixture: %ls\n",root.c_str());
        return g_fail ? 1 : 0;
    };
    const auto run = [&](ops::OpRequest request) {
        const auto before = g_ops.Status().completed_ops;
        const auto status = RunOp(std::move(request));
        Check(status.completed_ops>before,L"alias operation reaches its own terminal result");
        return status;
    };
    const auto alias = alias_dir / L"payload.bin";
    Check(CreateHardLinkW(alias.c_str(),file.c_str(),nullptr)!=FALSE,L"OPS-02 isolated hard-link fixture");
    if (g_fail) return finish();
    for (const auto type : {ops::OpType::Copy,ops::OpType::Move}) {
        for (bool reverse : {false,true}) {
            auto request=SimpleOp(type,{(reverse?alias:file).c_str()},(reverse?source:alias_dir).c_str());
            request.collision_policy=ops::CollisionPolicy::Replace;
            const auto result=run(request);
            Check(result.phase==ops::OpPhase::Completed && result.last_error.empty() &&
                  FileId(file.wstring())==FileId(alias.wstring()) && sha_matches(file) && sha_matches(alias),
                  L"OPS-02 hard-link Copy/Move Replace preserves both same-object names");
        }
    }
    const auto junction=root/L"junction";
    Check(MakeJunction(junction.wstring(),source.wstring()),L"OPS-02 isolated directory junction fixture");
    if (g_fail) return finish();
    for (const auto type : {ops::OpType::Copy,ops::OpType::Move}) {
        for (bool reverse : {false,true}) {
            const auto input=(reverse?junction:source)/L"payload.bin";
            auto request=SimpleOp(type,{input.c_str()},(reverse?source:junction).c_str());
            request.collision_policy=ops::CollisionPolicy::Replace;
            const auto result=run(request);
            Check(result.phase==ops::OpPhase::Completed && result.last_error.empty() && sha_matches(file) &&
                  sha_matches(junction/L"payload.bin") && FileId(file.wstring())==FileId((junction/L"payload.bin").wstring()),
                  L"OPS-02 junction aliases Copy/Move Replace preserve file in both directions");
        }
    }
    const auto source_link=root/L"source-junction";
    Check(MakeJunction(source_link.wstring(),outside.wstring()),L"junction source fixture");
    if (g_fail) return finish();
    const auto before_link=reparse_bytes(source_link);
    auto copied_link=run(SimpleOp(ops::OpType::Copy,{source_link.c_str()},output.c_str()));
    const auto after_link=reparse_bytes(output/L"source-junction");
    Check(copied_link.phase==ops::OpPhase::Completed && copied_link.last_error.empty() && !before_link.empty() && after_link==before_link,
          L"source junction copy preserves reparse payload instead of materializing its target tree");
    Check(sha_matches(outside/L"payload.bin") && sha_matches(outside/L"nested"/L"leaf.bin") &&
          FileId((outside/L"payload.bin").wstring())==target_id && FileId((outside/L"nested"/L"leaf.bin").wstring())==leaf_id,
          L"junction target and nested target SHA256 and identities unchanged");
    const auto symbolic=root/L"file-symlink.bin";
    BOOL symlink=CreateSymbolicLinkW(symbolic.c_str(),file.c_str(),0x2); // ALLOW_UNPRIVILEGED_CREATE
    DWORD symlink_error=symlink?ERROR_SUCCESS:GetLastError();
    if (!symlink && symlink_error==ERROR_INVALID_PARAMETER) {
        symlink=CreateSymbolicLinkW(symbolic.c_str(),file.c_str(),0);
        symlink_error=symlink?ERROR_SUCCESS:GetLastError();
    }
    if (!symlink && (symlink_error==ERROR_PRIVILEGE_NOT_HELD || symlink_error==ERROR_ACCESS_DENIED || symlink_error==ERROR_NOT_SUPPORTED)) {
        printf("[SKIP] file symlink creation unavailable: Win32=%lu\n",symlink_error);
    } else {
        Check(symlink!=FALSE,L"file symlink fixture creation");
        if (symlink) {
            const auto original_reparse=reparse_bytes(symbolic);
            const auto result=run(SimpleOp(ops::OpType::Copy,{symbolic.c_str()},output.c_str()));
            Check(result.phase==ops::OpPhase::Completed && result.last_error.empty() && !original_reparse.empty() &&
                  reparse_bytes(output/L"file-symlink.bin")==original_reparse && sha_matches(file) && sha_matches(output/L"file-symlink.bin"),
                  L"file symlink copy retains link payload and leaves referenced content unchanged");
        }
    }
    return finish();
}

int RunRetryTemplateTest(bool memory_only);
#include "operation_skip_test.inc"
#include "undo_history_summary_test.inc"
#include "undo_history_persistence_test.inc"
#include "undo_history_progress_test.inc"
#include <unordered_set>
static int RunUndoRedoHistoryTests(bool progress_only = false) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const auto root = std::filesystem::current_path() / L"bench_data" /
        (L"undo-redo-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root / L"to");
    pulse::diagnostics::runtime::Initialize((root.parent_path() / L"undo-redo-diagnostics").wstring(), "undo_redo_test");
    ops::OpsManager manager;
    manager.Start([] {}, false);
    const auto wait = [&] {
        const auto deadline = GetTickCount64() + 15000;
        while (manager.UndoHistory().busy && GetTickCount64() < deadline) Sleep(5);
        const auto status = manager.Status();
        if (manager.UndoHistory().busy || status.phase == ops::OpPhase::Failed)
            fwprintf(stderr, L"[history diagnostic] busy=%d task=%llu phase=%d type=%d completed=%llu error=%ls\n",
                manager.UndoHistory().busy, static_cast<unsigned long long>(status.task_id),
                static_cast<int>(status.phase), static_cast<int>(status.type),
                static_cast<unsigned long long>(status.completed_items), status.last_error.c_str());
        return !manager.UndoHistory().busy;
    };
    const auto run = [&](ops::OpRequest request) {
        const auto task = manager.Submit(std::move(request));
        Check(task && wait(), L"history task reaches terminal state");
        return task;
    };
    const auto undone = [&](uint64_t task) {
        const auto snapshot = manager.UndoHistory();
        for (const auto& item : snapshot.entries) if (item.entry.task_id == task) return item.undone;
        return false;
    };
    const auto a = (root / L"a.txt").wstring();
    const auto moved = (root / L"to" / L"a.txt").wstring();
    const auto renamed = (root / L"to" / L"b.txt").wstring();
    const auto trash = (root / L"permanent.txt").wstring();
    MakeFile(a, "payload", 7); MakeFile(trash, "discard", 7);
    const auto first = run(SimpleOp(ops::OpType::Move, {a.c_str()}, (root / L"to").c_str()));
    auto rename = SimpleOp(ops::OpType::Rename, {moved.c_str()}); rename.new_name = L"b.txt";
    const auto second = run(std::move(rename));
    const auto permanent = run(SimpleOp(ops::OpType::RealDelete, {trash.c_str()}));
    const auto before = manager.UndoHistory();
    Check(std::any_of(before.entries.begin(), before.entries.end(), [&](const auto& item) {
        return item.entry.task_id == permanent && !item.entry.supported && !item.undone;
    }), L"permanent delete remains a non-undoable history record");
    const auto persisted_before_undo = manager.UndoJsonSnapshot();
    Check(manager.UndoTo(first) && wait() && Exists(a) && !Exists(renamed) && undone(first) && undone(second),
        L"multi-step undo skips unsupported delete and reverses rename before move");
    const auto persisted_after_undo = manager.UndoJsonSnapshot();
    Check(persisted_after_undo != persisted_before_undo && manager.UndoHistorySummary(false).empty() &&
        manager.UndoHistoryCount() == before.entries.size(),
        L"undo invalidates persistence and hides undone or unsupported rows from collapsed summary");
    Check(manager.RedoTo(second) && wait() && Exists(renamed) && !Exists(a) && !undone(first) && !undone(second),
        L"multi-step redo preserves original dependency order");
    const auto persisted_after_redo = manager.UndoJsonSnapshot();
    Check(persisted_after_redo != persisted_after_undo && persisted_after_redo->find(L"b.txt") != std::wstring::npos &&
        persisted_after_redo->size() == persisted_before_undo->size(),
        L"redo republishes original persisted paths after reverse operations");
    const auto replay_result = manager.UndoHistory();
    Check(replay_result.completed_sequence == 2 && replay_result.completed_steps == 2 &&
        replay_result.completed_redo && replay_result.completed_success,
        L"replay completion reports the actual successful step count");
    Check(HistoryProgressMatches(manager), L"lightweight progress matches successful redo sequence and steps");
    Check(manager.UndoTask(second) && wait() && manager.CanRedo(), L"single-task undo exposes redo");
    const auto missing = (root / L"missing.txt").wstring();
    run(SimpleOp(ops::OpType::Move, {missing.c_str()}, (root / L"to").c_str()));
    Check(manager.CanRedo() && undone(second), L"failed new operation preserves redo history");
    const auto fresh = (root / L"fresh.txt").wstring();
    const auto fresh_output = (root / L"to" / L"fresh.txt").wstring();
    MakeFile(fresh, "fresh", 5);
    const auto newer = run(SimpleOp(ops::OpType::Move, {fresh.c_str()}, (root / L"to").c_str()));
    const auto after = manager.UndoHistory();
    Check(!manager.CanRedo() && std::none_of(after.entries.begin(), after.entries.end(),
        [](const auto& item) { return item.undone; }) && std::any_of(after.entries.begin(), after.entries.end(),
        [&](const auto& item) { return item.entry.task_id == newer && item.redo_cleared; }),
        L"successful new operation clears redo and marks its own history entry");
    DeleteFileW(fresh_output.c_str());
    Check(manager.UndoTask(newer) && wait() && !undone(newer) && manager.CanUndoTask(newer),
        L"failed undo retains the original entry instead of migrating history");
    Check(HistoryProgressMatches(manager) && !manager.HistoryProgress().completed_success,
        L"lightweight progress preserves failed undo notification");
    MakeFile(fresh_output, "fresh", 5);
    Check(manager.UndoTask(newer) && wait() && Exists(fresh) && undone(newer),
        L"failed undo can be retried after its source is restored");
    const auto x = (root / L"x.txt").wstring(), y = (root / L"y.txt").wstring();
    const auto x_to = (root / L"to" / L"x.txt").wstring();
    MakeFile(x, "x", 1); MakeFile(y, "y", 1);
    const auto batch = run(SimpleOp(ops::OpType::Move, {x.c_str(), y.c_str()}, (root / L"to").c_str()));
    DeleteFileW(x_to.c_str());
    Check(manager.UndoTask(batch) && wait() && Exists(y) && !undone(batch),
        L"partially completed inverse retains its step when a later subrequest fails");
    MakeFile(x_to, "x", 1);
    Check(manager.UndoTask(batch) && wait() && Exists(x) && Exists(y) && undone(batch),
        L"retry resumes after committed subrequests without repeating them");
    Check(!manager.UndoTo(99999999) && !manager.RedoTo(permanent), L"unknown and unsupported targets are rejected");
    if (progress_only) {
        Check(HistoryProgressMatches(manager) && manager.HistoryProgress().has_entries &&
            !manager.HistoryProgress().busy && manager.CanRedo() && !manager.Status().active,
            L"rename history progress preserves final state and redo admission");
        manager.Stop();
        pulse::diagnostics::runtime::Shutdown();
        std::error_code error;
        std::filesystem::remove_all(root, error);
        Check(!error, L"isolated rename history fixture cleanup");
        wprintf(L"history-progress-behavior: %d passed, %d failed\n", g_pass, g_fail);
        CoUninitialize();
        return g_fail ? 1 : 0;
    }
    const auto copied = run(SimpleOp(ops::OpType::Copy, {x.c_str()}, (root / L"to").c_str()));
    Check(manager.UndoTask(copied) && wait() && !Exists(x_to) && manager.CanRedo(),
        L"undo copy recycles only its actual output");
    Check(manager.Redo() && wait() && Exists(x_to) && FileSize(x_to) == 1,
        L"redo copy restores the original committed output");
    const auto folder = (root / L"created").wstring();
    const auto created = run(SimpleOp(ops::OpType::CreateFolder, {folder.c_str()}));
    MakeFile(folder + L"\\preserve.txt", "contents", 8);
    Check(manager.UndoTask(created) && wait() && !Exists(folder) && manager.Redo() && wait() &&
        FileSize(folder + L"\\preserve.txt") == 8,
        L"redo created folder restores recycled contents instead of recreating an empty folder");
    const auto saved_history = manager.UndoToJson();
    Check(manager.UndoFromJson(saved_history), L"legacy history fields restore without a schema migration");
    const auto restored_history = manager.UndoHistory();
    std::unordered_set<uint64_t> restored_ids;
    bool unique = !restored_history.entries.empty();
    uint64_t maximum = 0;
    for (const auto& item : restored_history.entries) {
        unique &= item.entry.task_id != 0 && restored_ids.insert(item.entry.task_id).second;
        maximum = (std::max)(maximum, item.entry.task_id);
    }
    Check(unique, L"restored records receive unique nonzero session identities");
    const auto restored_next = restored_history.entries.front().entry.task_id;
    Check(manager.UndoTask(restored_next) && wait() && !Exists(folder) && manager.Redo() && wait() && Exists(folder),
        L"restored identity can undo and redo the exact latest persisted step");
    const auto after_restore = run(SimpleOp(ops::OpType::CreateTextFile, {(root / L"after-restore.txt").c_str()}));
    Check(after_restore > maximum, L"task allocation after restore cannot collide with restored history identities");
    {
        ops::OpsManager queued;
        Check(queued.UndoFromJson(saved_history), L"inactive manager can load existing history");
        const auto latest = queued.UndoHistory().entries.front().entry.task_id;
        queued.Submit(SimpleOp(ops::OpType::CreateTextFile, {(root / L"not-executed.txt").c_str()}));
        Check(!queued.CanUndo() && !queued.CanUndoTask(latest) && !queued.CanRedo() && queued.UndoHistory().busy,
            L"queued operations disable history controls consistently with replay admission");
    }
    Check(HistoryProgressMatches(manager) && manager.HistoryProgress().completed_sequence > 0,
        L"lightweight history preserves completed undo-redo notification state");
    manager.Stop();
    pulse::diagnostics::runtime::Shutdown();
    std::error_code error; std::filesystem::remove_all(root, error);
    Check(!error, L"isolated undo-redo fixture cleanup");
    CoUninitialize();
    wprintf(L"undo-redo: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring_view(argv[1]) == L"--undo-history-persistence-only") return RunUndoHistoryPersistenceTests();
    if (argc == 2 && std::wstring_view(argv[1]) == L"--undo-history-progress-behavior") return RunUndoRedoHistoryTests(true);
    if (argc == 2 && std::wstring_view(argv[1]) == L"--undo-history-progress-only") return RunUndoHistoryProgressTests(false);
    if (argc == 2 && std::wstring_view(argv[1]) == L"--undo-history-progress-baseline") return RunUndoHistoryProgressTests(true);
    if (argc == 2 && std::wstring_view(argv[1]) == L"--undo-history-summary-only") return RunUndoHistorySummaryTests();
    if (argc == 2 && std::wstring_view(argv[1]) == L"--undo-redo-history-only") return RunUndoRedoHistoryTests();
    if (argc == 2 && std::wstring_view(argv[1]) == L"--skip-item-only") return RunOperationSkipTests();
    if (argc == 2 && std::wstring_view(argv[1]) == L"--delete-completion-only") {
        setvbuf(stdout, nullptr, _IONBF, 0);
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const auto base = SandboxRoot();
        if (base.empty()) return 2;
        const auto root = std::filesystem::path(base).parent_path() /
            (L"delete_feedback_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(root);
        g_ops.Start([] {});
        std::optional<ops::FinishedTask> finished_result;
        const auto wait = [&](uint64_t task_id, uint64_t timeout) {
            const auto deadline = GetTickCount64() + timeout;
            while (GetTickCount64() < deadline) {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                for (const auto& finished : g_ops.DrainFinishedTasks()) {
                    if (finished.task_id == task_id) { finished_result = finished; return true; }
                }
                MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
            }
            const auto status = g_ops.Status();
            fwprintf(stderr, L"[DELETE WAIT] phase=%u active=%d authorization=%u completed=%llu current=%s error=%s\n",
                static_cast<unsigned>(status.phase), status.active, static_cast<unsigned>(status.authorization),
                static_cast<unsigned long long>(status.completed_ops), status.current_item.c_str(), status.last_error.c_str());
            return false;
        };
        for (unsigned scenario = 0; scenario < 3; ++scenario) {
            const auto locked = root / (L"locked" + std::to_wstring(scenario));
            const auto free = root / (L"free" + std::to_wstring(scenario));
            Check(MakeFile(locked.wstring(), "keep", 4) && MakeFile(free.wstring(), "remove", 6),
                L"create isolated locked/free deletion fixture");
            HANDLE handle = CreateFileW(locked.c_str(), GENERIC_READ, FILE_SHARE_READ,
                nullptr, OPEN_EXISTING, 0, nullptr);
            Check(handle != INVALID_HANDLE_VALUE, L"deny delete sharing on isolated file");
            std::vector<std::wstring> leaving;
            uint64_t since = 0, done = 0, task = 0;
            ops::OpRequest request;
            request.type = ops::OpType::RealDelete;
            if (scenario != 0) request.sources.push_back(free.wstring());
            if (scenario != 1) request.sources.push_back(locked.wstring());
            g_ops.DrainCompletions();
            finished_result.reset();
            const auto submitted = g_ops.Submit(request);
            Check(submitted != 0, L"normal operations manager accepts the delete request");
            Check(leaving.empty(), L"pending deletion never starts a row departure");
            const bool terminal = wait(submitted, 10000);
            Check(terminal && finished_result && finished_result->task_id == submitted,
                L"isolated delete task publishes its own FinishedTask terminal result");
            if (!terminal) {
                g_ops.CancelCurrent();
                if (!wait(submitted, 5000)) {
                    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
                    g_ops.Stop();
                    CoUninitialize();
                    return 1;
                }
            }
            for (const auto& completed : g_ops.DrainCompletions())
                pulse::app::RecordConfirmedDelete(completed, leaving, since, done, task, GetTickCount64());
            Check(std::find(leaving.begin(), leaving.end(), locked.wstring()) == leaving.end() &&
                std::filesystem::exists(locked), L"failed occupied file remains visible and exists on disk");
            Check((std::find(leaving.begin(), leaving.end(), free.wstring()) != leaving.end()) == (scenario != 0),
                L"success and mixed result dispatch animate only confirmed deleted files");
            ops::CompletedOperation refresh;
            refresh.task_id = 123; refresh.type = ops::OpType::RealDelete;
            refresh.sources = {locked.wstring()}; refresh.refresh_only = true;
            Check(!pulse::app::RecordConfirmedDelete(refresh, leaving, since, done, task, GetTickCount64()),
                L"refresh-only completion cannot hide an occupied file");
            if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        }
        g_ops.Stop();
        std::filesystem::remove_all(root);
        CoUninitialize();
        return g_fail ? 1 : 0;
    }

    if (argc == 2 && std::wstring_view(argv[1]) == L"--copy-alias-only") return RunCopyAliasOnly();
    if (argc > 1 && std::wstring_view(argv[1]) == L"--retry-template-only") return RunRetryTemplateTest(false);
    if (argc > 1 && std::wstring_view(argv[1]) == L"--retry-template-memory") return RunRetryTemplateTest(true);
    setvbuf(stdout, nullptr, _IONBF, 0);
    // Assertions below use the Simplified texts (" - 副本", "已取消"); the
    // English forms are checked separately.
    l10n::SetLanguage(L"zh-CN");
    fprintf(stderr, "[test] start\n");

    // Which requests Explorer's engine runs (a phone's items: shell ids).
    {
        const std::wstring phone = L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\\\?\\usb#vid_04e8&pid_6860#x#{6ac27878-a6fa-4155-ba85-f98f491d4f33}\\SID-{10001,,1}\\{A}";
        Check(ops::IsShellItemId(phone) && !ops::IsShellItemId(L"C:\\x") && !ops::IsShellItemId(L"::") &&
                  !ops::IsShellItemId(L"pulse:device:k\\s"),
              L"shell routing: only a parsing name starting with :: is a shell item");
        Check(ops::IsShellTransferRequest(SimpleOp(ops::OpType::Copy, {L"C:\\a.txt"}, phone.c_str())) &&
                  ops::IsShellTransferRequest(SimpleOp(ops::OpType::Move, {phone.c_str()}, L"C:\\dest")) &&
                  ops::IsShellTransferRequest(SimpleOp(ops::OpType::CreateFolder, {}, phone.c_str(), L"x")) &&
                  !ops::IsShellTransferRequest(SimpleOp(ops::OpType::Copy, {L"C:\\a.txt"}, L"D:\\dest")) &&
                  !ops::IsShellTransferRequest(SimpleOp(ops::OpType::CreateFolder, {L"C:\\new"})),
              L"shell routing: a shell id on either side picks the shell engine, paths never do");
    }

    // A phone item's id is its ID list ("::pidl:<hex>"): its parsing name does
    // not parse back below the device (SHParseDisplayName: E_INVALIDARG).
    {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        wchar_t windows_dir[MAX_PATH] = {};
        GetWindowsDirectoryW(windows_dir, MAX_PATH);
        PIDLIST_ABSOLUTE original = nullptr;
        PIDLIST_ABSOLUTE decoded = nullptr;
        std::wstring id;
        if (SUCCEEDED(SHParseDisplayName(windows_dir, nullptr, &original, 0, nullptr)) && original)
            id = pulse::shell_id::EncodePidl(original);
        Check(pulse::shell_id::IsPidlId(id) && ops::IsShellItemId(id) &&
                  id.find_first_of(L"\\/") == std::wstring::npos &&
                  SUCCEEDED(pulse::shell_id::ParseId(id, &decoded)) && decoded && ILIsEqual(original, decoded),
              L"shell id: an ID list round-trips through its id, a shell id with no path separators");
        CoTaskMemFree(decoded);
        decoded = nullptr;
        IShellItem* item = nullptr;
        LPWSTR path = nullptr;
        bool same_path = false;
        if (SUCCEEDED(pulse::shell_id::CreateItem(id, IID_PPV_ARGS(&item))) && item &&
            SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path)
            same_path = _wcsicmp(path, windows_dir) == 0;
        Check(same_path && pulse::shell_id::FromItem(item) == id,
              L"shell id: the item made from an id is the same item, and gives the same id");
        if (path) CoTaskMemFree(path);
        if (item) item->Release();
        CoTaskMemFree(original);
        bool all_rejected = true;
        for (const wchar_t* bad : {L"::pidl:0", L"::pidl:00", L"::pidl:0G00", L"::pidl:0400", L"::pidl:030000",
                                   L"::pidl:01000000", L"::pidl:0000FF"}) {
            if (pulse::shell_id::ParseId(bad, &decoded) != E_INVALIDARG || decoded) all_rejected = false;
            CoTaskMemFree(decoded);
            decoded = nullptr;
        }
        Check(all_rejected, L"shell id: a malformed ID list is refused, never read past its end");
        Check(SUCCEEDED(pulse::shell_id::ParseId(L"::pidl:0000", &decoded)) && decoded && ILIsEmpty(decoded),
              L"shell id: the empty ID list (the desktop) decodes");
        CoTaskMemFree(decoded);
        if (SUCCEEDED(co)) CoUninitialize();
    }

    if (argc > 1 && std::wstring_view(argv[1]) == L"--shell-transfer") {
        // A phone's operations without a phone: a local folder named by its
        // shell parsing name under This PC is a shell id too, so the same
        // path runs (OpsManager -> REQ_SHELL_TRANSFER -> IFileOperation),
        // with Pulse's conflict prompts. Sandbox only.
        const auto root = std::filesystem::path(SandboxRoot()).parent_path() /
            (L"shell-transfer-" + std::to_wstring(GetCurrentProcessId()));
        const auto source = root / L"source";
        const auto target = root / L"target";
        const auto back = root / L"back";
        std::filesystem::create_directories(source);
        std::filesystem::create_directories(target);
        std::filesystem::create_directories(back);
        const auto shell_id = [](const std::filesystem::path& path) {
            return L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\" + path.wstring();
        };
        const auto file = source / L"sample.txt";
        const char content[] = "shell transfer";
        Check(MakeFile(file.wstring(), content, sizeof(content)), L"shell transfer: fixture");
        g_ops.Start([] {});
        g_ops.DrainCompletions();
        const std::wstring target_id = shell_id(target);
        auto status = RunOp(SimpleOp(ops::OpType::Copy, {file.c_str()}, target_id.c_str()));
        Check(status.phase == ops::OpPhase::Completed && Exists((target / L"sample.txt").wstring()) &&
                  Exists(file.wstring()) && !status.can_pause,
              L"shell transfer: copy into a shell folder");
        auto completions = g_ops.DrainCompletions();
        Check(completions.size() == 1 && completions[0].refresh_only, L"shell transfer: a shell target only re-lists");
        size_t prompts = 0;
        status = RunConflictOp(SimpleOp(ops::OpType::Copy, {file.c_str()}, target_id.c_str()),
                               ops::ConflictChoice::KeepBoth, false, &prompts);
        Check(status.phase == ops::OpPhase::Completed && prompts == 1 &&
                  Exists((target / L"sample.txt").wstring()) && Exists((target / L"sample (2).txt").wstring()),
              L"shell transfer: a name taken asks Pulse's dialog, keep both names it as Explorer does");
        if (!Exists((target / L"sample (2).txt").wstring())) {
            fwprintf(stderr, L"[test] keep both: phase=%d prompts=%zu error=%ls\n", static_cast<int>(status.phase),
                     prompts, status.last_error.c_str());
            for (const auto& entry : std::filesystem::directory_iterator(target))
                fwprintf(stderr, L"[test]   target has: %ls\n", entry.path().filename().c_str());
        }
        status = RunConflictOp(SimpleOp(ops::OpType::Copy, {file.c_str()}, target_id.c_str()),
                               ops::ConflictChoice::Skip, false, &prompts);
        Check(prompts == 1 && !Exists((target / L"sample (3).txt").wstring()),
              L"shell transfer: skip copies nothing");
        g_ops.DrainCompletions();
        const std::wstring item_id = shell_id(target / L"sample.txt");
        status = RunOp(SimpleOp(ops::OpType::Move, {item_id.c_str()}, back.c_str()));
        completions = g_ops.DrainCompletions();
        Check(status.phase == ops::OpPhase::Completed && Exists((back / L"sample.txt").wstring()) &&
                  !Exists((target / L"sample.txt").wstring()),
              L"shell transfer: move from a shell folder to a local one");
        Check(completions.size() == 1 && !completions[0].refresh_only && completions[0].destinations.size() == 1 &&
                  _wcsicmp(completions[0].destinations[0].c_str(), (back / L"sample.txt").c_str()) == 0,
              L"shell transfer: a local target names its arrivals");
        auto create = SimpleOp(ops::OpType::CreateFolder, {}, target_id.c_str(), L"新建文件夹");
        create.refresh_directories.push_back(L"pulse:device:test\\storage");
        status = RunOp(std::move(create));
        completions = g_ops.DrainCompletions();
        Check(status.phase == ops::OpPhase::Completed && Exists((target / L"新建文件夹").wstring()),
              L"shell transfer: new folder in a shell folder");
        Check(completions.size() == 1 && completions[0].destinations.size() == 1 &&
                  completions[0].destinations[0] == L"pulse:device:test\\storage\\新建文件夹",
              L"shell transfer: the new folder is named by its Pulse path (rename in place)");
        const std::wstring folder_id = shell_id(target / L"新建文件夹");
        status = RunOp(SimpleOp(ops::OpType::RealDelete, {folder_id.c_str()}));
        Check(status.phase == ops::OpPhase::Completed && !Exists((target / L"新建文件夹").wstring()),
              L"shell transfer: delete in a shell folder");

        // The same operations with ID-list ids, as a phone's items travel now.
        {
            const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            const auto pidl_id = [](const std::filesystem::path& path) {
                PIDLIST_ABSOLUTE pidl = nullptr;
                std::wstring id;
                if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr)) && pidl)
                    id = pulse::shell_id::EncodePidl(pidl);
                CoTaskMemFree(pidl);
                return id;
            };
            const std::wstring target_pidl = pidl_id(target);
            Check(pulse::shell_id::IsPidlId(target_pidl), L"shell transfer (ID list): fixture id");
            g_ops.DrainCompletions();
            status = RunOp(SimpleOp(ops::OpType::Copy, {(back / L"sample.txt").c_str()}, target_pidl.c_str()));
            Check(status.phase == ops::OpPhase::Completed && Exists((target / L"sample.txt").wstring()),
                  L"shell transfer (ID list): copy into a folder named by its ID list");
            const std::wstring sample_pidl = pidl_id(target / L"sample.txt");
            status = RunOp(SimpleOp(ops::OpType::Copy, {sample_pidl.c_str()}, target_pidl.c_str()));
            Check(status.phase == ops::OpPhase::Completed && Exists((target / L"sample - 副本.txt").wstring()),
                  L"shell transfer (ID list): a copy into its own folder is a copy - 副本, as in Explorer");
            if (!Exists((target / L"sample - 副本.txt").wstring()))
                fwprintf(stderr, L"[test] pidl self copy: phase=%d error=%ls\n", static_cast<int>(status.phase),
                         status.last_error.c_str());
            const std::wstring copy_pidl = pidl_id(target / L"sample - 副本.txt");
            status = RunOp(SimpleOp(ops::OpType::Move, {copy_pidl.c_str()}, back.c_str()));
            Check(status.phase == ops::OpPhase::Completed && Exists((back / L"sample - 副本.txt").wstring()) &&
                      !Exists((target / L"sample - 副本.txt").wstring()),
                  L"shell transfer (ID list): move out to a local folder");
            status = RunOp(SimpleOp(ops::OpType::CreateFolder, {}, target_pidl.c_str(), L"列表文件夹"));
            Check(status.phase == ops::OpPhase::Completed && Exists((target / L"列表文件夹").wstring()),
                  L"shell transfer (ID list): new folder");
            const std::wstring new_folder_pidl = pidl_id(target / L"列表文件夹");
            status = RunOp(SimpleOp(ops::OpType::RealDelete, {new_folder_pidl.c_str()}));
            Check(status.phase == ops::OpPhase::Completed && !Exists((target / L"列表文件夹").wstring()),
                  L"shell transfer (ID list): delete");
            g_ops.DrainCompletions();
            if (SUCCEEDED(co)) CoUninitialize();
        }
        g_ops.Stop();
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        return g_fail ? 1 : 0;
    }

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

    if (argc > 1 && std::wstring_view(argv[1]) == L"--external-undo") {
        // AI tools (ai_tools.h): an item made outside the queue is one undo
        // step, and UndoGroup undoes it only while its group is the newest.
        const auto root = std::filesystem::path(SandboxRoot()).parent_path() /
            (L"external-undo-" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directories(root);
        const auto zip = root / L"made outside.zip";
        const auto folder = root / L"user folder";
        const char content[] = "external create";
        Check(MakeFile(zip.wstring(), content, sizeof(content)), L"external: fixture created");
        g_ops.Start([] {});
        constexpr uint64_t kGroup = 0x8000000000000123ull;
        g_ops.RecordExternalCreate(zip.wstring(), kGroup, L"新建压缩包（test）");
        Check(g_ops.TopUndoGroup() == kGroup && g_ops.CanUndo(), L"external: recorded as the newest undo step");
        Check(g_ops.UndoLabel() == L"撤销新建压缩包（test）", L"external: the undo label is the given one");
        const auto completions = g_ops.DrainCompletions();
        Check(completions.size() == 1 && completions[0].refresh_only && completions[0].destinations.size() == 1 &&
                  completions[0].destinations[0] == zip.wstring(),
              L"external: one refresh-only completion for the new item");
        Check(g_ops.UndoGroup(kGroup + 1) == ops::OpsManager::GroupUndo::NotOnTop &&
                  g_ops.UndoGroup(0) == ops::OpsManager::GroupUndo::NotOnTop && Exists(zip.wstring()),
              L"external: another group (or none) does not undo it");
        // A step of the user's on top: the AI group must not undo it.
        auto status = RunOp(SimpleOp(ops::OpType::CreateFolder, {folder.c_str()}));
        Check(status.phase == ops::OpPhase::Completed && Exists(folder.wstring()), L"external: a user step on top");
        Check(g_ops.TopUndoGroup() == 0 && g_ops.UndoGroup(kGroup) == ops::OpsManager::GroupUndo::NotOnTop &&
                  Exists(folder.wstring()) && Exists(zip.wstring()),
              L"external: the AI group does not undo the user's step");
        auto completed = g_ops.Status().completed_ops;
        g_ops.Undo();
        Check(WaitOpDone(completed) && !Exists(folder.wstring()), L"external: the user undoes their own step");
        completed = g_ops.Status().completed_ops;
        Check(g_ops.UndoGroup(kGroup) == ops::OpsManager::GroupUndo::Started, L"external: now the AI group is on top");
        Check(WaitOpDone(completed) && !Exists(zip.wstring()), L"external: its undo recycles the item");
        Check(!g_ops.CanUndo() && g_ops.UndoGroup(kGroup) == ops::OpsManager::GroupUndo::Unavailable,
              L"external: nothing left to undo");
        g_ops.DrainCompletions();   // the undo's own recycle completion
        Check(g_ops.TryPrepareForUpdate(), L"external: update preparation on an idle queue");
        g_ops.RecordExternalCreate(zip.wstring(), kGroup, L"x");
        const bool completion_recorded = !g_ops.DrainCompletions().empty();
        g_ops.CancelUpdatePreparation();
        // Checked after preparation ends: CanUndo() is false during it anyway.
        Check(!completion_recorded && !g_ops.CanUndo(), L"external: nothing recorded while preparing an update");
        g_ops.Stop();
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
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
                  }) == app::kUpdateShutdownCloseFailed,
                  L"failed window close rejects update installation");
            Check(app::RequestUpdateShutdown(idle, false, [] { return true; }) ==
                      app::kUpdateShutdownAccepted,
                  L"failed close restores admission and allows a later update retry");
            Check(app::RequestUpdateShutdown(idle, false, [] { return true; }) ==
                      app::kUpdateShutdownAccepted,
                  L"remaining shared-core windows accept update shutdown while admission stays closed");
            Check(!app::RequestUpdateLaunch(idle, false, [] {}),
                  L"another installer cannot launch while shared-core shutdown is prepared");
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
    const bool conflict_snapshot_only = argc > 1 && std::wstring_view(argv[1]) == L"--conflict-snapshot-only";
    const bool copy_diagnostics_only = argc > 1 && std::wstring_view(argv[1]) == L"--copy-diagnostics-only";
    const bool same_folder_only = argc > 1 && std::wstring_view(argv[1]) == L"--same-folder-only";
    const bool isolated_fixture = conflict_snapshot_only || copy_diagnostics_only || same_folder_only;
    size_t conflict_workload_count = 128;
    if (conflict_snapshot_only && argc > 2) {
        size_t parsed = 0;
        bool valid = argv[2][0] != L'\0';
        for (const wchar_t* digit = argv[2]; valid && *digit; ++digit) {
            if (*digit < L'0' || *digit > L'9') { valid = false; break; }
            parsed = parsed * 10 + static_cast<size_t>(*digit - L'0');
            if (parsed > 4096) valid = false;
        }
        if (!valid || parsed < 16 || argc > 3) {
            fprintf(stderr, "[test] conflict count must be an integer from 16 through 4096\n");
            return 2;
        }
        conflict_workload_count = parsed;
    }
    const auto sandbox_root = SandboxRoot();
    const std::wstring root = isolated_fixture && !sandbox_root.empty()
        ? (std::filesystem::path(sandbox_root).parent_path() /
           ((copy_diagnostics_only ? L"copy_diagnostics_" : L"conflict_snapshot_") + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()))).wstring()
        : sandbox_root;
    if (root.empty()) {
        fprintf(stderr, "[test] cannot locate bench_data/opstest\n");
        return 2;
    }
    std::wstring srcDir = root + L"\\src";
    std::wstring dstDir = root + L"\\dst";
    // Exact, workspace-contained sandbox path.
    std::error_code cleanup_error;
    if (!isolated_fixture) std::filesystem::remove_all(std::filesystem::path(root), cleanup_error);
    if (!cleanup_error) std::filesystem::create_directories(srcDir, cleanup_error);
    if (!cleanup_error) std::filesystem::create_directories(dstDir, cleanup_error);
    if (cleanup_error) {
        fprintf(stderr, "[test] cannot prepare isolated fixture: %s\n", cleanup_error.message().c_str());
        return 2;
    }

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

    if (argc > 1 && std::wstring_view(argv[1]) == L"--copy-diagnostics-only") {
        const auto log_root = root + L"\\copy-diagnostics";
        Check(diagnostics::runtime::Initialize(log_root, "copy_test"), L"copy diagnostics initialized");
        const auto source = srcDir + L"\\diagnostic.bin";
        Check(MakePatternFile(source, 64ull * 1024 * 1024), L"diagnostic source created");
        g_ops.SetVerifyCopies(false);
        const auto success = RunOp(SimpleOp(ops::OpType::Copy, {source.c_str()}, dstDir.c_str()));
        Check(success.phase == ops::OpPhase::Completed && FileHash(source) == FileHash(dstDir + L"\\diagnostic.bin"),
              L"diagnostic copy preserves bytes");
        const auto missing = srcDir + L"\\missing.bin";
        const auto failed = RunOp(SimpleOp(ops::OpType::Copy, {missing.c_str()}, dstDir.c_str()));
        Check(failed.phase == ops::OpPhase::Failed, L"diagnostic copy failure retained");
        const auto cancel_dest = dstDir + L"\\cancel";
        MakeDir(cancel_dest);
        const auto previous = g_ops.Status().completed_ops;
        g_pause_when_active.store(true);
        g_ops.Submit(SimpleOp(ops::OpType::Copy, {source.c_str()}, cancel_dest.c_str()));
        const auto wait_start = GetTickCount64();
        while (g_pause_when_active.load() && GetTickCount64() - wait_start < 5000) Sleep(10);
        Check(!g_pause_when_active.load(), L"diagnostic copy pause observed");
        Sleep(5500); // No copy callbacks: the independent monitor must still report.
        g_ops.CancelCurrent();
        Check(WaitOpDone(previous) && g_ops.Status().phase == ops::OpPhase::Failed &&
              g_ops.Status().last_error.find(l10n::Get(l10n::StringId::Canceled)) != std::wstring::npos,
              L"diagnostic paused copy cancels");
        g_ops.Stop();
        Check(diagnostics::runtime::Flush(), L"copy diagnostics flushed");
        diagnostics::runtime::Shutdown();
        std::string logs;
        for (const auto& item : std::filesystem::recursive_directory_iterator(log_root)) {
            if (!item.is_regular_file()) continue;
            std::ifstream file(item.path(), std::ios::binary);
            logs.append(std::istreambuf_iterator<char>(file), {});
        }
        const auto count = [&](const std::string& needle) {
            size_t found = 0, offset = 0;
            while ((offset = logs.find(needle, offset)) != std::string::npos) { ++found; offset += needle.size(); }
            return found;
        };
        Check(count("\"event\":\"copy_monitor_begin\"") == 3 && count("\"event\":\"copy_monitor_end\"") == 3,
              L"copy diagnostic start/end cover success failure cancellation");
        Check(count("\"event\":\"copy_api_begin\"") >= 1 && count("\"event\":\"copy_api_end\"") >= 1,
              L"real large copy logs API boundaries");
        Check(count("\"event\":\"copy_heartbeat\"") >= 1 && count("\"event\":\"copy_heartbeat\"") <= 3 &&
              logs.find("\"paused\":1") != std::string::npos && logs.find("\"interval_bytes\":0") != std::string::npos,
              L"callback-free pause produces throttled heartbeat");
        const auto event_has = [&](const char* event, std::initializer_list<const char*> fields) {
            const std::string event_key = std::string("\"event\":\"") + event + "\"";
            size_t offset = 0;
            while ((offset = logs.find(event_key, offset)) != std::string::npos) {
                const auto end = logs.find('\n', offset);
                const auto line = logs.substr(offset, end == std::string::npos ? end : end - offset);
                const bool matches = std::all_of(fields.begin(), fields.end(),
                    [&](const char* field) { return line.find(field) != std::string::npos; });
                if (matches) return true;
                offset += event_key.size();
            }
            return false;
        };
        // One large serial copy queries the target classification, not a source thread-count probe.
        Check(event_has("copy_storage_probe", {"\"endpoint\":2", "\"storage\":", "\"error\":",
                  "\"bus_type\":", "\"bus_query_error\":", "\"seek_query_error\":"}),
              L"storage evidence includes target endpoint bus and query errors");
        Check(event_has("copy_buffer_policy", {"\"file_bytes\":67108864", "\"memory_known\":1",
                  "\"unbuffered\":", "\"available_bytes\":", "\"remaining_large_bytes\":"}),
              L"buffer decision records real file size memory and buffering mode");
        Check(event_has("copy_device_topology", {"\"physical_known\":", "\"shared_physical_device\":"}),
              L"topology records explicit knowledge and shared-device result");
        Check(event_has("copy_preparation_end", {"\"task\":", "\"elapsed_ms\":"}),
              L"preparation boundary records task and elapsed time");
        Check(logs.find("diagnostic.bin") == std::string::npos, L"diagnostic events omit filename");
        CoUninitialize();
        return g_fail ? 1 : 0;
    }

    if (argc > 1 && std::wstring_view(argv[1]) == L"--conflict-snapshot-only") {
        wprintf(L"[INFO] conflict fixture: %ls\n", root.c_str());
        Check(diagnostics::runtime::Initialize(root + L"\\conflict-diagnostics", "conflict_test"), L"snapshot diagnostics initialized");
        const auto finish = [&] {
            g_ops.Stop();
            diagnostics::runtime::Flush(); diagnostics::runtime::Shutdown();
            CoUninitialize();
            return g_fail ? 1 : 0;
        };
        if (g_fail) return finish();
        for (const auto choice : {ops::ConflictChoice::Skip, ops::ConflictChoice::Replace,
                                  ops::ConflictChoice::KeepBoth}) {
            const auto source = srcDir + L"\\snapshot-" + std::to_wstring(static_cast<int>(choice));
            const auto target = dstDir + L"\\snapshot-" + std::to_wstring(static_cast<int>(choice));
            MakeDir(source); MakeDir(target);
            ops::OpRequest request;
            request.type = ops::OpType::Copy; request.dest_dir = target;
            for (size_t i = 0; i < conflict_workload_count; ++i) {
                const auto name = L"\\item-" + std::to_wstring(i) + L".txt";
                Check(MakeFile(source + name, "new", 3) && MakeFile(target + name, "old", 3), L"snapshot fixture");
                if (g_fail) return finish();
                request.sources.push_back(source + name);
            }
            const auto previous = g_ops.Status().completed_ops;
            const auto begin = std::chrono::steady_clock::now();
            g_ops.Submit(request);
            size_t count = 0; uint64_t token = 0; bool counts_ok = true;
            while (g_ops.Status().completed_ops == previous &&
                   std::chrono::steady_clock::now() - begin < std::chrono::seconds(30 + conflict_workload_count / 16)) {
                if (const auto info = g_ops.PendingConflict(); info && info->token != token) {
                    token = info->token;
                    counts_ok = counts_ok && count < conflict_workload_count && info->remaining == conflict_workload_count - count;
                    ++count;
                    g_ops.ResolveConflict(token, choice, false);
                }
                Sleep(1);
            }
            Check(count == conflict_workload_count && counts_ok && g_ops.Status().completed_ops > previous &&
                  g_ops.Status().last_error.empty(), L"snapshot: real conflicts count down and complete");
            bool content_ok = true;
            for (const auto& file : request.sources) {
                const auto name = std::filesystem::path(file).filename().wstring();
                if (choice == ops::ConflictChoice::Replace)
                    content_ok = content_ok && FileHash(file) == FileHash(target + L"\\" + name);
                else {
                    const auto original = target + L"\\" + name;
                    content_ok = content_ok && FileHash(file) != FileHash(original) && FileSize(original) == 3;
                }
            }
            size_t outputs = 0, new_copies = 0;
            for (const auto& item : std::filesystem::directory_iterator(target)) {
                ++outputs;
                if (!Exists(source + L"\\" + item.path().filename().wstring())) {
                    ++new_copies;
                    content_ok = content_ok && item.is_regular_file() &&
                        FileHash(item.path().wstring()) == FileHash(request.sources.front());
                }
            }
            if (choice == ops::ConflictChoice::KeepBoth) content_ok = content_ok && new_copies == conflict_workload_count;
            Check(content_ok && outputs == (choice == ops::ConflictChoice::KeepBoth ? conflict_workload_count * 2 : conflict_workload_count),
                  L"snapshot: replace, skip and keep-both preserve expected outputs");
            printf("[INFO] conflict%zu choice=%d elapsed=%.6fs\n", conflict_workload_count, static_cast<int>(choice),
                   std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count());
        }
        for (const auto choice : {ops::ConflictChoice::KeepBoth, ops::ConflictChoice::Skip}) {
            const auto name = L"folder-" + std::to_wstring(static_cast<int>(choice));
            const auto folder = srcDir + L"\\" + name;
            const auto target = dstDir + L"\\directory-case-" + std::to_wstring(static_cast<int>(choice));
            MakeDir(folder); MakeDir(target);
            Check(MakeFile(folder + L"\\child.txt", "new", 3) &&
                  MakeFile(target + L"\\" + name, "old", 3), L"directory snapshot fixture");
            if (g_fail) return finish();
            size_t resolved = 0;
            const auto result = RunConflictOp(SimpleOp(ops::OpType::Copy, {folder.c_str()}, target.c_str()),
                                               choice, false, &resolved);
            bool copied_child = false;
            for (const auto& item : std::filesystem::recursive_directory_iterator(target)) {
                if (item.is_regular_file() && item.path().filename() == L"child.txt")
                    copied_child = FileHash(item.path().wstring()) == FileHash(folder + L"\\child.txt");
            }
            Check(resolved == 1 && result.last_error.empty() &&
                  copied_child == (choice == ops::ConflictChoice::KeepBoth) &&
                  FileSize(target + L"\\" + name) == 3,
                  L"snapshot: directory rename updates child target; directory skip omits child");
        }
        const auto a = srcDir + L"\\dynamic-a.txt";
        const auto b = srcDir + L"\\dynamic-b.txt";
        Check(MakeFile(a, "new", 3) && MakeFile(b, "new", 3) &&
              MakeFile(dstDir + L"\\dynamic-a.txt", "old", 3), L"dynamic snapshot fixture");
        if (g_fail) return finish();
        const auto previous = g_ops.Status().completed_ops;
        g_ops.Submit(SimpleOp(ops::OpType::Copy, {a.c_str(), b.c_str()}, dstDir.c_str()));
        size_t count = 0; uint64_t token = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (g_ops.Status().completed_ops == previous && std::chrono::steady_clock::now() < deadline) {
            if (const auto info = g_ops.PendingConflict(); info && info->token != token) {
                token = info->token;
                if (++count == 1) Check(MakeFile(dstDir + L"\\dynamic-b.txt", "old", 3), L"dynamic destination created");
                g_ops.ResolveConflict(token, ops::ConflictChoice::Skip, false);
            }
            Sleep(1);
        }
        Check(count == 2 && g_ops.Status().completed_ops > previous &&
              FileHash(b) != FileHash(dstDir + L"\\dynamic-b.txt"),
              L"snapshot: destination created after snapshot is still a real conflict");
        size_t cancelled_count = 0;
        RunConflictOp(SimpleOp(ops::OpType::Copy, {a.c_str()}, dstDir.c_str()),
                      ops::ConflictChoice::Cancel, false, &cancelled_count);
        Check(cancelled_count == 1 && FileHash(a) != FileHash(dstDir + L"\\dynamic-a.txt"),
              L"snapshot: cancel preserves existing destination");
        return finish();
    }

    if (argc > 1 && std::wstring_view(argv[1]) == L"--restore-index-only") {
        const auto one = srcDir + L"\\restore-index-one.txt";
        const auto two = srcDir + L"\\restore-index-two.txt";
        const auto three = srcDir + L"\\restore-index-three.txt";
        MakeFile(one, "one", 3); MakeFile(two, "second", 6); MakeFile(three, "third", 5);
        const auto deleted = RunOp(SimpleOp(ops::OpType::RecycleDelete, {one.c_str(), two.c_str(), three.c_str()}));
        Check(deleted.last_error.empty() && !Exists(one) && !Exists(two) && !Exists(three),
              L"restore index fixture recycles only three isolated files");
        const auto restored = RunOp(SimpleOp(ops::OpType::RestoreRecycle, {one.c_str(), two.c_str(), three.c_str()}));
        Check(restored.last_error.empty() && FileSize(one) == 3 && FileSize(two) == 6 && FileSize(three) == 5,
              L"real shell batch restores original-path identities through the shared index");
        DeleteFileW(one.c_str()); DeleteFileW(two.c_str()); DeleteFileW(three.c_str());
        g_ops.Stop();
        CoUninitialize();
        return g_fail ? 1 : 0;
    }

    if (argc > 1 && std::wstring_view(argv[1]) == L"--completion-locations-only") {
        const auto source = srcDir + L"\\locate-result.txt";
        const auto original = dstDir + L"\\locate-result.txt";
        MakeFile(source, "new", 3);
        MakeFile(original, "old", 3);
        auto request = SimpleOp(ops::OpType::Copy, {source.c_str()}, dstDir.c_str());
        request.collision_policy = ops::CollisionPolicy::KeepBoth;
        g_ops.DrainCompletions();
        const auto status = RunOp(request);
        const auto pending = g_ops.TaskLocations(status.task_id);
        const auto notifications = g_ops.DrainCompletions();
        const auto retained = g_ops.TaskLocations(status.task_id);
        Check(status.phase == ops::OpPhase::Completed && pending.destinations.size() == 1 &&
              pending.destinations.front() != original && Exists(pending.destinations.front()),
              L"locate results uses the committed keep-both name before notification drain");
        Check(retained.sources == pending.sources && retained.destinations == pending.destinations &&
              retained.sources == std::vector<std::wstring>{source} && !notifications.empty(),
              L"locate results retains exact source and output paths after notification drain");
        Check(g_ops.TaskLocations(0).destinations.empty() &&
              g_ops.TaskLocations(status.task_id + 1000).destinations.empty(),
              L"locate results never adopts another task for unknown identities");
        g_ops.Stop();
        CoUninitialize();
        return g_fail ? 1 : 0;
    }

    if (argc > 1 && std::wstring_view(argv[1]) == L"--partial-move-refresh-only") {
        const auto source_folder = srcDir + L"\\merge";
        const auto target_folder = dstDir + L"\\merge";
        MakeDir(source_folder);
        MakeDir(target_folder);
        const auto source = source_folder + L"\\locked.txt";
        const auto target = target_folder + L"\\locked.txt";
        Check(MakeFile(source, "retained", 8), L"prepare locked move source");
        HANDLE lock = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(lock != INVALID_HANDLE_VALUE, L"deny source deletion while allowing copy");
        auto request = SimpleOp(ops::OpType::Move, {source_folder.c_str()}, dstDir.c_str());
        request.collision_policy = ops::CollisionPolicy::Replace;
        g_ops.DrainCompletions();
        const auto status = RunOp(request);
        const auto completions = g_ops.DrainCompletions();
        Check(Exists(source) && Exists(target), L"failed source deletion retains both copies");
        Check(status.phase != ops::OpPhase::Completed, L"partial move does not report success");
        Check(!g_ops.CanUndoTask(status.task_id), L"failed move does not enter undo");
        bool source_refresh = false, target_refresh = false, false_remap = false;
        for (const auto& completion : completions) {
            false_remap |= !completion.sources.empty() || !completion.destinations.empty();
            if (!completion.refresh_only) continue;
            for (const auto& directory : completion.refresh_directories) {
                source_refresh |= directory == source_folder;
                target_refresh |= directory == target_folder;
            }
        }
        Check(source_refresh && target_refresh, L"partial move refreshes both affected directories");
        Check(!false_remap, L"refresh does not publish failed move identity remapping");
        if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
        g_ops.Stop();
        CoUninitialize();
        return g_fail ? 1 : 0;
    }

    if (argc > 1 && std::wstring_view(argv[1]) == L"--completion-refresh-only") {
        auto request = SimpleOp(ops::OpType::CreateTextFile, {(srcDir + L"\\refresh.txt").c_str()});
        request.refresh_directories = {srcDir};
        g_ops.DrainCompletions();
        const auto status = RunOp(request);
        const auto completions = g_ops.DrainCompletions();
        Check(status.phase == ops::OpPhase::Completed && completions.size() == 1 &&
              completions.front().refresh_directories == request.refresh_directories,
              L"completion preserves explicitly affected directories for visible-pane refresh");
        g_ops.Stop();
        CoUninitialize();
        return g_fail ? 1 : 0;
    }

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
        auto payloads_of = [&](const std::wstring& wanted) {
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
                if (!pulse::path::EqualInsensitive(pulse::path::StripExtendedPathPrefix(original), pulse::path::StripExtendedPathPrefix(wanted))) continue;
                auto content = index; content[content.find_last_of(L'\\') + 2] = L'R';
                result.push_back(content);
            } while (FindNextFileW(find, &data));
            FindClose(find);
            return result;
        };
        auto version_paths = [&] { return payloads_of(versioned); };
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
        // OPS-05b: restores that used to fail. The original folder is gone, the
        // original name is taken again, and the path is beyond MAX_PATH.
        {
            const auto gone_parent = srcDir + L"\\restore-parent";
            const auto child = gone_parent + L"\\child.txt";
            const auto taken = srcDir + L"\\restore-taken.txt";
            const auto taken_copy = srcDir + L"\\restore-taken (2).txt";
            const auto deep_root = L"\\\\?\\" + srcDir + L"\\restore-long";
            std::vector<std::wstring> deep_dirs{deep_root};
            for (int level = 0; level < 5; ++level)
                deep_dirs.push_back(deep_dirs.back() + L"\\" + std::wstring(52, static_cast<wchar_t>(L'k' + level)));
            const auto deep_file = deep_dirs.back() + L"\\long.txt";
            const auto deep_plain = deep_file.substr(4);
            MakeDir(gone_parent);
            for (const auto& dir : deep_dirs) MakeDir(dir);
            Check(MakeFile(child, "child", 5) && MakeFile(taken, "old", 3) && MakeFile(deep_file, "deep", 4) &&
                  deep_plain.size() > MAX_PATH, L"OPS-05b restore fixtures created");
            for (const auto& item : { child, taken, deep_file })
                RunOp(SimpleOp(ops::OpType::RecycleDelete, { item.c_str() }));
            const auto child_payloads = payloads_of(child);
            const auto taken_payloads = payloads_of(taken);
            const auto deep_payloads = payloads_of(deep_plain);
            Check(child_payloads.size() == 1 && taken_payloads.size() == 1 && deep_payloads.size() == 1,
                  L"OPS-05b each restore fixture has one recycle version");
            RemoveDirectoryW(gone_parent.c_str());
            MakeFile(taken, "new", 3);
            const auto new_hash = FileHash(taken);
            if (child_payloads.size() == 1) {
                const auto restored_child = RunOp(SimpleOp(ops::OpType::RestoreRecycle, { child_payloads[0].c_str() }));
                Check(restored_child.last_error.empty() && FileSize(child) == 5 && payloads_of(child).empty(),
                      L"OPS-05b restore recreates a missing original folder");
            }
            if (taken_payloads.size() == 1) {
                const auto restored_taken = RunOp(SimpleOp(ops::OpType::RestoreRecycle, { taken_payloads[0].c_str() }));
                Check(restored_taken.last_error.empty() && FileHash(taken) == new_hash && Exists(taken_copy) &&
                      FileHash(taken_copy) != new_hash && payloads_of(taken).empty(),
                      L"OPS-05b restore keeps both when the original name is taken");
            }
            if (deep_payloads.size() == 1) {
                const auto restored_deep = RunOp(SimpleOp(ops::OpType::RestoreRecycle, { deep_payloads[0].c_str() }));
                Check(restored_deep.last_error.empty() && FileSize(deep_file) == 4 && payloads_of(deep_plain).empty(),
                      L"OPS-05b restore handles paths beyond MAX_PATH");
            }
            // Leave nothing of ours in the real bin, whatever happened above.
            for (const auto& original : { child, taken, deep_plain }) {
                for (const auto& left : payloads_of(original)) {
                    auto left_index = left;
                    left_index[left_index.find_last_of(L'\\') + 2] = L'I';
                    DeleteFileW((L"\\\\?\\" + left).c_str());
                    DeleteFileW((L"\\\\?\\" + left_index).c_str());
                }
            }
            DeleteFileW(child.c_str());
            RemoveDirectoryW(gone_parent.c_str());
            DeleteFileW(taken.c_str());
            DeleteFileW(taken_copy.c_str());
            DeleteFileW(deep_file.c_str());
            for (auto it = deep_dirs.rbegin(); it != deep_dirs.rend(); ++it) RemoveDirectoryW(it->c_str());
            Check(payloads_of(child).empty() && payloads_of(taken).empty() && payloads_of(deep_plain).empty() &&
                  !Exists(deep_root), L"OPS-05b controlled recycle fixtures cleaned");
        }
        // OPS-05c: direct emptying, on a fake bin folder only (never the real one).
        {
            const auto fake = srcDir + L"\\fake-bin";
            const auto outside = srcDir + L"\\fake-outside";
            MakeDir(fake);
            MakeDir(outside);
            MakeFile(outside + L"\\keep.txt", "keep", 4);
            const auto write_index = [](const std::wstring& path, const std::wstring& original, uint64_t size) {
                std::vector<BYTE> bytes(28 + (original.size() + 1) * 2, 0);
                bytes[0] = 2;
                memcpy(bytes.data() + 8, &size, 8);
                const uint32_t units = static_cast<uint32_t>(original.size() + 1);
                memcpy(bytes.data() + 24, &units, 4);
                memcpy(bytes.data() + 28, original.c_str(), original.size() * 2);
                return MakeFile(path, bytes.data(), static_cast<DWORD>(bytes.size()));
            };
            bool built = write_index(fake + L"\\$IAAAAAA.txt", L"C:\\fixture\\a.txt", 1) &&
                         MakeFile(fake + L"\\$RAAAAAA.txt", "a", 1);
            const auto tree = fake + L"\\$RBBBBBB";
            MakeDir(tree);
            for (int d = 0; d < 3; ++d) {
                const auto sub = tree + L"\\d" + std::to_wstring(d);
                MakeDir(sub);
                for (int f = 0; f < 100; ++f) built = MakeFile(sub + L"\\f" + std::to_wstring(f) + L".txt", "x", 1) && built;
            }
            built = MakeFile(tree + L"\\readonly.txt", "r", 1) && built;
            SetFileAttributesW((tree + L"\\readonly.txt").c_str(), FILE_ATTRIBUTE_READONLY);
            std::wstring deep = L"\\\\?\\" + tree;
            for (int level = 0; level < 5; ++level) { deep += L"\\" + std::wstring(52, static_cast<wchar_t>(L'p' + level)); MakeDir(deep); }
            built = MakeFile(deep + L"\\deep.txt", "d", 1) && built;
            built = MakeJunction(tree + L"\\link", outside) && built;
            built = write_index(fake + L"\\$IBBBBBB", L"C:\\fixture\\tree", 303) && built;
            built = write_index(fake + L"\\$ICCCCCC.txt", L"C:\\fixture\\orphan.txt", 1) && built;
            MakeDir(fake + L"\\$RDDDDDD");
            built = MakeFile(fake + L"\\$RDDDDDD\\one.txt", "1", 1) && MakeFile(fake + L"\\$RDDDDDD\\two.txt", "2", 1) && built;
            built = MakeFile(fake + L"\\desktop.ini", "ini", 3) && MakeFile(fake + L"\\.other", "o", 1) && built;
            Check(built, L"OPS-05c fake bin fixture created");

            // Cancelled before the first file: every item stays intact.
            auto cancelled_run = recycle_empty::EmptyFolders({ L"\\\\?\\" + fake }, [] { return true; }, nullptr);
            Check(cancelled_run.cancelled && cancelled_run.deleted_files == 0 && Exists(fake + L"\\$RAAAAAA.txt") &&
                  Exists(fake + L"\\$IBBBBBB") && Exists(tree + L"\\d2\\f99.txt"), L"OPS-05c cancelled empty leaves items intact");

            recycle_empty::Progress last;
            bool saw_totals = false;
            const auto result = recycle_empty::EmptyFolders({ L"\\\\?\\" + fake }, [] { return false; },
                [&](const recycle_empty::Progress& p) { last = p; saw_totals = saw_totals || (p.total_items == 2 && p.total_bytes == 304); }, 4);
            Check(result.removed_items == 2 && result.deleted_files == 305 && result.failed_entries == 0 && !result.cancelled,
                  L"OPS-05c direct empty removes every item and file");
            Check(saw_totals && last.removed_items == 2 && last.deleted_files == 305, L"OPS-05c progress reports totals from the $I records");
            Check(Exists(outside + L"\\keep.txt"), L"OPS-05c a junction inside a payload is removed without following it");
            Check(!Exists(fake + L"\\$IAAAAAA.txt") && !Exists(fake + L"\\$RAAAAAA.txt") && !Exists(fake + L"\\$IBBBBBB") &&
                  !Exists(tree) && !Exists(fake + L"\\$ICCCCCC.txt") && !Exists(fake + L"\\$RDDDDDD") &&
                  Exists(fake + L"\\desktop.ini") && Exists(fake + L"\\.other"),
                  L"OPS-05c pairs, orphans and stray payloads go; other files stay");
            DeleteFileW((fake + L"\\desktop.ini").c_str());
            DeleteFileW((fake + L"\\.other").c_str());
            RemoveDirectoryW(fake.c_str());
            DeleteFileW((outside + L"\\keep.txt").c_str());
            RemoveDirectoryW(outside.c_str());
            Check(!Exists(fake) && !Exists(outside), L"OPS-05c fake bin cleaned");
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


    if (same_folder_only) {
        const auto file = srcDir + L"\\same.txt";
        MakeFile(file, payload, sizeof(payload) - 1);
        for (int pass = 0; pass < 2; ++pass) {
            auto status = RunOp(SimpleOp(ops::OpType::Copy, {file.c_str()}, srcDir.c_str()));
            size_t copies = 0;
            bool contents = true;
            for (const auto& item : std::filesystem::directory_iterator(srcDir)) {
                if (!item.path().filename().wstring().starts_with(L"same")) continue;
                ++copies;
                std::ifstream stream(item.path(), std::ios::binary);
                const std::string bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
                contents = contents && bytes == payload;
            }
            Check(status.last_error.empty() && copies == static_cast<size_t>(pass + 2) && contents,
                L"same-folder copy preserves source and creates a unique identical duplicate");
        }
        auto status = RunOp(SimpleOp(ops::OpType::Move, {file.c_str()}, srcDir.c_str()));
        Check(status.last_error.empty() && status.summary.empty() && Exists(file),
            L"same-folder move stays a silent no-op");
        status = RunOp(SimpleOp(ops::OpType::Copy, {file.c_str()}, dstDir.c_str()));
        Check(status.last_error.empty() && Exists(file) && FileSize(dstDir + L"\\same.txt") == sizeof(payload) - 1,
            L"cross-folder copy retains source and writes destination");
        g_ops.Stop();
        wprintf(L"Same folder: %d passed, %d failed\n", g_pass, g_fail);
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

    // --- 新建 from a ShellNew entry (template file / data bytes) -------------
    {
        const auto ticks = [](const FILETIME& f) {
            return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
        };
        // An old, read-only template, like the ones under Program Files.
        const std::wstring tpl = srcDir + L"\\shellnew-template.docx";
        MakeFile(tpl, payload, sizeof(payload) - 1);
        SYSTEMTIME old_time{2020, 1, 0, 1, 0, 0, 0, 0};
        FILETIME old_file_time{};
        SystemTimeToFileTime(&old_time, &old_file_time);
        HANDLE tpl_handle = CreateFileW(tpl.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        nullptr, OPEN_EXISTING, 0, nullptr);
        if (tpl_handle != INVALID_HANDLE_VALUE) {
            SetFileTime(tpl_handle, &old_file_time, &old_file_time, &old_file_time);
            CloseHandle(tpl_handle);
        }
        SetFileAttributesW(tpl.c_str(), FILE_ATTRIBUTE_READONLY);

        const std::wstring made = srcDir + L"\\new-from-template.docx";
        ops::OpRequest req;
        req.type = ops::OpType::CreateTextFile;
        req.sources = { made };
        req.template_file = tpl;
        const auto created = RunOp(req);
        WIN32_FILE_ATTRIBUTE_DATA info{};
        const bool got = GetFileAttributesExW(made.c_str(), GetFileExInfoStandard, &info) != FALSE;
        FILETIME now{};
        GetSystemTimeAsFileTime(&now);
        Check(got && created.last_error.empty(), L"新建 from a template creates the file");
        Check(got && info.nFileSizeLow == sizeof(payload) - 1, L"template copy keeps the template bytes");
        Check(got && !(info.dwFileAttributes & FILE_ATTRIBUTE_READONLY), L"template copy is not read-only");
        Check(got && ticks(now) - ticks(info.ftLastWriteTime) < 10ull * 60 * 10000000,
              L"template copy is dated now, not with the template's date");

        const std::wstring made_data = srcDir + L"\\new-from-data.zip";
        ops::OpRequest data_req;
        data_req.type = ops::OpType::CreateTextFile;
        data_req.sources = { made_data };
        data_req.template_data = { 'P', 'K', 5, 6 };
        RunOp(data_req);
        WIN32_FILE_ATTRIBUTE_DATA data_info{};
        Check(GetFileAttributesExW(made_data.c_str(), GetFileExInfoStandard, &data_info) &&
              data_info.nFileSizeLow == 4, L"新建 from data bytes writes them");

        SetFileAttributesW(tpl.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(tpl.c_str());
        DeleteFileW(made.c_str());
        DeleteFileW(made_data.c_str());
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
        Check(g_ops.Status().summary == l10n::Get(l10n::StringId::OpUndoneMove),
              L"undo move finishes as 已撤销移动, not as the inverse move");

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
        Check(g_ops.Status().summary == l10n::Get(l10n::StringId::OpUndoneCopy),
              L"undo copy finishes as 已撤销复制, not as 删除完成");
        Check(!g_ops.CanUndoTask(g_ops.Status().task_id),
              L"a finished undo offers no undo of its own in the pill");
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
        // History is capped at 100 steps; the oldest are dropped first.
        std::wstring many = L"[";
        for (int i = 0; i < 150; ++i) {
            wchar_t name[16];
            swprintf_s(name, L"u%03d", i);
            if (i) many += L",";
            many += L"{\"type\":0,\"sup\":true,\"dest\":\"C:\\\\tmp\",\"name\":\"\",\"src\":[\"C:\\\\tmp\\\\";
            many += name;
            many += L".txt\"]}";
        }
        many += L"]";
        ops::OpsManager capped;
        Check(capped.UndoFromJson(many), L"150-step undo JSON parses");
        const std::wstring kept = capped.UndoToJson();
        size_t steps = 0;
        for (size_t at = kept.find(L"\"type\""); at != std::wstring::npos;
             at = kept.find(L"\"type\"", at + 1))
            ++steps;
        Check(steps == 100, L"undo history keeps at most 100 steps");
        Check(kept.find(L"u149") != std::wstring::npos && kept.find(L"u050") != std::wstring::npos &&
              kept.find(L"u049") == std::wstring::npos && kept.find(L"u000") == std::wstring::npos,
              L"undo history drops the oldest steps first");
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
        {
            // The progress card's history is recorded by the engine, so a card
            // shown late still draws the run.
            const auto history = g_ops.SpeedGraph(completed.task_id);
            bool ordered = history.by_progress;
            for (size_t i = 1; i < history.points.size(); ++i)
                ordered = ordered && history.points[i].x >= history.points[i - 1].x;
            Check(ordered && history.points.size() <= ops::OpSpeedGraph::kMaxProgressPoints &&
                  g_ops.SpeedGraph(completed.task_id + 1000).points.empty(),
                  L"background speed history follows progress and belongs to the newest task");

            ops::OpSpeedGraph graph;
            uint64_t tick = 1000;
            for (int i = 0; i <= 1000; ++i, tick += 250) graph.Sample(tick, i / 10.0f, 100.0 + i);
            Check(graph.by_progress && graph.points.size() <= ops::OpSpeedGraph::kMaxProgressPoints &&
                  graph.points.size() > ops::OpSpeedGraph::kMaxProgressPoints / 2 &&
                  graph.points.back().x == 1.0f && graph.points.back().v == 1100.0,
                  L"speed graph thins to its point budget and keeps the newest sample exact");
            const size_t kept = graph.points.size();
            graph.Sample(tick - 250 + 100, 50.0f, 1.0);
            Check(graph.points.size() == kept && graph.points.back().v == 1100.0,
                  L"speed graph ignores samples inside the 250 ms interval");
            graph.Sample(tick + 250, 10.0f, 7.0);
            Check(graph.points.size() == 1 && graph.points.front().v == 7.0,
                  L"speed graph restarts when progress goes back (a new phase)");
            for (int i = 0; i < 200; ++i) graph.Sample(tick + 500 + i * 250ull, -1.0f, 3.0);
            Check(!graph.by_progress && graph.points.size() == ops::OpSpeedGraph::kMaxTimePoints,
                  L"speed graph without progress keeps a rolling time window");

            // Time left after a cold start: 10 MB/s for 1.5 s, then 100 MB/s.
            // The estimate used to read ~80 s at 3 s for ~8 s of work.
            const uint64_t total = 1000ull * 1000ull * 1000ull;
            auto run_rate = [&](auto bytes_per_tick, ULONGLONG until) {
                ops::TransferRateEstimator estimator;
                estimator.Reset(0, 0);
                uint64_t done = 0;
                for (ULONGLONG at = 100; at <= until; at += 100) {
                    done = (std::min)(total, done + bytes_per_tick(at));
                    estimator.Observe(at, done, total);
                }
                return std::pair<uint64_t, uint64_t>{ estimator.eta_seconds(), total - done };
            };
            const auto cold = run_rate([](ULONGLONG at) { return at <= 1500 ? 1000000ull : 10000000ull; }, 3000);
            const double cold_truth = static_cast<double>(cold.second) / 1e8;
            Check(cold.first > 0 && cold.first <= cold_truth * 1.5 + 1 && cold.first + 2 >= cold_truth,
                  L"time left settles within seconds after a cold start");
            const auto steady = run_rate([](ULONGLONG) { return 10000000ull; }, 5000);
            Check(steady.first >= 4 && steady.first <= 6, L"time left matches a steady rate");
            // A pause is cut out of the rate clock: on resume the speed carries on.
            ops::TransferRateEstimator held;
            held.Reset(0, 0);
            uint64_t held_done = 0;
            for (ULONGLONG at = 100; at <= 3000; at += 100) held.Observe(at, held_done += 10000000ull, total);
            const double before_pause = held.speed();
            held.Observe(3100, held_done += 10000000ull, total);   // 20 s paused, not on this clock
            Check(before_pause > 9e7 && held.speed() > 9e7 && held.eta_seconds() > 0,
                  L"speed and time left carry on across a pause");
        }
        {
            // A real pause mid-copy: on resume the live speed carries on and the
            // background graph never dips to 0 (it used to reset and warm up).
            const std::wstring held_source = srcDir + L"\\held.bin";
            const std::wstring held_target = dstDir + L"\\held.bin";
            HANDLE file = CreateFileW(held_source.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            bool sized = file != INVALID_HANDLE_VALUE;
            if (sized) {
                LARGE_INTEGER size{};
                size.QuadPart = 4ll * 1024 * 1024 * 1024;   // zero-filled lazily: quick to make
                sized = SetFilePointerEx(file, size, nullptr, FILE_BEGIN) && SetEndOfFile(file);
                CloseHandle(file);
            }
            const uint64_t held_previous = g_ops.Status().completed_ops;
            g_ops.Submit(SimpleOp(ops::OpType::Copy, { held_source.c_str() }, dstDir.c_str()));
            bool measured = false;
            uint64_t task = 0;
            const auto live_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            while (sized && std::chrono::steady_clock::now() < live_deadline) {
                const auto st = g_ops.Status();
                if (st.active && st.phase == ops::OpPhase::Running && st.bytes_per_second > 0.0 &&
                    st.percent > 3.0f && st.percent < 60.0f) { measured = true; task = st.task_id; break; }
                if (!st.active && st.completed_ops > held_previous) break;
                Sleep(10);
            }
            bool held_paused = false, resumed = false, dipped = false;
            if (measured) {
                g_ops.PauseCurrent();
                const auto held_pause_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (std::chrono::steady_clock::now() < held_pause_deadline) {
                    if (g_ops.Status().phase == ops::OpPhase::Paused) { held_paused = true; break; }
                    Sleep(5);
                }
                Sleep(1500);
                g_ops.ResumeCurrent();
                const auto watch_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
                while (std::chrono::steady_clock::now() < watch_until) {
                    const auto st = g_ops.Status();
                    if (!st.active) break;
                    if (st.phase == ops::OpPhase::Running && !st.pause_requested) {
                        resumed = true;
                        dipped = dipped || st.bytes_per_second <= 0.0;
                    }
                    Sleep(5);
                }
            }
            const auto history = g_ops.SpeedGraph(task);
            bool graph_dips = false, seen_speed = false;
            for (const auto& point : history.points) {
                if (point.v > 0.0) seen_speed = true;
                else if (seen_speed) graph_dips = true;
            }
            g_ops.CancelCurrent();
            WaitOpDone(held_previous);
            wprintf(L"[INFO] resume speed: measured=%d paused=%d resumed=%d dipped=%d graph_points=%zu graph_dips=%d\n",
                    measured ? 1 : 0, held_paused ? 1 : 0, resumed ? 1 : 0, dipped ? 1 : 0,
                    history.points.size(), graph_dips ? 1 : 0);
            Check(measured && held_paused && resumed && !dipped,
                  L"resume keeps the live speed instead of dropping to 0");
            Check(seen_speed && !graph_dips, L"background speed graph does not dip to 0 across a pause");
            DeleteFileW(held_target.c_str());
            DeleteFileW(held_source.c_str());
        }

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

    // --- Parallel small-file copy keeps what a serial copy keeps ------------
    {
        // 40 small files: enough for the copy pool. Attributes, a stream,
        // timestamps, and the order-dependent results must match.
        const std::wstring par_src = root + L"\\par-src";
        const std::wstring par_dst = root + L"\\par-dst";
        MakeDir(par_src);
        MakeDir(par_src + L"\\tree");
        MakeDir(par_dst);
        constexpr int kParallelFiles = 40;
        auto par_name = [](int i) {
            wchar_t name[32];
            swprintf_s(name, L"\\f%02d.txt", i);
            return std::wstring(name);
        };
        for (int i = 0; i < kParallelFiles; ++i)
            MakeFile(par_src + L"\\tree" + par_name(i), payload, sizeof(payload) - 1);
        const std::wstring tree = par_src + L"\\tree";
        // A large file in the middle goes behind the small ones.
        Check(MakePatternFile(tree + L"\\f20-big.bin", 3ull << 20), L"parallel copy: large fixture");
        SetFileAttributesW((tree + par_name(0)).c_str(), FILE_ATTRIBUTE_READONLY);
        SetFileAttributesW((tree + par_name(1)).c_str(), FILE_ATTRIBUTE_HIDDEN);
        const char stream_data[] = "side stream";
        MakeFile(tree + par_name(2) + L":meta", stream_data, sizeof(stream_data) - 1);
        FILETIME old_time{};
        SYSTEMTIME old_system{2001, 1, 0, 1, 12, 0, 0, 0};
        SystemTimeToFileTime(&old_system, &old_time);
        {
            HANDLE dated = CreateFileW((tree + par_name(3)).c_str(), FILE_WRITE_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (dated != INVALID_HANDLE_VALUE) {
                SetFileTime(dated, nullptr, nullptr, &old_time);
                CloseHandle(dated);
            }
        }
        auto par_st = RunOp(SimpleOp(ops::OpType::Copy, { tree.c_str() }, par_dst.c_str()));
        const std::wstring copied = par_dst + L"\\tree";
        int present = 0;
        for (int i = 0; i < kParallelFiles; ++i)
            if (FileSize(copied + par_name(i)) == sizeof(payload) - 1) ++present;
        Check(par_st.last_error.empty() && present == kParallelFiles,
              L"parallel copy: every small file arrives intact");
        Check(FileHash(copied + L"\\f20-big.bin") == FileHash(tree + L"\\f20-big.bin"),
              L"parallel copy: the deferred large file arrives intact");
        const DWORD ro = GetFileAttributesW((copied + par_name(0)).c_str());
        const DWORD hidden = GetFileAttributesW((copied + par_name(1)).c_str());
        Check(ro != INVALID_FILE_ATTRIBUTES && (ro & FILE_ATTRIBUTE_READONLY) &&
              hidden != INVALID_FILE_ATTRIBUTES && (hidden & FILE_ATTRIBUTE_HIDDEN),
              L"parallel copy: read-only and hidden attributes kept");
        Check(FileSize(copied + par_name(2) + L":meta") == sizeof(stream_data) - 1,
              L"parallel copy: alternate data stream kept");
        FILETIME copied_time{};
        {
            HANDLE dated = CreateFileW((copied + par_name(3)).c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (dated != INVALID_HANDLE_VALUE) {
                GetFileTime(dated, nullptr, nullptr, &copied_time);
                CloseHandle(dated);
            }
        }
        Check(CompareFileTime(&copied_time, &old_time) == 0,
              L"parallel copy: modified time kept");
        SetFileAttributesW((copied + par_name(0)).c_str(), FILE_ATTRIBUTE_NORMAL);
        Check(g_ops.CanUndo(), L"parallel copy: undo available");

        // A large file that meets a conflict while the small files go to the
        // pool is deferred after the choice; keep-both must still copy it.
        {
            const std::wstring lc_src = par_src + L"\\lc";
            const std::wstring lc_dst = par_dst + L"\\lcdst";
            const std::wstring lc_in = lc_dst + L"\\lc";
            MakeDir(lc_src);
            MakeDir(lc_dst);
            MakeDir(lc_in);
            for (int i = 0; i < 20; ++i) MakeFile(lc_src + par_name(i), payload, sizeof(payload) - 1);
            constexpr uint64_t kLargeConflictBytes = 70ull << 20;
            Check(MakePatternFile(lc_src + L"\\zbig.bin", kLargeConflictBytes), L"large conflict: fixture");
            MakeFile(lc_in + L"\\zbig.bin", payload, sizeof(payload) - 1);
            auto keep = RunConflictOp(SimpleOp(ops::OpType::Copy, { lc_src.c_str() }, lc_dst.c_str()),
                                      ops::ConflictChoice::KeepBoth, false);
            int big_copies = 0;
            int names = 0;
            WIN32_FIND_DATAW found{};
            HANDLE find = FindFirstFileW((lc_in + L"\\*").c_str(), &found);
            if (find != INVALID_HANDLE_VALUE) {
                do {
                    if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    ++names;
                    const uint64_t size = (static_cast<uint64_t>(found.nFileSizeHigh) << 32) | found.nFileSizeLow;
                    if (size == kLargeConflictBytes) ++big_copies;
                } while (FindNextFileW(find, &found));
                FindClose(find);
            }
            fwprintf(stderr, L"[INFO] large conflict keep-both: names=%d big=%d err=%ls\n",
                     names, big_copies, keep.last_error.c_str());
            Check(keep.last_error.empty() && big_copies == 1 &&
                  FileSize(lc_in + L"\\zbig.bin") == sizeof(payload) - 1,
                  L"large conflict: keep-both copies the large file beside the old one");
            auto replace = RunConflictOp(SimpleOp(ops::OpType::Copy, { lc_src.c_str() }, lc_dst.c_str()),
                                         ops::ConflictChoice::Replace, true);
            Check(replace.last_error.empty() && FileSize(lc_in + L"\\zbig.bin") == kLargeConflictBytes,
                  L"large conflict: replace overwrites with the large file");
        }

        // A large single copy reports bytes as it goes, and a pause request
        // shows before the worker reaches its next check.
        {
            const std::wstring big = par_src + L"\\progress.bin";
            const std::wstring big_dst = par_dst + L"\\progress";
            MakeDir(big_dst);
            Check(MakePatternFile(big, 256ull << 20), L"copy progress: fixture");
            const uint64_t before = g_ops.Status().completed_ops;
            g_ops.Submit(SimpleOp(ops::OpType::Copy, { big.c_str() }, big_dst.c_str()));
            bool mid = false;
            bool requested = false;
            bool cleared = false;
            bool paused_once = false;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            while (std::chrono::steady_clock::now() < deadline) {
                const auto st = g_ops.Status();
                if (st.completed_ops > before) break;
                if (st.active && st.total_bytes > 0 && st.transferred_bytes > 0 &&
                    st.transferred_bytes < st.total_bytes) {
                    mid = true;
                    if (!paused_once) {
                        paused_once = true;
                        g_ops.PauseCurrent();
                        requested = g_ops.Status().pause_requested;
                        g_ops.ResumeCurrent();
                        cleared = !g_ops.Status().pause_requested;
                    }
                }
                Sleep(1);
            }
            Check(mid, L"copy progress: a large file reports bytes while copying");
            Check(requested && cleared,
                  L"copy progress: a pause request shows at once and clears on resume");
            Check(FileHash(big_dst + L"\\progress.bin") == FileHash(big),
                  L"copy progress: the paused large file arrives intact");
            DeleteFileW((big_dst + L"\\progress.bin").c_str());
            DeleteFileW(big.c_str());
        }

        // One target name from two folders: the second must meet the first
        // as a conflict, not race it.
        const std::wstring other = par_src + L"\\other";
        MakeDir(other);
        MakeFile(other + par_name(5), payload, sizeof(payload) - 1);
        const std::wstring same_dst = par_dst + L"\\same";
        MakeDir(same_dst);
        ops::OpRequest same_name;
        same_name.type = ops::OpType::Copy;
        same_name.dest_dir = same_dst;
        for (int i = 0; i < 20; ++i) same_name.sources.push_back(tree + par_name(i));
        same_name.sources.push_back(other + par_name(5));
        size_t same_resolved = 0;
        auto same_st = RunConflictOp(std::move(same_name), ops::ConflictChoice::KeepBoth, true,
                                     &same_resolved);
        int same_count = 0;
        WIN32_FIND_DATAW found{};
        HANDLE listing = FindFirstFileW((same_dst + L"\\*").c_str(), &found);
        if (listing != INVALID_HANDLE_VALUE) {
            do { if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) ++same_count; }
            while (FindNextFileW(listing, &found));
            FindClose(listing);
        }
        Check(same_st.last_error.empty() && same_resolved == 1 && same_count == 21,
              L"parallel copy: same name from two folders becomes a conflict");

        // Merge-move into an existing folder takes the per-file path: the
        // pool copies and deletes each source.
        const std::wstring merge_src = par_src + L"\\merge";
        const std::wstring merge_dst = par_dst + L"\\merge";
        MakeDir(merge_src);
        MakeDir(merge_dst);
        for (int i = 0; i < 20; ++i)
            MakeFile(merge_src + par_name(i), payload, sizeof(payload) - 1);
        SetFileAttributesW((merge_src + par_name(0)).c_str(), FILE_ATTRIBUTE_READONLY);
        auto merge_st = RunOp(SimpleOp(ops::OpType::Move, { merge_src.c_str() }, par_dst.c_str()));
        int moved = 0;
        for (int i = 0; i < 20; ++i)
            if (Exists(merge_dst + par_name(i)) && !Exists(merge_src + par_name(i))) ++moved;
        Check(merge_st.last_error.empty() && moved == 20 && !Exists(merge_src),
              L"parallel move: merged files moved and the source folder removed");
        SetFileAttributesW((merge_dst + par_name(0)).c_str(), FILE_ATTRIBUTE_NORMAL);

        // Partial failure: files another program holds are listed with a
        // reason, the rest go through, and a retry finishes them.
        const auto hold = [](const std::wstring& path) {
            return CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        };
        const std::wstring part_src = par_src + L"\\partial";
        const std::wstring part_dst = par_dst + L"\\partial";
        MakeDir(part_src);
        MakeDir(part_dst);
        for (int i = 0; i < 20; ++i)
            MakeFile(part_src + par_name(i), payload, sizeof(payload) - 1);
        HANDLE held_3 = hold(part_src + par_name(3));
        HANDLE held_7 = hold(part_src + par_name(7));
        auto part_st = RunOp(SimpleOp(ops::OpType::Move, { part_src.c_str() }, par_dst.c_str()));
        int part_moved = 0;
        for (int i = 0; i < 20; ++i)
            if (i != 3 && i != 7 && Exists(part_dst + par_name(i)) && !Exists(part_src + par_name(i))) ++part_moved;
        wprintf(L"[INFO] partial move: phase=%d failed=%llu listed=%zu moved=%d error=%s\n",
                static_cast<int>(part_st.phase), static_cast<unsigned long long>(part_st.failed_items),
                part_st.failures.size(), part_moved, part_st.last_error.c_str());
        Check(part_st.phase == ops::OpPhase::Failed && part_st.failed_items == 2 &&
                  part_st.failures.size() == 2 && part_moved == 18,
              L"partial move: two held files listed, the other 18 moved");
        Check(Exists(part_src + par_name(3)) && Exists(part_src + par_name(7)) &&
                  !Exists(part_dst + par_name(3)) && !Exists(part_dst + par_name(7)),
              L"partial move: held sources kept, nothing half-written at the target");
        Check(!part_st.failures.empty() && !part_st.failures[0].reason.empty() &&
                  part_st.failures[0].reason.find(L" | ") == std::wstring::npos,
              L"partial move: each item has a reason without the path");
        if (held_3 != INVALID_HANDLE_VALUE) CloseHandle(held_3);
        if (held_7 != INVALID_HANDLE_VALUE) CloseHandle(held_7);
        const uint64_t before_retry = g_ops.Status().completed_ops;
        const auto retry_spawns = g_ops.RetryFailedItems(part_st.task_id);
        const bool retry_queued = retry_spawns.size() == 1 && retry_spawns[0].items.size() == 2;
        Check(retry_queued && WaitOpDone(before_retry) && Exists(part_dst + par_name(3)) &&
                  Exists(part_dst + par_name(7)) && !Exists(part_src + par_name(3)) &&
                  !Exists(part_src + par_name(7)),
              L"partial move: retry moves the failed items to their folder");
        const auto retry_final = retry_queued ? g_ops.FinalStatus(retry_spawns[0].task_id) : std::nullopt;
        const auto first_final = g_ops.FinalStatus(part_st.task_id);
        Check(retry_final && retry_final->phase == ops::OpPhase::Completed && first_final &&
                  first_final->phase == ops::OpPhase::Failed && first_final->failed_items == 2,
              L"partial move: how each task ended can be looked up afterwards");
        Check(g_ops.RetryFailedItems(part_st.task_id).empty(), L"partial move: a finished retry cannot run twice");

        // Few files (no pool), one held: listed, and the file-in-use prompt
        // gets it as before.
        const std::wstring few_src = par_src + L"\\few";
        const std::wstring few_dst = par_dst + L"\\few";
        MakeDir(few_src);
        for (int i = 0; i < 3; ++i)
            MakeFile(few_src + par_name(i), payload, sizeof(payload) - 1);
        HANDLE held_1 = hold(few_src + par_name(1));
        auto few_st = RunOp(SimpleOp(ops::OpType::Copy, { few_src.c_str() }, par_dst.c_str()));
        if (held_1 != INVALID_HANDLE_VALUE) CloseHandle(held_1);
        Check(few_st.phase == ops::OpPhase::Failed && few_st.failed_items == 1 &&
                  Exists(few_dst + par_name(0)) && Exists(few_dst + par_name(2)) && !Exists(few_dst + par_name(1)) &&
                  !few_st.locked_path.empty(),
              L"partial copy: one held file listed, the rest copied, in-use prompt offered");
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

// ops_test_main.cpp — Stage 1B-1 ops-layer self test (console).
//
// Drives the full stack: OpsManager -> ShellClient -> pipe -> pulse_shell.exe
// -> IFileOperation. All file operations are confined to
// bench_data/opstest (created/cleaned by this test). Prints one PASS/FAIL
// line per check; exit code 0 iff all checks pass.
#include "../ops/ops_manager.h"
#include "../ops/clipboard.h"
#include "../ipc/shell_client.h"
#include <windows.h>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

using namespace pulse;

namespace {

const wchar_t* kRoot = L"C:\\Users\\SS\\Desktop\\pulse\\bench_data\\opstest";

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

uint64_t FileSize(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return ~0ull;
    ULARGE_INTEGER s{ fad.nFileSizeLow, fad.nFileSizeHigh };
    return s.QuadPart;
}

ops::OpsManager g_ops;
std::atomic<bool> g_pause_when_active{false};
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

} // namespace

int wmain() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    fprintf(stderr, "[test] start\n");

    // --- Setup sandbox ------------------------------------------------------
    std::wstring root = kRoot;
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
        if (g_pause_when_active.load() && status.active &&
            status.phase == ops::OpPhase::Scanning) {
            g_pause_when_active.store(false);
            g_ops.PauseCurrent();
        }
    });
    Sleep(800); // let the ops worker bring up ShellClient before direct IPC calls
    fprintf(stderr, "[test] ops started\n");

    // --- 1. Ping ------------------------------------------------------------
    {
        uint64_t prev = g_ops.Status().completed_ops;
        (void)prev;
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

        auto move_st = RunOp(SimpleOp(ops::OpType::Move,
            { (srcDir + L"\\same.txt").c_str() }, srcDir.c_str()));
        Check(Exists(srcDir + L"\\same.txt"), L"move into same folder is a no-op");
        Check(move_st.last_error.empty(), L"same-folder move reported no error");
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

        // copy src\d.txt -> dst, then undo (deletes the copy, to recycle bin)
        RunOp(SimpleOp(ops::OpType::Copy, { (srcDir + L"\\d.txt").c_str() }, dstDir.c_str()));
        Check(Exists(dstDir + L"\\d.txt"), L"undo-copy setup: d.txt copied");
        prev = g_ops.Status().completed_ops;
        g_ops.Undo();
        WaitOpDone(prev);
        Check(!Exists(dstDir + L"\\d.txt") && Exists(srcDir + L"\\d.txt"),
              L"undo copy: dst copy removed, source intact");
    }

    // --- 9. Undo of recycle delete is recorded but unsupported --------------
    {
        MakeFile(srcDir + L"\\e.txt", payload, sizeof(payload) - 1);
        RunOp(SimpleOp(ops::OpType::RecycleDelete, { (srcDir + L"\\e.txt").c_str() }));
        Check(!Exists(srcDir + L"\\e.txt"), L"recycle-delete src\\e.txt");
        Check(g_ops.CanUndo(), L"undo available for recycle delete");
        uint64_t prev = g_ops.Status().completed_ops;
        g_ops.Undo();
        WaitOpDone(prev);
        Check(Exists(srcDir + L"\\e.txt"), L"undo recycle-delete restores src\\e.txt");
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

    g_ops.Stop();

    wprintf(L"\n== ops self test: %d passed, %d failed ==\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

// file_lock_owner_test.cpp — Restart Manager lookup + end-process-and-retry.
//
// Every fixture lives under bench_data\file-lock-<pid>. Lock holders are copies
// of this executable started with --hold, so the test never touches user
// processes or files.
#include "../common/localization.h"
#include "../ops/file_lock_owner.h"
#include "../ops/ops_manager.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace pulse::ops;
namespace fsys = std::filesystem;

namespace {
int failures = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    failures += !ok;
}

int HoldMode(const wchar_t* path, const wchar_t* event_name) {
    HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 2;
    if (HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, event_name)) {
        SetEvent(ready);
        CloseHandle(ready);
    }
    Sleep(120000);   // safety net: the test ends us long before this
    CloseHandle(file);
    return 0;
}

struct Holder {
    PROCESS_INFORMATION pi{};
    bool Alive() const { return pi.hProcess && WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT; }
    void Stop() {
        if (!pi.hProcess) return;
        if (Alive()) TerminateProcess(pi.hProcess, 0);
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        pi = {};
    }
    ~Holder() { Stop(); }
};

fsys::path g_holder_exe;
int g_holder_seq = 0;

bool StartHolder(Holder& holder, const fsys::path& file, const fsys::path& exe = {}) {
    const fsys::path& run = exe.empty() ? g_holder_exe : exe;
    const std::wstring event_name = L"Local\\pulse-lock-test-" + std::to_wstring(GetCurrentProcessId())
        + L"-" + std::to_wstring(++g_holder_seq);
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, event_name.c_str());
    std::wstring command = L"\"" + run.wstring() + L"\" --hold \"" + file.wstring() + L"\" "
        + event_name;
    STARTUPINFOW si{sizeof(si)};
    const bool started = CreateProcessW(run.c_str(), command.data(), nullptr, nullptr, FALSE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &holder.pi) != FALSE;
    const bool signalled = started && WaitForSingleObject(ready, 10000) == WAIT_OBJECT_0;
    CloseHandle(ready);
    return signalled;
}

void Touch(const fsys::path& path) {
    fsys::create_directories(path.parent_path());
    std::ofstream(path) << "fixture";
}

const LockOwner* FindPid(const std::vector<LockOwner>& owners, DWORD pid) {
    for (const auto& owner : owners)
        if (owner.pid == pid) return &owner;
    return nullptr;
}

std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0,
                                         nullptr, nullptr);
    std::string out(static_cast<size_t>(size > 0 ? size : 0), '\0');
    if (size > 0)
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size, nullptr,
                            nullptr);
    return out;
}

bool WaitDone(OpsManager& ops, uint64_t before) {
    const ULONGLONG deadline = GetTickCount64() + 20000;
    while (GetTickCount64() < deadline) {
        const OpStatus st = ops.Status();
        if (st.completed_ops > before && !st.active) return true;
        Sleep(5);
    }
    const OpStatus st = ops.Status();
    std::printf("[INFO] timeout task=%llu phase=%d authorization=%d completed=%llu active=%d\n",
                static_cast<unsigned long long>(st.task_id), static_cast<int>(st.phase),
                static_cast<int>(st.authorization), static_cast<unsigned long long>(st.completed_ops),
                st.active ? 1 : 0);
    return false;
}

// Submit, wait, return the final status.
OpStatus Run(OpsManager& ops, OpRequest request) {
    const uint64_t before = ops.Status().completed_ops;
    ops.Submit(std::move(request));
    if (!WaitDone(ops, before)) std::printf("[INFO] operation timed out\n");
    return ops.Status();
}

OpStatus Retry(OpsManager& ops, uint64_t task_id, bool close) {
    const uint64_t before = ops.Status().completed_ops;
    if (!ops.RetryLockedOperation(task_id, close)) return OpStatus{};
    if (!WaitDone(ops, before)) std::printf("[INFO] retry timed out\n");
    return ops.Status();
}

void PureChecks() {
    Check(IsLockLikeError(HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION)), "sharing violation is lock-like");
    Check(IsLockLikeError(HRESULT_FROM_WIN32(ERROR_LOCK_VIOLATION)), "lock violation is lock-like");
    Check(IsLockLikeError(static_cast<HRESULT>(0x80270027L)), "copy engine source sharing violation");
    Check(IsLockLikeError(static_cast<HRESULT>(0x80270028L)), "copy engine destination sharing violation");
    Check(!IsLockLikeError(E_FAIL), "E_FAIL is not lock-like");
    // One file in use is the item's failure, not the task's (its low word
    // is ERROR_HANDLE_DISK_FULL's).
    Check(!IsStopTransferError(static_cast<HRESULT>(0x80270027L)), "copy engine sharing violation goes on");
    Check(!IsStopTransferError(static_cast<HRESULT>(0x8027003BL)), "copy engine name too long goes on");
    Check(IsStopTransferError(static_cast<HRESULT>(0x80270032L)), "copy engine disk full stops");
    Check(IsStopTransferError(HRESULT_FROM_WIN32(ERROR_HANDLE_DISK_FULL)), "Win32 disk full stops");
    Check(IsStopTransferError(STG_E_MEDIUMFULL), "storage medium full stops");
    Check(IsStopTransferError(static_cast<HRESULT>(ERROR_DISK_FULL)), "bare Win32 disk full stops");
    Check(!IsStopTransferError(HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION)), "Win32 sharing violation goes on");
    Check(!IsLockLikeError(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)), "not found is not lock-like");
    Check(!IsLockLikeError(S_OK), "success is not lock-like");

    Check(FailedPathFromError(L"另一个程序正在使用此文件。 | C:\\a\\b.txt") == L"C:\\a\\b.txt",
          "path after the last separator");
    Check(FailedPathFromError(L"D:\\only\\path") == L"D:\\only\\path", "bare path error");
    Check(FailedPathFromError(L"\\\\server\\share\\x | \\\\server\\share\\y") == L"\\\\server\\share\\y",
          "UNC path");
    Check(FailedPathFromError(L"操作失败").empty(), "message without path");
    Check(FailedPathFromError(L"无法创建目标目录：C:\\x | 目标已存在且不是文件夹").empty(),
          "non-path tail");

    LockOwner word;
    word.pid = 1234;
    word.app_name = L"Microsoft Word";
    word.image_name = L"WINWORD.EXE";
    Check(DescribeLockOwner(word) == L"Microsoft Word (WINWORD.EXE, PID 1234)", "describe app + image");
    LockOwner bare;
    bare.pid = 7;
    bare.image_name = L"tool.exe";
    Check(DescribeLockOwner(bare) == L"tool.exe (PID 7)", "describe image only");
    LockOwner same;
    same.pid = 8;
    same.app_name = L"notepad.exe";
    same.image_name = L"NOTEPAD.EXE";
    Check(DescribeLockOwner(same) == L"notepad.exe (PID 8)", "app name equal to image is not repeated");
    std::vector<LockOwner> many(7, bare);
    const std::wstring lines = FormatLockOwnerLines(many, 5);
    size_t breaks = 0;
    for (wchar_t c : lines) breaks += c == L'\n';
    Check(breaks == 5 && lines.back() == L'\x2026', "owner list capped at 5 lines plus ellipsis");
    Check(FormatLockOwnerLines({}).empty(), "empty owner list");
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 4 && std::wstring(argv[1]) == L"--hold") return HoldMode(argv[2], argv[3]);
    setvbuf(stdout, nullptr, _IONBF, 0);
    // Failure reasons are localized text, as in the app.
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");

    PureChecks();

    const fsys::path root = fsys::absolute(fsys::path(L"bench_data") /
        (L"file-lock-" + std::to_wstring(GetCurrentProcessId())));
    std::error_code ec;
    fsys::remove_all(root, ec);
    fsys::create_directories(root / L"holder");
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    g_holder_exe = root / L"holder" / L"lock_holder.exe";
    Check(CopyFileW(self, g_holder_exe.c_str(), FALSE) != FALSE, "holder copy created");
    const std::wstring exe_dir = CurrentModuleDirectory();
    Check(!exe_dir.empty() && fsys::exists(fsys::path(exe_dir) / fsys::path(self).filename()),
          "current module directory");

    // --- this process holds a file: reported, never closable ---------------
    {
        const fsys::path own = root / L"own.txt";
        Touch(own);
        HANDLE file = CreateFileW(own.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        const auto owners = FindLockOwners({own.wstring()}, exe_dir);
        const LockOwner* me = FindPid(owners, GetCurrentProcessId());
        Check(me != nullptr, "own handle found by Restart Manager");
        Check(me && !me->closable, "own process is not closable");
        Check(me && CloseLockOwner(*me, 100) == CloseOwnerResult::Refused, "closing self is refused");
        CloseHandle(file);
        Check(FindPid(FindLockOwners({own.wstring()}, exe_dir), GetCurrentProcessId()) == nullptr,
              "closed handle no longer reported");
    }

    // --- another process holds a file ----------------------------------------
    {
        const fsys::path locked = root / L"a" / L"locked.txt";
        Touch(locked);
        Holder holder;
        Check(StartHolder(holder, locked), "holder started");
        const auto owners = FindLockOwners({locked.wstring()}, exe_dir);
        const LockOwner* other = FindPid(owners, holder.pi.dwProcessId);
        Check(other != nullptr, "holder found");
        Check(other && other->closable, "holder closable");
        Check(other && _wcsicmp(other->image_name.c_str(), L"lock_holder.exe") == 0, "holder image name");
        const auto protected_owners = FindLockOwners({locked.wstring()}, (root / L"holder").wstring());
        const LockOwner* guarded = FindPid(protected_owners, holder.pi.dwProcessId);
        Check(guarded && !guarded->closable, "process in the protected folder is not closable");
        if (other) {
            LockOwner stale = *other;
            stale.start_time.dwLowDateTime ^= 1;
            Check(CloseLockOwner(stale, 100) == CloseOwnerResult::AlreadyGone && holder.Alive(),
                  "pid with a different start time is not terminated");
            Check(CloseLockOwner(*other, 5000) == CloseOwnerResult::Closed && !holder.Alive(),
                  "holder terminated");
            Check(DeleteFileW(locked.c_str()) != FALSE, "file deletable after the owner ended");
            Check(CloseLockOwner(*other, 100) == CloseOwnerResult::AlreadyGone, "second close is a no-op");
        }
    }

    // --- inside Pulse's own folder: helpers yes, Pulse itself no -------------
    {
        const fsys::path app_dir = root / L"app";
        fsys::create_directories(app_dir);
        const fsys::path helper_copy = app_dir / L"Pulse.Index.exe";
        const fsys::path window_copy = app_dir / L"pulse.exe";
        std::error_code copy_error;
        fsys::copy_file(g_holder_exe, helper_copy, fsys::copy_options::overwrite_existing, copy_error);
        fsys::copy_file(g_holder_exe, window_copy, fsys::copy_options::overwrite_existing, copy_error);

        // The index host is what usually holds a portable copy being deleted;
        // it restarts on demand, so ending it must be offered.
        const fsys::path helper_file = root / L"helper" / L"held.txt";
        Touch(helper_file);
        Holder helper;
        Check(StartHolder(helper, helper_file, helper_copy), "helper holder started");
        const auto helper_owners = FindLockOwners({helper_file.wstring()}, app_dir.wstring());
        const LockOwner* helper_owner = FindPid(helper_owners, helper.pi.dwProcessId);
        Check(helper_owner && helper_owner->closable,
              "Pulse's restartable helper stays closable inside Pulse's own folder");

        // The window process itself is never ended, not even to free a file.
        const fsys::path window_file = root / L"window" / L"held.txt";
        Touch(window_file);
        Holder window;
        Check(StartHolder(window, window_file, window_copy), "window holder started");
        const auto window_owners = FindLockOwners({window_file.wstring()}, app_dir.wstring());
        const LockOwner* window_owner = FindPid(window_owners, window.pi.dwProcessId);
        Check(window_owner && !window_owner->closable,
              "Pulse itself is not closable inside its own folder");
    }

    // --- folder probe + full failure probe ------------------------------------
    {
        const fsys::path dir = root / L"folder";
        const fsys::path inner = dir / L"sub" / L"inner.txt";
        Touch(inner);
        Touch(dir / L"free.txt");
        Holder holder;
        Check(StartHolder(holder, inner), "folder holder started");
        const auto files = LockProbeFiles(dir.wstring());
        Check(files.size() == 2, "folder probe lists nested files");
        Check(LockProbeFiles(dir.wstring(), 1).size() == 1, "folder probe honours the limit");
        const LockReport report = ProbeLockFailure(HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED),
            L"拒绝访问。 | " + dir.wstring(), {}, exe_dir);
        Check(report.path == dir.wstring() && FindPid(report.owners, holder.pi.dwProcessId),
              "access denied on a folder finds the owner inside");
        Check(ProbeLockFailure(E_FAIL, L"x | " + dir.wstring(), {}, exe_dir).Empty(),
              "non-lock error is not probed");
        const std::wstring free_path = (dir / L"free.txt").wstring();
        const LockReport single = ProbeLockFailure(HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION),
            L"另一个程序正在使用此文件。", {inner.wstring()}, exe_dir);
        Check(single.path == inner.wstring() && FindPid(single.owners, holder.pi.dwProcessId),
              "single source used when the error has no path");
        const LockReport by_name = ProbeLockFailure(static_cast<HRESULT>(0x80270027L), L"inner.txt",
            {free_path, inner.wstring()}, exe_dir);
        Check(by_name.path == inner.wstring() && FindPid(by_name.owners, holder.pi.dwProcessId),
              "display name matched against the sources");
        const LockReport nested = ProbeLockFailure(static_cast<HRESULT>(0x80270027L), L"inner.txt",
            {dir.wstring(), (root / L"own.txt").wstring()}, exe_dir);
        Check(nested.path == inner.wstring() && FindPid(nested.owners, holder.pi.dwProcessId),
              "name inside a source folder: all sources probed, item resolved");
        const LockReport message_only = ProbeLockFailure(HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION),
            L"另一个程序正在使用此文件。", {dir.wstring(), free_path}, exe_dir);
        Check(message_only.path == dir.wstring() && FindPid(message_only.owners, holder.pi.dwProcessId),
              "bare message never becomes the item name");
        Check(ProbeLockFailure(static_cast<HRESULT>(0x80270027L), L"inner.txt", {free_path}, exe_dir).Empty(),
              "no owner among the sources: empty report");
        // NTFS lists free.txt before sub\, so a one-file budget never reaches inner.txt.
        Check(ProbeLockFailure(static_cast<HRESULT>(0x80270027L), L"x.txt", {dir.wstring()}, exe_dir, 1)
              .Empty(), "probe limit applies across sources");
    }

    // --- OpsManager end to end --------------------------------------------------
    OpsManager ops;
    ops.Start([] {});
    {
        const fsys::path src = root / L"move-src" / L"m.txt";
        const fsys::path dest = root / L"move-dest";
        Touch(src);
        fsys::create_directories(dest);
        Holder holder;
        Check(StartHolder(holder, src), "move holder started");
        OpRequest move;
        move.type = OpType::Move;
        move.sources = {src.wstring()};
        move.dest_dir = dest.wstring();
        OpStatus st = Run(ops, move);
        Check(st.phase == OpPhase::Failed && FindPid(st.lock_owners, holder.pi.dwProcessId),
              "locked move reports the owner");
        Check(st.locked_path == src.wstring(), "locked move reports the item");
        Check(!ops.RetryLockedOperation(st.task_id + 1000, true), "retry needs the failed task id");
        st = Retry(ops, st.task_id, false);
        Check(st.phase == OpPhase::Failed && FindPid(st.lock_owners, holder.pi.dwProcessId) &&
              holder.Alive(), "plain retry keeps the owner and fails again");
        st = Retry(ops, st.task_id, true);
        Check(st.phase == OpPhase::Completed && fsys::exists(dest / L"m.txt") && !fsys::exists(src),
              "end-process retry completes the move");
        Check(!holder.Alive(), "move owner ended");
        Check(st.lock_owners.empty() && st.locked_path.empty(), "lock info cleared after success");
    }
    {
        // A moves, B is locked. The user closes the owner and presses plain
        // retry: the retry must skip A (already moved) and finish B.
        const fsys::path a = root / L"partial-src" / L"a.txt";
        const fsys::path b = root / L"partial-src" / L"b.txt";
        const fsys::path dest = root / L"partial-dest";
        Touch(a);
        Touch(b);
        fsys::create_directories(dest);
        Holder holder;
        Check(StartHolder(holder, b), "partial move holder started");
        OpRequest move;
        move.type = OpType::Move;
        move.sources = {a.wstring(), b.wstring()};
        move.dest_dir = dest.wstring();
        OpStatus st = Run(ops, move);
        std::printf("[INFO] partial move phase=%d a_moved=%d\n", static_cast<int>(st.phase),
                    fsys::exists(dest / L"a.txt") ? 1 : 0);
        Check(st.phase == OpPhase::Failed && fsys::exists(b), "partial move fails on the locked item");
        holder.Stop();
        st = Retry(ops, st.task_id, false);
        Check(st.phase == OpPhase::Completed && fsys::exists(dest / L"a.txt") &&
              fsys::exists(dest / L"b.txt") && !fsys::exists(a) && !fsys::exists(b),
              "plain retry after the owner closed skips moved items and completes");
    }
    {
        const fsys::path folder = root / L"dir-src" / L"project";
        const fsys::path inner = folder / L"doc.txt";
        const fsys::path dest = root / L"dir-dest";
        Touch(inner);
        fsys::create_directories(dest);
        Holder holder;
        Check(StartHolder(holder, inner), "folder move holder started");
        OpRequest move;
        move.type = OpType::Move;
        move.sources = {folder.wstring()};
        move.dest_dir = dest.wstring();
        OpStatus st = Run(ops, move);
        std::printf("[INFO] folder move phase=%d owners=%zu\n", static_cast<int>(st.phase),
                    st.lock_owners.size());
        Check(st.phase == OpPhase::Failed && FindPid(st.lock_owners, holder.pi.dwProcessId),
              "locked file inside a moved folder reports the owner");
        Check(st.authorization == AuthorizationState::None,
              "locked folder move does not request administrator permission");
        st = Retry(ops, st.task_id, true);
        Check(st.phase == OpPhase::Completed && fsys::exists(dest / L"project" / L"doc.txt"),
              "folder move completes after ending the owner");
    }
    {
        const fsys::path src = root / L"copy-src" / L"c.txt";
        const fsys::path dest = root / L"copy-dest";
        Touch(src);
        fsys::create_directories(dest);
        Holder holder;
        Check(StartHolder(holder, src), "copy holder started");
        OpRequest copy;
        copy.type = OpType::Copy;
        copy.sources = {src.wstring()};
        copy.dest_dir = dest.wstring();
        OpStatus st = Run(ops, copy);
        Check(st.phase == OpPhase::Failed && FindPid(st.lock_owners, holder.pi.dwProcessId),
              "locked copy source reports the owner");
        st = Retry(ops, st.task_id, true);
        Check(st.phase == OpPhase::Completed && fsys::exists(dest / L"c.txt") && fsys::exists(src),
              "copy completes after ending the owner");
    }
    {
        // Permanent delete through pulse_shell (IFileOperation) of fixture files only.
        const fsys::path free_file = root / L"del" / L"free.txt";
        const fsys::path locked = root / L"del" / L"locked.txt";
        Touch(free_file);
        Touch(locked);
        Holder holder;
        Check(StartHolder(holder, locked), "delete holder started");
        OpRequest del;
        del.type = OpType::RealDelete;
        del.sources = {free_file.wstring(), locked.wstring()};
        OpStatus st = Run(ops, del);
        std::printf("[INFO] delete phase=%d owners=%zu error_len=%zu\n", static_cast<int>(st.phase),
                    st.lock_owners.size(), st.last_error.size());
        Check(st.phase == OpPhase::Failed && FindPid(st.lock_owners, holder.pi.dwProcessId),
              "locked shell delete reports the owner");
        Check(fsys::exists(locked), "locked file survived the failed delete");
        st = Retry(ops, st.task_id, true);
        Check(st.phase == OpPhase::Completed && !fsys::exists(locked) && !fsys::exists(free_file),
              "delete retry completes and skips items already gone");
    }
    {
        // A bulk delete goes past items in use: the rest are deleted, the
        // held ones are listed with their owner, and 重试失败项 deletes them
        // once released.
        std::vector<fsys::path> files;
        for (int i = 0; i < 6; ++i) {
            files.push_back(root / L"bulk" / (L"f" + std::to_wstring(i) + L".txt"));
            Touch(files.back());
        }
        Holder first, second;
        Check(StartHolder(first, files[1]) && StartHolder(second, files[4]), "bulk delete holders started");
        OpRequest del;
        del.type = OpType::RealDelete;
        for (const auto& file : files) del.sources.push_back(file.wstring());
        OpStatus st = Run(ops, del);
        std::printf("[INFO] bulk delete phase=%d failed=%llu completed=%llu\n", static_cast<int>(st.phase),
                    static_cast<unsigned long long>(st.failed_items),
                    static_cast<unsigned long long>(st.completed_items));
        const bool rest_gone = !fsys::exists(files[0]) && !fsys::exists(files[2]) && !fsys::exists(files[3]) &&
            !fsys::exists(files[5]);
        Check(st.phase == OpPhase::Failed && rest_gone && fsys::exists(files[1]) && fsys::exists(files[4]),
              "bulk delete goes past items in use");
        Check(st.failed_items == 2 && st.failures.size() == 2 && st.completed_items == 4,
              "bulk delete lists the items it could not delete");
        Check(st.failures.size() == 2 && _wcsicmp(st.failures[0].source.c_str(), files[1].c_str()) == 0 &&
              _wcsicmp(st.failures[1].source.c_str(), files[4].c_str()) == 0 && st.failures[0].destination.empty(),
              "failed delete items keep their order and have no destination");
        const auto owners = FindLockOwners({files[1].wstring()}, exe_dir);
        const LockOwner* owner = FindPid(owners, first.pi.dwProcessId);
        const std::wstring owner_name = !owner ? std::wstring{}
            : owner->app_name.empty() ? owner->image_name : owner->app_name;
        if (!st.failures.empty())
            std::printf("[INFO] bulk failure hr=0x%08X reason=%s owner=%s\n",
                        static_cast<unsigned>(st.failures[0].hr), Utf8(st.failures[0].reason).c_str(),
                        Utf8(owner_name).c_str());
        Check(st.failures.size() == 2 && !owner_name.empty() &&
              st.failures[0].reason.find(owner_name) != std::wstring::npos,
              "a failed delete item names the program holding it");
        first.Stop();
        second.Stop();
        const uint64_t before = ops.Status().completed_ops;
        const auto spawns = ops.RetryFailedItems(st.task_id);
        Check(spawns.size() == 1 && spawns[0].items.size() == 2, "failed delete items retry as one task");
        if (!WaitDone(ops, before)) std::printf("[INFO] failed-items retry timed out\n");
        st = ops.Status();
        Check(st.phase == OpPhase::Completed && !fsys::exists(files[1]) && !fsys::exists(files[4]),
              "retried delete items are gone");
    }
    {
        // Held for a moment only (a thumbnail being made): tried again shortly.
        const fsys::path brief = root / L"brief" / L"b.txt";
        Touch(brief);
        HANDLE held = CreateFileW(brief.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        Check(held != INVALID_HANDLE_VALUE, "brief holder opened");
        std::thread release([held] {
            Sleep(150);
            CloseHandle(held);
        });
        OpRequest del;
        del.type = OpType::RealDelete;
        del.sources = {brief.wstring()};
        const OpStatus st = Run(ops, del);
        release.join();
        Check(st.phase == OpPhase::Completed && st.failed_items == 0 && !fsys::exists(brief),
              "an item in use for a moment is deleted on a later try");
    }
    {
        // Unrelated failures carry no lock info and cannot be lock-retried.
        OpRequest move;
        move.type = OpType::Move;
        move.sources = {(root / L"missing.txt").wstring()};
        move.dest_dir = (root / L"move-dest").wstring();
        const OpStatus st = Run(ops, move);
        Check(st.phase == OpPhase::Failed && st.lock_owners.empty() && st.locked_path.empty(),
              "missing source has no lock info");
        Check(!ops.RetryLockedOperation(st.task_id, true), "no lock retry for unrelated failures");
    }
    ops.Stop();

    fsys::remove_all(root, ec);
    std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}

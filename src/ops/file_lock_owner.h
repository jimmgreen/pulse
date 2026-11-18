// file_lock_owner.h — Which processes keep a file open (Restart Manager).
//
// Used after a delete/move/copy failed with a sharing/lock style error: the
// ops worker asks Restart Manager which processes hold the failed item (or, for
// a folder, a bounded set of files inside it) and the UI offers to end them and
// retry. Every function here may block on the file system or on other
// processes; call them from worker threads only, never from the UI thread.
#pragma once
#include <windows.h>
#include <cstddef>
#include <string>
#include <vector>

namespace pulse::ops {

struct LockOwner {
    DWORD pid = 0;
    FILETIME start_time{};      // with pid: identifies the process instance
    std::wstring app_name;      // Restart Manager display name
    std::wstring image_name;    // "WINWORD.EXE"; empty when the process is not queryable
    // false: Pulse itself, services, critical processes and Explorer. Pulse's
    // own helpers (index, preview, content, AI, shell) stay closable: they
    // restart on demand.
    bool closable = true;
};

struct LockReport {
    std::wstring path;          // item the operation failed on
    std::vector<LockOwner> owners;
    bool Empty() const noexcept { return owners.empty(); }
};

// True for errors that an open handle in another process can cause. Callers
// must still find an owner before blaming one: access denied may also be a
// real permission problem.
bool IsLockLikeError(HRESULT hr) noexcept;

// Ops error text is "<message> | <path>" (or only "<path>" when the system has
// no message for the code). Returns the path part, or empty.
std::wstring FailedPathFromError(const std::wstring& error);

// The file itself, or up to `limit` regular files below a folder (reparse
// points are not followed).
std::vector<std::wstring> LockProbeFiles(const std::wstring& path, size_t limit = 256);

// Restart Manager query. Processes whose image lives in `protected_dir`
// (Pulse's own install folder) and this process are reported but not closable,
// except Pulse's restartable helpers (Pulse.Index/Preview/Content/AI/...).
std::vector<LockOwner> FindLockOwners(const std::vector<std::wstring>& files,
                                      const std::wstring& protected_dir);

// Full probe used by the ops worker: classify `hr`, then find the failed item.
// The error may carry a full path, only a display name (pulse_shell reports
// IFileOperation item names) or nothing; a name is matched against `sources`,
// otherwise every source (folders expanded, `limit` files in total) is probed
// and the item is the probed file with that name, else the first source.
LockReport ProbeLockFailure(HRESULT hr, const std::wstring& error,
                            const std::vector<std::wstring>& sources,
                            const std::wstring& protected_dir, size_t limit = 256);

enum class CloseOwnerResult { Closed, AlreadyGone, Refused, Denied, TimedOut, Failed };

// Terminates the owner only if `pid` still names the same process instance
// (start time match) and it is closable; waits up to `wait_ms` for the exit.
CloseOwnerResult CloseLockOwner(const LockOwner& owner, DWORD wait_ms, DWORD* error = nullptr);

// Same kill through pulse_integration.exe started with "runas", for owners the
// window may not end (services, other users, higher integrity). One UAC prompt
// per call; Denied means the user cancelled it. Blocks until the helper exits,
// so call it from a worker thread.
CloseOwnerResult CloseLockOwnerElevated(const LockOwner& owner, DWORD wait_ms, DWORD* error = nullptr);

// "Microsoft Word (WINWORD.EXE, PID 1234)" — language neutral.
std::wstring DescribeLockOwner(const LockOwner& owner);
// One bullet line per owner, at most `max_lines` (then "…").
std::wstring FormatLockOwnerLines(const std::vector<LockOwner>& owners, size_t max_lines = 5);

// Directory of the running executable, without trailing separator.
std::wstring CurrentModuleDirectory();

} // namespace pulse::ops

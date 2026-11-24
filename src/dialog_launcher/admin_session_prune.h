#pragma once
#include <windows.h>
#include <string>

namespace pulse::dialog_launcher::admin_auto {
inline constexpr ULONGLONG kSessionPruneIntervalMs = 10ull * 60 * 1000;
inline constexpr ULONGLONG kSessionPruneIdleMs = 24ull * 60 * 60 * 1000;
struct SessionPruneReport {
    unsigned removed = 0;   // exited target, all members deleted
    unsigned live = 0;      // target still running (or liveness unknown)
    unsigned recent = 0;    // exited, but written within the grace period
    unsigned retained = 0;  // not provably ours or not removable; left untouched
    bool limited = false;   // enumeration bound reached
};
// Removes GUID-named session directories under the admin control root whose
// session.bin names this directory and whose target process has exited. A
// session written within min_idle_ms is kept so its decision logs stay
// available to diagnostics shortly after the target closes. Unknown names,
// subdirectories, unreadable or mismatched session files, busy members and
// targets that cannot be queried are retained.
SessionPruneReport PruneExitedSessions(const std::wstring& control_root, ULONGLONG min_idle_ms) noexcept;
}

// shell_namespace_cache.h — shell namespace items for the This PC sidebar.
//
// Explorer lists a phone over MTP, a cloud-drive folder and third-party
// namespace extensions under This PC; Pulse only listed drive letters, because
// GetLogicalDrives() returns nothing else. The items come from pulse_shell over
// the pipe (the UI process does not touch Shell COM), so they cannot be fetched
// inline while the sidebar is being built.
//
// Instead the rows are served from a process-wide cache: the sidebar worker
// asks for a refresh, the answer arrives on the pipe thread, and the next
// rebuild picks it up. Tests and the very first frame simply see an empty list
// rather than blocking, which is the same trade the context-menu cache makes.
#pragma once
#include "../ipc/protocol.h"
#include <string>
#include <vector>

namespace pulse::app {

// Items to show under This PC that are not drive letters, newest snapshot.
// Safe to call from any thread.
std::vector<ipc::ShellItem> CachedShellRoots();

// Ask pulse_shell for the current list unless a request is already in flight.
// Cheap to call on every sidebar rebuild. The answer lands in the cache and the
// caller is told whether anything changed through the return of
// TakeShellRootsChanged().
void RequestShellRootsRefresh();

// True once per completed refresh that actually changed the list, so the caller
// can rebuild the sidebar exactly when there is something new to show.
bool TakeShellRootsChanged();

// Where the shell client hands an answer: called on its reader thread. Answers
// are routed by request id, because the same callback serves the This PC roots
// and a folder listing. Returns false when the id belongs to neither, so the
// caller can ignore it.
bool DispatchShellItems(uint32_t request_id, std::vector<ipc::ShellItem> items, bool ok);

// Enumerate one shell folder and wait for the answer. Blocking, and must not be
// called on the UI thread: the app worker uses it when a pane enters a device or
// a cloud-drive folder, which has no Win32 enumeration path.
bool ListShellFolderBlocking(const std::wstring& shell_path,
                             std::vector<ipc::ShellItem>& out, int timeout_ms = 15000);

} // namespace pulse::app

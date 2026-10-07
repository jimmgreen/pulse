// shell_items.h — Shell namespace enumeration for pulse_shell (STA host).
//
// "This PC" in Explorer is a shell namespace. Pulse built its drive list from
// GetLogicalDrives(), which only returns letters, so anything the shell adds
// without one - a phone over MTP, a cloud drive's folder, a third-party
// namespace extension - never appeared.
//
// This header is the host-side half of the fix: it turns the shell's items into
// plain strings and numbers that can travel over the existing pipe. The UI
// process never touches Shell COM, per the project's isolation rule.
#pragma once
#include "../ipc/protocol.h"
#include <string>
#include <vector>

namespace pulse::shell {

// True when the string is one of our shell namespace paths (pulse:shell:...).
bool IsShellPath(const std::wstring& path);

// The Desktop-absolute parsing name behind a pulse:shell: path; empty when the
// string is not one. Duplicated here on purpose: pulling src/fs/fs_enum.cpp into
// this host would drag in the ntdll-based enumerator, and the host only needs
// this one string operation.
std::wstring ParsingNameOfPath(const std::wstring& path);

// Build the path a UI row navigates to for an item with this parsing name.
std::wstring MakeShellPath(const std::wstring& parsing_name);

// Items under This PC that are not plain drive letters, for REQ_SHELL_ROOTS.
// Drive roots are excluded because the UI already lists them from
// GetLogicalDrives(); everything else - portable devices, cloud-drive folders,
// namespace extensions - is what the Win32 enumeration cannot see.
//
// Must run on the STA thread that owns the shell.
std::vector<ipc::ShellItem> EnumerateThisPcExtras();

// Children of one shell folder, for REQ_SHELL_LIST. `parsing_name` is the
// Desktop-absolute parsing name behind a pulse:shell: path. Returns false when
// the name cannot be resolved at all (a stale or malformed path), so the caller
// can report a failure instead of an empty folder.
//
// Must run on the STA thread.
bool EnumerateShellFolder(const std::wstring& parsing_name,
                          std::vector<ipc::ShellItem>& out);

} // namespace pulse::shell

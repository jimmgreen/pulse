// folder_open_com.h — folder-open delegate (see folder_open_com.cpp).
#pragma once

#include <windows.h>
// Pull the MIDL interfaces in at global scope, before this header's namespace
// exists. `interface IStream;` inside shobjidl.h is resolved by name, so if the
// namespace were already open the declaration would land in it and every later
// use would fail with "undefined base class".
#include <objidl.h>
#include <shobjidl.h>

#include <string>
#include <vector>

namespace pulse::app::folder_open {

// A folder the shell asked us to open on behalf of another program's "open file
// location". `target` is the entry to put the cursor on when the caller could
// tell us which one; the shell only ever hands us the folder, so it is usually
// empty (see folder_open_com.cpp).
struct Request {
    std::wstring folder;
    std::wstring target;
    std::wstring verb;
};

// Registers the CLSID the Folder\shell\open DelegateExecute value points at.
// Call on the owning STA; revoke before COM shutdown.
HRESULT RegisterCommandServer(const CLSID* class_override = nullptr);
void RevokeCommandServer();

// Requests collected since the last call, for the window to open.
std::vector<Request> TakeRequests();
bool CommandServerBusy();

// The entry to focus when the caller named none. The shell never delivers the
// file to the delegate (measured; see folder_open_com.cpp), and its
// IShellView::SelectItem push does not reach a delegate-opened view either, so
// the window resolves the target itself. A launcher's "open file location" is
// nearly always about the item it just wrote, which is the newest one here.
std::wstring NewestEntryPath(const std::wstring& folder);

// Appends one line to %LOCALAPPDATA%\Pulse\pulse_folder_open.log. A folder open
// that silently does nothing cannot be diagnosed from outside, and the shell
// reports no error for it.
void LogDelegate(const wchar_t* format, ...);

} // namespace pulse::app::folder_open

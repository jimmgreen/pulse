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

} // namespace pulse::app::folder_open

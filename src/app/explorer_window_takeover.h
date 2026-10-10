// explorer_window_takeover.h — Experimental (B站 #1 phase 2b): File Explorer
// windows that programs open directly (explorer.exe /select,<file>, which
// bypasses the folder verbs and IShellWindows lookups) are opened in Pulse.
// A source is closed only after navigation/selection acknowledgement, another
// source identity check, and proof that the source holds no other tab.
//
// A new Explorer window is made fully transparent the moment it is shown, so
// it does not flash up before Pulse; any window that is not taken (virtual
// locations, failed hand-offs, Pulse stopping) is made visible again.
//
// Only windows that appear while this runs are touched; windows already open
// are left alone, as are virtual locations Pulse cannot show and any window
// opened with Shift held. Every call into Explorer runs on this object's own
// STA thread.
#pragma once
#include <windows.h>
#include <memory>
#include <string>
#include <vector>
#include "explorer_handoff.h"

namespace pulse::app {

struct ExplorerTakeoverRequest {
    std::wstring folder;               // file system folder; empty = This PC
    std::vector<std::wstring> names;   // items selected in the Explorer window
    std::shared_ptr<ExplorerHandoff> handoff;
    bool selection_read = false;
    // Follow-up without a hand-off: select `names` in the tab already opened
    // for `folder` (a /select that applied after the folder was handed over).
    bool select_only = false;
};

class ExplorerWindowTakeover {
public:
    // Requests are posted to `window` as `message` with a heap-allocated
    // ExplorerTakeoverRequest* in lParam (the receiver deletes it).
    ExplorerWindowTakeover(HWND window, UINT message);
    ~ExplorerWindowTakeover();
    ExplorerWindowTakeover(const ExplorerWindowTakeover&) = delete;
    ExplorerWindowTakeover& operator=(const ExplorerWindowTakeover&) = delete;

    // Ends the thread, waiting at most `timeout_ms`.
    void Stop(DWORD timeout_ms = 2000);

    struct Shared;

private:
    std::shared_ptr<Shared> shared_;
    HANDLE thread_ = nullptr;
};

} // namespace pulse::app

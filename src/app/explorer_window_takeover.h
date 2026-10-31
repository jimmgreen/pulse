// explorer_window_takeover.h — Experimental (B站 #1 phase 2b): File Explorer
// windows that programs open directly (explorer.exe /select,<file>, which
// bypasses the folder verbs and IShellWindows lookups) are opened in Pulse.
// A source is closed only after navigation/selection acknowledgement, another
// source identity check, and proof that the source holds no other tab.
//
// The source remains visible and usable until the acknowledged handoff is
// committed. Failed, cancelled or changed requests leave it untouched.
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

struct ExplorerSourceScope;
class ExplorerSubmissionPort;

struct ExplorerTakeoverRequest {
    std::wstring folder;               // file system folder; empty = This PC
    std::vector<std::wstring> names;   // items selected in the Explorer window
    std::shared_ptr<ExplorerHandoff> handoff;
    bool selection_read = false;
};

class ExplorerWindowTakeover {
public:
    // The port owns queued immutable requests; messages only wake its consumer.
    ExplorerWindowTakeover(std::shared_ptr<ExplorerSubmissionPort> port,
                           std::shared_ptr<const ExplorerSourceScope> scope = {});
    ~ExplorerWindowTakeover();
    ExplorerWindowTakeover(const ExplorerWindowTakeover&) = delete;
    ExplorerWindowTakeover& operator=(const ExplorerWindowTakeover&) = delete;

    // Ends the thread, waiting at most `timeout_ms`.
    void Stop(DWORD timeout_ms = 2000);
    // Whether This PC windows are taken too (「此电脑」 is handed to Pulse).
    void SetTakeThisPc(bool on);

    struct Shared;

private:
    std::shared_ptr<Shared> shared_;
    HANDLE thread_ = nullptr;
};

} // namespace pulse::app

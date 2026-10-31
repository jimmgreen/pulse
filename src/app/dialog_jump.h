// dialog_jump.h - the jump bar under other programs' Open / Save dialogs.
//
// While a common file dialog of another (non-elevated) program is in front, a
// slim bar sits under it: a search box (folders Pulse knows instantly, then
// the index, pinyin included), the folders open in Pulse as chips, the
// current Pulse folder on Ctrl+G and a menu of Quick access / recent places.
// Jumps are remembered per program (JumpHistory): frequent places rank first
// and show when the empty search box takes the focus.
// Picking one moves the dialog there; a typed Save name is kept, and in an
// Open dialog a file is filled in and opened. The dialog itself is driven
// through its address bar for folders in modern dialogs; legacy dialogs
// and file selections use the file name box. No separate window is opened.
//
// Everything runs on a thread of its own: Pulse's UI thread only answers the
// context request (which folders are open) when a dialog shows up.
#pragma once
#include <windows.h>
#include <d2d1.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pulse::app {
class JumpHistory;
struct DialogJumpFolder {
    std::wstring path, label;   // label: shown name when it is not the folder name
    bool open = false;          // open in a Pulse tab (shown as a chip)
    bool pinned = false;        // Quick access (the rest are recent folders)
};
struct DialogJumpContext {
    std::vector<DialogJumpFolder> folders;  // most relevant first
    std::wstring current;                    // the active Pulse tab's folder
    bool dark = false;
    D2D1_COLOR_F accent{0.0f, 0.47f, 0.84f, 1.0f};
    bool pinyin = true;
    bool index = true;                       // search master switch on
    bool remember = true;                    // record jumps, frequent places first
    bool auto_jump = false;                  // a new dialog goes to `current` once
};

class DialogJump {
public:
    DialogJump();
    ~DialogJump();
    DialogJump(const DialogJump&) = delete;
    DialogJump& operator=(const DialogJump&) = delete;
    // When a dialog appears, `context_message` is posted to `service`; the
    // receiver answers with SetContext.
    void Update(HWND service, UINT context_message, bool enabled);
    void Stop();
    void SetContext(DialogJumpContext context);
    // Where jumps are recorded and frequent places come from; set before Update.
    void SetHistory(std::shared_ptr<JumpHistory> history);
    // Two Ctrl taps while a dialog with a bar is in front: focus the bar's
    // search box instead of opening quick search. True when it did.
    bool FocusSearch();
    struct Impl;
private:
    std::shared_ptr<Impl> impl_;
    std::thread thread_;
};
}

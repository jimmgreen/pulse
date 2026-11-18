#pragma once

namespace pulse::app {
// Called after OLE/localization initialization, before main application state.
// owner_window (optional, decimal HWND): the caller's window. Used as the
// dialog's owner only when it belongs to parent_pid and the owner guard accepts
// it; otherwise the dialog is unowned, as before.
int RunPickerHostMode(const wchar_t* pipe_name, const wchar_t* parent_pid, const wchar_t* owner_window = nullptr);
// --picker-standby: a warmed-up host waiting on the per-user standby pipe for
// one request. It starts its successor when a verified client connects, and
// retires when interception no longer points at its bundle (or another
// standby of this bundle is already idle).
int RunPickerStandbyMode();
// From the main app: start a standby for the registered interception bundle
// when the pulse.exe route is on and none is idle. Worker thread; no waiting.
// Not called at startup yet: measured warm and cold hosts reach a ready dialog
// in the same time, because the dialog's WM_CREATE (220-400 ms) dominates
// (docs/dialog-out-of-process-plan-20261107.md, stage 4b).
void SeedPickerStandbyAsync();
}

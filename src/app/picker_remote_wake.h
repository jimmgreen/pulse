#pragma once
#include <windows.h>

namespace pulse::app {
// PeekMessage can mark a just-posted host message as old while checking for
// WM_QUIT. Keep a separate completion signal so that queue-status observation
// cannot consume the reader/writer wakeup. Do not use MWMO_INPUTAVAILABLE:
// unrelated picker input must stay queued without spinning during callbacks.
inline DWORD WaitPickerRemoteWake(HANDLE wake, DWORD timeout = INFINITE) {
    return MsgWaitForMultipleObjectsEx(1, &wake, timeout, QS_ALLINPUT, 0);
}
}

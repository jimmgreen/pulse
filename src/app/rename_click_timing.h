#pragma once
#include <windows.h>

namespace pulse::app {
inline bool RenameClickReady(ULONGLONG now, ULONGLONG due) {
    if (!due || now < due) return false;
    // The frame pump can run the UI timer ahead of queued input. Let a
    // pending second click cancel the candidate before creating the editor.
    if (HIWORD(GetQueueStatus(QS_INPUT | QS_POSTMESSAGE)) != 0) return false;
    return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0;
}
}

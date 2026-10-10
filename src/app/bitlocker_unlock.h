// bitlocker_unlock.h — the system BitLocker unlock prompt for a locked drive.
#pragma once

#include <windows.h>
#include <string>

namespace pulse::app {

struct BitLockerUnlockResult {
    std::wstring root;      // "W:\"
    bool unlocked = false;  // false: every prompt was closed with the drive still locked
};

// Shows the prompt File Explorer uses for 解锁驱动器 (the Drive "unlock-bde"
// verb, bdeunlock.exe) unless one is already open - Windows opens one by
// itself when a locked volume arrives - and waits on a background thread
// until the drive is unlocked or the prompts are gone. Posts msg to hwnd with
// LPARAM = BitLockerUnlockResult* (the receiver deletes it). Returns false
// when root is not a drive or a wait for it is already running.
bool StartBitLockerUnlock(HWND hwnd, UINT msg, const std::wstring& root);

} // namespace pulse::app

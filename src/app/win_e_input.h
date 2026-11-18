#pragma once
#include <windows.h>
#include "win_e_chord.h"

namespace pulse::app {
// Event history is diagnostic, not authority to capture a key: another hook
// can consume a release, leaving our observed modifiers stale. Routing uses the
// current asynchronous snapshot; foreground refresh restores hook visibility.
struct WinEInputState {
    static unsigned Modifier(DWORD key) noexcept {
        switch (key) {
        case VK_LWIN: return 1;
        case VK_RWIN: return 2;
        case VK_CONTROL: case VK_LCONTROL: return 4;
        case VK_RCONTROL: return 8;
        case VK_MENU: case VK_LMENU: return 16;
        case VK_RMENU: return 32;
        case VK_SHIFT: case VK_LSHIFT: return 64;
        case VK_RSHIFT: return 128;
        default: return 0;
        }
    }
    void ObserveModifier(DWORD key, bool down) noexcept {
        const unsigned bit = Modifier(key);
        if (down) modifiers |= bit;
        else modifiers &= ~bit;
    }
    bool Eligible(unsigned asynchronous) const noexcept {
        return (asynchronous & 3) != 0 && (asynchronous & 252) == 0;
    }
    unsigned modifiers = 0;
    WinEChord chord;
};
}

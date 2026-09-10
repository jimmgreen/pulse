#pragma once
#include <windows.h>

namespace pulse::ui {
// Hide the complete owned-window tree before tearing down composition resources.
// Pass the disabled owner for a modal dialog; omit it for a modeless window.
void HideComposedDialog(HWND dialog, HWND modal_owner = nullptr);
}

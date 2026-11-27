#pragma once
#include "notification_toast.h"

namespace pulse::ui {
// UI-thread only. The target receives only an explicitly clicked action. The
// unowned, nonactivating tool window remains visible when the target is hidden.
HWND ShowPickerSavedToast(HWND target, bool dark, D2D1_COLOR_F accent,
    std::wstring title, std::wstring detail, std::vector<SnackAction> actions);

#ifdef PULSE_PICKER_SAVED_TOAST_TEST
namespace picker_saved_toast_test {
bool Snapshot(HWND hwnd, const wchar_t* path);
D2D1_RECT_F Bounds(HWND hwnd);
void Expire(HWND hwnd);
void Resize(HWND hwnd, int width, float scale);
}
#endif
}

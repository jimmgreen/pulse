#pragma once
#include <windows.h>
#include <string>

namespace pulse {
struct AppState;
DWORD PrepareAnimationIntegrationDesktop(int argc, wchar_t** argv);

// Explicit test-instance adapter for the OS modal-move boundary only. No system
// cursor, async key state, display mode or user's desktop is changed by the probe.
struct AnimationIntegrationDragInput {
    POINT (*cursor)() = nullptr;
    bool (*primary_down)() = nullptr;
    bool (*move_loop)(AppState&, HWND source) = nullptr; // true means Escape
};
bool SetAnimationIntegrationDragInput(AppState&, const AnimationIntegrationDragInput*);
int RunAnimationIntegrationProbe(AppState&, const std::wstring& report_path);
}

#include "instance_launcher.h"
#include "single_instance_coordinator.h"

#include <windows.h>
#include <shellapi.h>
#include <cwchar>

namespace pulse::app {
namespace {

// One argument, quoted for CreateProcess/ShellExecute: a folder path may contain
// spaces, and the shell splits the argument string itself.
std::wstring Quoted(const std::wstring& text) {
    std::wstring quoted = L"\"";
    quoted.reserve(text.size() + 2);
    for (const wchar_t c : text) {
        if (c == L'"') quoted += L'\\';
        quoted += c;
    }
    quoted += L'"';
    return quoted;
}

} // namespace

bool LaunchNewWindow(const std::wstring& path) {
    wchar_t exe[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe)) == 0) return false;
    std::wstring arguments = L"--new-window";
    if (!path.empty()) arguments += L" " + Quoted(path);
    const auto result = ShellExecuteW(nullptr, L"open", exe, arguments.c_str(),
                                      nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

HWND PulseWindowUnderPoint(POINT screen_point, HWND exclude) {
    const HWND hit = WindowFromPoint(screen_point);
    if (!hit) return nullptr;
    // Popups and overlays belong to the window that owns them, so walk to the
    // root and then along the owner chain: the preview overlay, a dialog or a
    // menu over another instance is still that instance.
    for (HWND walk = GetAncestor(hit, GA_ROOT); walk; walk = GetWindow(walk, GW_OWNER)) {
        if (walk == exclude) return nullptr;
        wchar_t name[64]{};
        if (GetClassNameW(walk, name, ARRAYSIZE(name)) == 0) continue;
        // The drag card follows the pointer and has no owner: never a target.
        if (_wcsicmp(name, L"PulseTabDragGhost") == 0) return nullptr;
        if (_wcsicmp(name, SingleInstanceCoordinator::WindowClassName()) == 0) return walk;
    }
    return nullptr;
}

} // namespace pulse::app

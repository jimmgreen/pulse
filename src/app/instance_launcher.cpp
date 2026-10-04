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
    return LaunchNewWindow(path, {}, {});
}

bool LaunchNewWindow(const std::wstring& path, const std::vector<std::wstring>& selected_names,
                     const std::wstring& focus_name) {
    wchar_t exe[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe)) == 0) return false;
    std::wstring arguments = L"--new-window";
    if (!path.empty()) arguments += L" " + Quoted(path);
    // One flag per entry: a file name may contain any separator an argument list
    // could use, so the names never share one argument.
    for (const auto& name : selected_names)
        if (!name.empty()) arguments += L" --entry " + Quoted(name);
    if (!focus_name.empty()) arguments += L" --focus " + Quoted(focus_name);
    const auto result = ShellExecuteW(nullptr, L"open", exe, arguments.c_str(),
                                      nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

namespace {

BOOL CALLBACK CollectPulseWindows(HWND hwnd, LPARAM param) {
    auto* found = reinterpret_cast<std::vector<HWND>*>(param);
    wchar_t name[64]{};
    if (GetClassNameW(hwnd, name, ARRAYSIZE(name)) == 0) return TRUE;
    if (_wcsicmp(name, SingleInstanceCoordinator::WindowClassName()) != 0) return TRUE;
    found->push_back(hwnd);
    return TRUE;
}

} // namespace

std::vector<HWND> OtherPulseWindows(HWND exclude) {
    std::vector<HWND> windows;
    EnumWindows(CollectPulseWindows, reinterpret_cast<LPARAM>(&windows));
    if (exclude) {
        for (auto it = windows.begin(); it != windows.end();) {
            if (*it == exclude) it = windows.erase(it);
            else ++it;
        }
    }
    return windows;
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

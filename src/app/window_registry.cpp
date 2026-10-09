// window_registry.cpp - owns every top-level Pulse window and its AppState.
#include "window_registry.h"
#include "app_state.h"

#include <algorithm>
#include <mutex>

namespace pulse {
namespace {

std::mutex g_windows_mutex;
// Creation order, so index 0 is the primary (tray / endpoint / hotkeys) window.
std::vector<HWND> g_windows;
// AppState of every window, same order as g_windows. The primary entry is
// borrowed (wWinMain's stack frame owns it); the rest are owned here.
std::vector<AppState*> g_states;
// True where the matching entry was allocated by OpenPulseWindow.
std::vector<bool> g_owned;
// AppStates of closed windows, freed after the message loop unwinds.
std::vector<AppState*> g_retired;
PulseWindowCreator g_creator;

HWND CreateWindowFor(AppState* state, int show_cmd, bool owned) {
    if (!state) return nullptr;
    PulseWindowCreator creator;
    {
        std::lock_guard lock(g_windows_mutex);
        creator = g_creator;
    }
    if (!creator) {
        if (owned) delete state;
        return nullptr;
    }
    HWND hwnd = creator(state, show_cmd);
    if (!hwnd) {
        if (owned) delete state;
        return nullptr;
    }
    {
        std::lock_guard lock(g_windows_mutex);
        g_windows.push_back(hwnd);
        g_states.push_back(state);
        g_owned.push_back(owned);
    }
    return hwnd;
}

} // namespace

void SetPulseWindowCreator(PulseWindowCreator creator) {
    std::lock_guard lock(g_windows_mutex);
    g_creator = std::move(creator);
}

HWND CreatePrimaryPulseWindow(AppState* state, int show_cmd) {
    // The caller (wWinMain) keeps ownership of `state`.
    return CreateWindowFor(state, show_cmd, /*owned=*/false);
}

HWND OpenPulseWindow(const std::wstring& path) {
    auto* state = new AppState();
    state->primary_window = false;
    state->secondary_window = true;
    state->open_path = path;
    return CreateWindowFor(state, SW_SHOWNORMAL, /*owned=*/true);
}

HWND PrimaryPulseWindow() {
    std::lock_guard lock(g_windows_mutex);
    return g_windows.empty() ? nullptr : g_windows.front();
}

AppState* PrimaryPulseState() {
    std::lock_guard lock(g_windows_mutex);
    return g_states.empty() ? nullptr : g_states.front();
}

std::vector<HWND> PulseWindows() {
    std::lock_guard lock(g_windows_mutex);
    return g_windows;
}

bool IsPrimaryPulseWindow(HWND hwnd) {
    std::lock_guard lock(g_windows_mutex);
    return !g_windows.empty() && g_windows.front() == hwnd;
}

bool IsLastPulseWindow(HWND hwnd) {
    std::lock_guard lock(g_windows_mutex);
    return g_windows.size() <= 1 && !g_windows.empty() && g_windows.front() == hwnd;
}

HWND AnyOtherPulseWindow(HWND hwnd) {
    std::lock_guard lock(g_windows_mutex);
    for (HWND other : g_windows) {
        if (other != hwnd && IsWindow(other)) return other;
    }
    return nullptr;
}

void ElectPrimaryPulseWindow(HWND hwnd) {
    std::lock_guard lock(g_windows_mutex);
    if (!hwnd || !IsWindow(hwnd)) return;
    if (!g_windows.empty() && g_windows.front() == hwnd) return;
    auto it = std::find(g_windows.begin(), g_windows.end(), hwnd);
    if (it == g_windows.end()) return;
    const size_t idx = static_cast<size_t>(it - g_windows.begin());
    std::rotate(g_windows.begin(), g_windows.begin() + idx, g_windows.begin() + idx + 1);
    std::rotate(g_states.begin(), g_states.begin() + idx, g_states.begin() + idx + 1);
    std::rotate(g_owned.begin(), g_owned.begin() + idx, g_owned.begin() + idx + 1);
}

bool ReleasePulseWindow(HWND hwnd) {
    AppState* state = nullptr;
    bool owned = false;
    bool last = false;
    {
        std::lock_guard lock(g_windows_mutex);
        const auto it = std::find(g_windows.begin(), g_windows.end(), hwnd);
        if (it != g_windows.end()) {
            const size_t idx = static_cast<size_t>(it - g_windows.begin());
            state = g_states[idx];
            owned = g_owned[idx];
            g_windows.erase(g_windows.begin() + idx);
            g_states.erase(g_states.begin() + idx);
            g_owned.erase(g_owned.begin() + idx);
        } else {
            state = GetAppState(hwnd);
        }
        last = g_windows.empty();
    }
    // WM_DESTROY still holds AppState on its stack: retire it instead of
    // deleting it here. Borrowed states belong to wWinMain and are never freed.
    if (state && owned) {
        std::lock_guard lock(g_windows_mutex);
        g_retired.push_back(state);
    }
    return last;
}

void ReclaimRetiredPulseStates() {
    std::vector<AppState*> retired;
    {
        std::lock_guard lock(g_windows_mutex);
        retired.swap(g_retired);
    }
    for (AppState* state : retired) delete state;
}

} // namespace pulse

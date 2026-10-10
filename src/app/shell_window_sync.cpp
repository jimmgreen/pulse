// shell_window_sync.cpp — see shell_window_sync.h.
#include "shell_window_sync.h"

#include "app_input.h"
#include "app_navigation.h"
#include "app_runtime.h"
#include "app_state.h"
#include "../common/path_utils.h"

#include <shlobj.h>
#include <algorithm>

namespace pulse {
namespace {

// Shot and test runs stay out of the real shell. Selftest builds can opt an
// isolated instance in (`variable` set) for real-shell tests; it never
// touches the user's Pulse, but it does take part in the shell.
bool ShellIntegrationAllowed(const AppState& s, const wchar_t* variable) {
    if (s.shot.active || s.menushot) return false;
    if (!s.isolatedTest) return true;
#ifdef PULSE_WITH_SELFTEST
    return GetEnvironmentVariableW(variable, nullptr, 0) > 0;
#else
    (void)variable;
    return false;
#endif
}

bool ShellWindowsEnabled(const AppState& s) {
    if (s.isolatedTest) return ShellIntegrationAllowed(s, L"PULSE_TEST_SHELL_WINDOWS");
    return ShellIntegrationAllowed(s, nullptr) && s.appPrefs.integration_enabled && s.appPrefs.open_folders_in_pulse;
}

bool ExplorerTakeoverEnabled(const AppState& s) {
    if (s.isolatedTest) return ShellIntegrationAllowed(s, L"PULSE_TEST_EXPLORER_TAKEOVER");
    return ShellIntegrationAllowed(s, nullptr) && s.appPrefs.integration_enabled && s.appPrefs.take_over_explorer_windows;
}

void SyncExplorerTakeover(AppState& s) {
    const bool enabled = ExplorerTakeoverEnabled(s);
    if (enabled && !s.explorer_takeover) {
        s.explorer_takeover = std::make_unique<app::ExplorerWindowTakeover>(s.hwnd, WM_EXPLORER_TAKEOVER);
    } else if (!enabled && s.explorer_takeover) {
        s.explorer_takeover->Stop();
        s.explorer_takeover.reset();
    }
}

// Explorer held the foreground; the takeover request arrives while it still
// does, so share its input state long enough to take the foreground over.
void BringToFront(HWND hwnd) {
    const HWND foreground = GetForegroundWindow();
    if (foreground == hwnd) return;
    const DWORD self = GetCurrentThreadId();
    const DWORD other = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    const bool attached = other && other != self && AttachThreadInput(self, other, TRUE);
    SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);
    if (attached) AttachThreadInput(self, other, FALSE);
}

uint64_t PaneKey(const app::Pane& pane) { return reinterpret_cast<uint64_t>(&pane); }

bool SameFolder(const std::wstring& a, const std::wstring& b) {
    return path::EqualInsensitive(path::StripExtendedPathPrefix(fs::NormalizePath(a)),
                                  path::StripExtendedPathPrefix(fs::NormalizePath(b)));
}

// Adds `name` to the selection instead of replacing it: the shell sends one
// SelectItem per item when a program selects several files.
void AddNameToSelection(AppState& s, app::Tab& tab, const std::wstring& name) {
    tab.pending_selected_names.push_back(name);
    tab.pending_selected_name = name;
    tab.pending_selection_revision = tab.selection_revision;
    tab.pending_ensure_selection_visible = true;
    if (!tab.snapshot) return;
    const auto& entries = *tab.snapshot;
    for (size_t i = 0; i < entries.size(); ++i) {
        const int index = static_cast<int>(i);
        if (_wcsicmp(entries[i].name.c_str(), name.c_str()) != 0 || !tab.EntryVisible(index)) continue;
        if (!tab.IsSelected(index)) tab.ToggleSelect(index);
        tab.pending_selection_revision = tab.selection_revision;
        EnsureRowVisible(s, tab, index);
        break;
    }
}

} // namespace

void SyncShellWindows(AppState& s) {
    SyncExplorerTakeover(s);
    std::vector<app::ShellWindowEntry> wanted;
    if (ShellWindowsEnabled(s)) {
        ForEachPane(s, [&](app::Pane& pane) {
            const app::Tab* tab = pane.ActiveTab();
            if (!tab) return;
            // The fs layer's \\?\ paths mean nothing to the shell.
            std::wstring folder = path::StripExtendedPathPrefix(tab->current_path);
            if (app::IsShellWindowPath(folder)) wanted.push_back({PaneKey(pane), std::move(folder)});
        });
    }
    if (wanted == s.shell_windows_published) {
        // Published is delivery deduplication, not acknowledgement. The STA
        // retries COM failures; this only recovers a failed thread start.
        if (s.shell_windows) s.shell_windows->EnsureRunning();
        return;
    }
    app::TraceShellWindows(L"sync enabled=%d wanted=%zu first=%s", ShellWindowsEnabled(s) ? 1 : 0,
                           wanted.size(), wanted.empty() ? L"" : wanted.front().path.c_str());
    if (!s.shell_windows) {
        if (wanted.empty()) return;
        s.shell_windows = std::make_unique<app::ShellWindowRegistry>(s.hwnd, WM_SHELL_SELECT);
    }
    s.shell_windows->Publish(wanted);
    s.shell_windows_published = std::move(wanted);
}

void StopShellWindows(AppState& s) {
    if (s.explorer_takeover) s.explorer_takeover->Stop();
    s.explorer_takeover.reset();
    // Stop synchronizes with posting; release requests which the destroyed
    // target window will no longer dispatch.
    MSG pending{};
    while (PeekMessageW(&pending, s.hwnd, WM_EXPLORER_TAKEOVER, WM_EXPLORER_TAKEOVER, PM_REMOVE)) {
        std::unique_ptr<app::ExplorerTakeoverRequest> request(
            reinterpret_cast<app::ExplorerTakeoverRequest*>(pending.lParam));
        if (request && request->handoff) request->handoff->Cancel();
    }
    if (s.shell_windows) s.shell_windows->Stop();
    s.shell_windows.reset();
    s.shell_windows_published.clear();
}

void HandleShellSelect(AppState& s, const app::ShellSelectRequest& request) {
    size_t layout_index = 0;
    app::Pane* target = nullptr;
    for (size_t i = 0; i < s.window_tabs.items.size() && !target; ++i) {
        if (!s.window_tabs.items[i]) continue;
        for (auto& pane : s.window_tabs.items[i]->panes) {
            if (pane && PaneKey(*pane) == request.key) {
                layout_index = i;
                target = pane.get();
                break;
            }
        }
    }
    app::TraceShellWindows(L"select request key=%llx path=%s flags=0x%x found=%d",
                           static_cast<unsigned long long>(request.key), request.path.c_str(), request.flags,
                           target ? 1 : 0);
    if (!target) return;   // pane closed after the shell looked it up

    s.tray_controller.RestoreWindow();
    if (layout_index != s.window_tabs.active) SwitchTab(s, layout_index);
    FocusPane(s, target);
    app::Tab* tab = ActiveTab(s);
    if (!tab) return;

    std::wstring folder;
    std::wstring leaf;
    // This PC (drives) or a share root: showing the pane is all there is.
    if (!app::SplitShellItemPath(request.path, folder, leaf) || tab->current_path.empty()) {
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    if (!SameFolder(folder, tab->current_path)) {
        NavigateTo(s, folder);
        tab = ActiveTab(s);
        if (!tab) return;
    }
    // SVSI_EDIT (rename) still only selects: Pulse never starts a rename
    // on another program's behalf.
    if (request.flags & SVSI_DESELECTOTHERS) SelectNameInTab(s, *tab, leaf);
    else AddNameToSelection(s, *tab, leaf);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

// A /select that applied in the hidden source after its folder was handed
// over: select those names in the tab opened for it, if it still shows it.
static void SelectTakenOverNames(AppState& s, const app::ExplorerTakeoverRequest& request) {
    if (!ExplorerTakeoverEnabled(s) || request.names.empty()) return;
    app::Tab* tab = ActiveTab(s);
    if (!tab || !SameFolder(tab->current_path, request.folder)) return;
    app::TraceShellWindows(L"takeover late selection folder=[%s] names=%zu", request.folder.c_str(),
                           request.names.size());
    SelectNameInTab(s, *tab, request.names.front());
    for (size_t i = 1; i < request.names.size(); ++i) AddNameToSelection(s, *tab, request.names[i]);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void HandleExplorerTakeover(AppState& s, const app::ExplorerTakeoverRequest& request) {
    if (request.select_only) {
        SelectTakenOverNames(s, request);
        return;
    }
    if (!request.handoff) return;
    if (!ExplorerTakeoverEnabled(s) || GetTickCount64() >= request.handoff->deadline_tick) {
        request.handoff->Cancel();
        return;
    }
    if (request.handoff->state != app::HandoffState::Pending) return;
    if (!request.handoff->Receive(GetTickCount64())) return;
    app::TraceShellWindows(L"takeover request folder=[%s] names=%zu", request.folder.c_str(), request.names.size());
    // Pulse may be closed to the tray or minimized; Explorer's window is about
    // to close, so the folder must open where it can be seen. Restore first:
    // a fresh start from the tray resets the tabs before showing the window.
    s.tray_controller.RestoreWindow();
    // This PC travels as its parsing name, which OpenFolderInNewTab knows.
    OpenFolderInNewTab(s, request.folder.empty() ? std::wstring(L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}")
                                                 : request.folder);
    BringToFront(s.hwnd);
    app::Tab* tab = ActiveTab(s);
    if (!tab || !SameFolder(tab->current_path, request.folder)) {
        request.handoff->Cancel();
        return;
    }
    // Require a fresh successful enumeration even when an existing tab or an
    // offline cache was activated. Capture the generation after queuing it.
    tab->explorer_handoff.reset();
    RefreshPath(s, tab->current_path);
    auto lease = std::make_unique<app::ExplorerNavigationLease>();
    lease->handoff = request.handoff;
    lease->path = tab->current_path;
    lease->generation = tab->pending_generation;
    lease->names = request.names;
    if (!lease->generation) return;
    tab->explorer_handoff = std::move(lease);
    tab->pending_selected_names = request.names;
    tab->pending_selection_revision = tab->selection_revision;
    tab->pending_selected_name = request.names.empty() ? L"" : request.names.front();
    tab->pending_ensure_selection_visible = !request.names.empty();
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void CompleteExplorerNavigation(app::Tab& tab, uint64_t generation, bool success) {
    auto* lease = tab.explorer_handoff.get();
    if (!lease || lease->generation != generation) return;
    if (!success || tab.current_path != lease->path || !tab.snapshot || tab.net_readonly) {
        tab.explorer_handoff.reset();
        return;
    }
    std::vector<std::wstring> selected;
    for (int index : tab.SelectedIndices()) {
        if (index >= 0 && static_cast<size_t>(index) < tab.EntryCount())
            selected.push_back(tab.EntryAt(index).name);
    }
    // Empty Explorer selections must remain empty, instead of the normal
    // first-row selection used when a folder loads.
    if (lease->names.empty()) { tab.ClearSelection(); selected.clear(); }
    lease->Complete(tab.current_path, generation, true, std::move(selected), GetTickCount64());
}

} // namespace pulse

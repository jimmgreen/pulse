// shell_window_sync.cpp — see shell_window_sync.h.
#include "shell_window_sync.h"
#include "explorer_takeover_probe.h"

#include "app_input.h"
#include "app_navigation.h"
#include "app_runtime.h"
#include "app_state.h"
#include "open_routing.h"
#include "tray_reveal.h"
#include "window_registry.h"
#include "../common/path_utils.h"
#include "../ui/ui_view_model.h"

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
    if (ExplorerTakeoverProbeRequested(s)) return ExplorerTakeoverProbeScope(s) != nullptr;
    if (s.isolatedTest) return ShellIntegrationAllowed(s, L"PULSE_TEST_EXPLORER_TAKEOVER");
    // 兼容打开方式 relies on the takeover: Explorer opens the folder, Pulse takes the window.
    return ShellIntegrationAllowed(s, nullptr) && s.appPrefs.integration_enabled &&
           (s.appPrefs.take_over_explorer_windows || s.appPrefs.integration_folders_compat);
}

void SyncExplorerTakeover(AppState& s) {
    const bool enabled = ExplorerTakeoverEnabled(s);
    if (enabled && !s.explorer_takeover) {
        // Requests reach the most recent window through the host.
        s.explorer_takeover = std::make_unique<app::ExplorerWindowTakeover>(s.single_instance.CreateExplorerSubmissionPort(ServiceWindow(s), WM_OPEN_EXTERNAL), ExplorerTakeoverProbeScope(s));
    } else if (!enabled && s.explorer_takeover) {
        s.explorer_takeover->Stop();
        s.explorer_takeover.reset();
    }
    if (s.explorer_takeover) s.explorer_takeover->SetTakeThisPc(s.isolatedTest || s.appPrefs.integration_this_pc);
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

// A pane in a given window. The window is mixed in (M4b) so that a pane
// moved to another window becomes a new shell entry with that window's HWND.
uint64_t PaneKey(const app::Pane& pane, HWND window) {
    return reinterpret_cast<uint64_t>(&pane) ^ (reinterpret_cast<uint64_t>(window) * 0x9E3779B97F4A7C15ull);
}

bool SameFolder(const std::wstring& a, const std::wstring& b) {
    return path::EqualInsensitive(path::StripExtendedPathPrefix(fs::NormalizePath(a)),
                                  path::StripExtendedPathPrefix(fs::NormalizePath(b)));
}

} // namespace

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

void SyncShellWindows(AppState& s) {
    SyncExplorerTakeover(s);
    std::vector<app::ShellWindowEntry> wanted;
    if (ShellWindowsEnabled(s)) {
        // Every window's panes (M4b), each registered with its own window.
        app::ForEachWindowSharingCore(s, [&](AppState& window) {
            ForEachPane(window, [&](app::Pane& pane) {
                const app::Tab* tab = pane.ActiveTab();
                if (!tab) return;
                // The fs layer's \\?\ paths mean nothing to the shell.
                std::wstring folder = path::StripExtendedPathPrefix(tab->current_path);
                if (app::IsShellWindowPath(folder))
                    wanted.push_back({PaneKey(pane, window.hwnd), std::move(folder), window.hwnd});
            });
        });
        // Windows are visited most recent first; a stable order keeps
        // switching windows from republishing an unchanged set.
        std::stable_sort(wanted.begin(), wanted.end(), [](const auto& a, const auto& b) {
            return reinterpret_cast<uintptr_t>(a.window) < reinterpret_cast<uintptr_t>(b.window);
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
        // Entries name their own windows; the host outlives every one of them.
        s.shell_windows = std::make_unique<app::ShellWindowRegistry>(ServiceWindow(s), WM_SHELL_SELECT);
    }
    s.shell_windows->Publish(wanted);
    s.shell_windows_published = std::move(wanted);
}

void StopShellWindows(AppState& s) {
    if (s.explorer_takeover) s.explorer_takeover->Stop();
    s.explorer_takeover.reset();
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
            if (pane && PaneKey(*pane, s.hwnd) == request.key) {
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

    RevealWindow(s);
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

ExternalOpenResult HandleExplorerTakeover(AppState& mru, const app::ExplorerOpenContext& request) {
    if (!request.handoff || !ExplorerTakeoverEnabled(mru) ||
        GetTickCount64() >= request.handoff->deadline_tick ||
        request.handoff->state != app::HandoffState::Pending) return {};
    bool available = false;
    mru.core.windows.ForEach([&](HWND, const AppState& window) {
        if (!ExternalOpenWindowBusy(window)) available = true;
    });
    if (!available) return {ExternalOpenStatus::Deferred, nullptr};
    app::TraceShellWindows(L"takeover request folder=[%s] names=%zu", request.folder.c_str(), request.names.size());
    // Explorer's window is about to close, so the folder must open where it
    // can be seen: plan §6 picks the window (M7a); a window minimized or in
    // the tray is restored (a fresh start from the tray included) when the
    // folder goes there. This PC travels as its parsing name, which
    // OpenExternalFolder knows.
    ExplorerTakeoverProbeDelivery(mru, request.handoff, "open-before");
    const auto opened = OpenExternalFolder(mru, request.folder.empty()
                                              ? std::wstring(L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}")
                                              : request.folder);
    ExplorerTakeoverProbeDelivery(mru, request.handoff, "open-after");
    if (opened.status != ExternalOpenStatus::Dispatched || !opened.target) {
        return opened;
    }
    AppState& s = *opened.target;
    ExplorerTakeoverProbeNavigation(s, ActiveTab(s), "bringfront-before");
    BringToFront(s.hwnd);
    ExplorerTakeoverProbeNavigation(s, ActiveTab(s), "bringfront-after");
    app::Tab* tab = ActiveTab(s);
    if (!tab || !SameFolder(tab->current_path, request.folder)) {
        return {};
    }
    // Require a fresh successful enumeration even when an existing tab or an
    // offline cache was activated. Capture the generation after queuing it.
    tab->explorer_handoff.reset();
    ExplorerTakeoverProbeNavigation(s, tab, "refresh-before");
    RefreshPath(s, tab->current_path);
    ExplorerTakeoverProbeNavigation(s, tab, "refresh-after");
    auto lease = std::make_unique<app::ExplorerNavigationLease>();
    lease->handoff = request.handoff;
    lease->path = tab->current_path;
    lease->generation = tab->pending_generation;
    lease->names = request.names;
    lease->initial_selection_revision = tab->selection_revision;
    if (!lease->generation) return {};
    tab->explorer_handoff = std::move(lease);
    tab->pending_selected_names = request.names;
    tab->pending_selection_revision = tab->selection_revision;
    tab->pending_selected_name = request.names.empty() ? L"" : request.names.front();
    tab->pending_ensure_selection_visible = !request.names.empty();
    ExplorerTakeoverProbeNavigation(s, tab, "lease-attached");
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return opened;
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
    lease->RecordListing(tab.current_path, generation, true, std::move(selected), GetTickCount64());
}

namespace {
bool ExplorerSelection(AppState& state, app::Tab& tab, std::vector<std::wstring>& selected) {
    for (int index : tab.SelectedIndices()) {
        if (index < 0 || static_cast<size_t>(index) >= tab.EntryCount() || !tab.EntryVisible(index)) {
            if (tab.explorer_handoff) {
                ExplorerTakeoverProbeNavigation(state, &tab, "selection-not-visible", static_cast<unsigned>(index));
                tab.explorer_handoff->handoff->Cancel();
            }
            return false;
        }
        selected.push_back(tab.EntryAt(index).name);
    }
    return true;
}
void TraceFrameOnce(AppState& s, app::Tab* tab, unsigned bit, const char* reason, unsigned detail) {
    if (!tab || !tab->explorer_handoff) return;
    static std::weak_ptr<app::ExplorerHandoff> last;
    static unsigned seen = 0;
    const auto handoff = tab->explorer_handoff->handoff;
    if (last.lock() != handoff) { last = handoff; seen = 0; }
    if (seen & bit) return;
    seen |= bit;
    ExplorerTakeoverProbeNavigation(s, tab, reason, detail);
}
bool PresentedPane(AppState& s, app::Tab* tab) {
    if (!IsWindowVisible(s.hwnd) || IsIconic(s.hwnd)) return false;
    const auto* layout = s.window_tabs.Active();
    if (!layout) return false;
    for (auto* pane : layout->VisiblePanes()) if (pane && pane->ActiveTab() == tab) return true;
    return false;
}
}
void TickExplorerPresentation(AppState& s) {
    const auto now = GetTickCount64();
    ForEachPane(s, [&](app::Pane& pane) {
        auto* tab = pane.ActiveTab();
        if (tab && tab->explorer_handoff) {
            std::vector<std::wstring> selected;
            if (!ExplorerSelection(s, *tab, selected)) return;
            const auto before = tab->explorer_handoff->handoff->state.load();
            const bool visible = PresentedPane(s, tab);
            tab->explorer_handoff->Presented(tab->current_path, tab->pending_generation, tab->applied_generation,
                selected, visible, false, now);
            if (before != tab->explorer_handoff->handoff->state.load()) {
                const auto& lease = *tab->explorer_handoff;
                const char* reason = now >= lease.handoff->deadline_tick ? "tick-expired" : !visible ? "tick-not-visible" :
                    tab->current_path != lease.path ? "tick-path-changed" : tab->pending_generation && tab->pending_generation != lease.generation ?
                    "tick-new-pending" : !tab->pending_generation && tab->applied_generation != lease.generation ? "tick-new-applied" : "tick-selection-changed";
                ExplorerTakeoverProbeNavigation(s, tab, reason);
            }
        }
    });
}
ExplorerFrame CaptureExplorerFrame(AppState& s, const ui::WindowViewModel& vm) {
    ExplorerFrame frame;
    if (vm.settings_open) {
        ForEachPane(s, [&](app::Pane& pane) { TraceFrameOnce(s, pane.ActiveTab(), 128u, "capture-settings-overlay", 1); });
        return frame;
    }
    const auto* layout = s.window_tabs.Active();
    if (!layout) return frame;
    const auto panes = layout->VisiblePanes();
    if (panes.size() != vm.pane_slots.size()) {
        for (auto* pane : panes) if (pane) TraceFrameOnce(s, pane->ActiveTab(), 128u, "capture-pane-count-mismatch",
            (static_cast<unsigned>(panes.size()) << 16) | static_cast<unsigned>(vm.pane_slots.size()));
        return frame;
    }
    for (size_t i = 0; i < panes.size(); ++i) {
        const auto& slot = vm.pane_slots[i];
        const auto& rect = slot.rect;
        auto* tab = panes[i] ? panes[i]->ActiveTab() : nullptr;
        if (tab && tab->explorer_handoff) {
            const unsigned gates = (tab->explorer_handoff->listing_complete ? 1u : 0u) | (!tab->loading ? 2u : 0u) |
                (rect.right > rect.left && rect.bottom > rect.top ? 4u : 0u) | (tab->snapshot ? 8u : 0u) |
                (slot.pane.snapshot == tab->snapshot ? 16u : 0u);
            TraceFrameOnce(s, tab, 1u, "capture-first-gates", gates);
            if (tab->explorer_handoff->listing_complete) TraceFrameOnce(s, tab, 2u, "capture-listed-gates", gates);
        }
        if (tab && tab->explorer_handoff && tab->explorer_handoff->listing_complete &&
            tab->explorer_handoff->handoff->state == app::HandoffState::Pending &&
            !tab->loading && rect.right > rect.left && rect.bottom > rect.top &&
            tab->snapshot && slot.pane.snapshot == tab->snapshot) {
            // Both tree walks are first-then-second; animation only changes
            // rectangles. Also verify the immutable listing and rendered row
            // mapping, rather than trusting ordinal correspondence alone.
            bool selection_visible = true;
            for (int index : tab->SelectedIndices()) {
                const int row = slot.pane.ViewIndex(index);
                if (!tab->EntryVisible(index) || row < 0 || static_cast<size_t>(row) >= slot.pane.EntryCount() ||
                    slot.pane.SourceIndex(row) != index || !slot.pane.IsRowSelected(index)) {
                    selection_visible = false; break;
                }
            }
            TraceFrameOnce(s, tab, selection_visible ? 4u : 8u, "capture-selection", selection_visible ? 1u : 0u);
            if (selection_visible) frame.push_back({tab, tab->applied_generation, tab->selection_revision});
        }
    }
    return frame;
}
void CompleteExplorerFrame(AppState& s, const ExplorerFrame& frame, bool presented) {
    TickExplorerPresentation(s);
    ForEachPane(s, [&](app::Pane& pane) {
        auto* tab = pane.ActiveTab();
        if (!tab || !tab->explorer_handoff) return;
        if (!presented) {
            TraceFrameOnce(s, tab, 16u, "frame-not-presented", static_cast<unsigned>(frame.size()));
            tab->explorer_handoff->handoff->Cancel(); return;
        }
        const auto found = std::find_if(frame.begin(), frame.end(), [&](const auto& item) {
            return item.tab == tab && item.generation == tab->applied_generation && item.selection_revision == tab->selection_revision;
        });
        if (found == frame.end()) { TraceFrameOnce(s, tab, 32u, "frame-no-matching-evidence", static_cast<unsigned>(frame.size())); return; }
        std::vector<std::wstring> selected;
        if (!ExplorerSelection(s, *tab, selected)) return;
        const bool acknowledged = tab->explorer_handoff->Presented(tab->current_path, tab->pending_generation, tab->applied_generation,
            selected, PresentedPane(s, tab) && !tab->loading && tab->snapshot && !tab->net_readonly, presented, GetTickCount64());
        TraceFrameOnce(s, tab, 64u, "frame-ack-result", acknowledged ? 1u : 0u);
        ExplorerTakeoverProbePresented(s, tab, acknowledged);
    });
}

} // namespace pulse
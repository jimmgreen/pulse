// shell_window_plan.h — Which Pulse panes the shell sees as folder windows
// (B站 #1 phase 2a), and what has to change when panes navigate or close.
//
// With Pulse as the default file manager, "open file location" in other
// programs (SHOpenFolderAndSelectItems) looks up IShellWindows for a window
// showing the folder and asks it to select the item. ShellWindowRegistry
// registers each pane there; this header holds the pure planning part.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace pulse::app {

struct ShellWindowEntry {
    uint64_t key = 0;      // pane identity
    std::wstring path;     // folder shown; empty = This PC
    bool operator==(const ShellWindowEntry&) const = default;
};

enum class ShellWindowActionKind { Register, Navigate, Revoke };

struct ShellWindowAction {
    ShellWindowActionKind kind = ShellWindowActionKind::Register;
    uint64_t key = 0;
    std::wstring path;
    bool operator==(const ShellWindowAction&) const = default;
};

// A drive or UNC folder, or This PC (empty). Pulse's own views (pulse:…
// search, settings, recycle bin) have no shell folder to report. Expects the
// plain form: strip \\?\ first (path::StripExtendedPathPrefix).
bool IsShellWindowPath(const std::wstring& path);

// Revokes first (keys that are gone), then registrations and navigations in
// `wanted` order. Paths compare case-insensitively.
std::vector<ShellWindowAction> PlanShellWindowChanges(
    const std::vector<ShellWindowEntry>& current, const std::vector<ShellWindowEntry>& wanted);

// Posted to the UI thread when the shell asks a pane to select an item.
struct ShellSelectRequest {
    uint64_t key = 0;
    std::wstring path;     // absolute parsing name of the item
    unsigned flags = 0;    // SVSIF
};

// Splits an item path into its folder and leaf ("C:\a\b.txt" -> "C:\a",
// "b.txt"; "C:\a" -> "C:\", "a"). False for a drive root or anything without
// a parent folder.
bool SplitShellItemPath(const std::wstring& path, std::wstring& folder, std::wstring& leaf);

// B站 #1 phase 2b: a File Explorer window opened behind Pulse's back
// (explorer.exe /select,… run directly). What one poll of it found:
struct ExplorerWindowProbe {
    unsigned age_ms = 0;          // since the window registered
    unsigned view_age_ms = 0;     // since its view first reported a folder
    bool view_ready = false;      // its folder view exists and reports a folder
    bool supported = false;       // a file system folder or This PC
    size_t selected = 0;          // items selected in it
};

enum class ExplorerTakeoverStep { Wait, Take, Leave };

constexpr unsigned kExplorerViewTimeoutMs = 4000;     // no view by then: leave it
// After the view is ready: a selection arriving this soon goes along with the
// request; a later one follows it (kExplorerLateSelectionMs).
constexpr unsigned kExplorerSelectionGraceMs = 150;
// The hidden source stays open this long after its view is ready, because
// /select often applies 590-640 ms after it (Windows 11 23H2).
constexpr unsigned kExplorerLateSelectionMs = 800;
// A selection that changes this soon after the view is ready comes from the
// program that opened the window (/select, SHOpenFolderAndSelectItems), not
// from the user, even if the window could not be hidden in time.
constexpr unsigned kExplorerProgrammaticSelectionMs = 1500;

// Wait for the view, leave virtual locations (Control Panel, Home, network…)
// alone, and take the window as soon as its view is ready — Pulse shows the
// folder at once while the source stays hidden.
ExplorerTakeoverStep DecideExplorerTakeover(const ExplorerWindowProbe& probe);

// The source window once Pulse has acknowledged the folder.
struct SourceCloseProbe {
    bool identity_kept = false;     // same window, process and folder; view still readable
    bool single_tab = false;        // exactly one tab and one shell view: nothing else to lose
    bool hidden = false;            // invisible since it appeared: the user cannot have used it
    bool selection_changed = false; // differs from what was sent to Pulse
    bool sent_selection = false;    // the request carried names
    unsigned view_age_ms = 0;
};

enum class SourceCloseStep { Wait, Close, CloseAndSelect, Abort };

// Never closes a window with other tabs or one the user may have touched. A
// hidden source waits a moment for a late /select, which then follows the
// request to Pulse.
SourceCloseStep DecideSourceClose(const SourceCloseProbe& probe);

} // namespace pulse::app

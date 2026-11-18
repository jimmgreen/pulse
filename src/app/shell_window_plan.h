// shell_window_plan.h — Which Pulse panes the shell sees as folder windows
// (B站 #1 phase 2a), and what has to change when panes navigate or close.
//
// With Pulse as the default file manager, "open file location" in other
// programs (SHOpenFolderAndSelectItems) looks up IShellWindows for a window
// showing the folder and asks it to select the item. ShellWindowRegistry
// registers each pane there; this header holds the pure planning part.
#pragma once
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace pulse::app {

struct ShellWindowEntry {
    uint64_t key = 0;      // pane identity
    std::wstring path;     // folder shown; empty = This PC
    // The Pulse window showing the pane (multi-window M4b): the shell
    // activates it and it receives the select message. Null = the
    // registry's own window. Fixed per key: SyncShellWindows mixes the
    // window into the key, so a pane that moves is revoked and re-registered.
    HWND window = nullptr;
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
    bool selection_read = false;
    unsigned stable_age_ms = 0;   // same readable folder + selection in consecutive observations
};

// IShellWindows::Item explicitly returns S_FALSE/null for a missing entry.
// https://learn.microsoft.com/en-us/windows/win32/api/exdisp/nf-exdisp-ishellwindows-item
// Any inconsistent pair or other HRESULT leaves enumeration incomplete.
enum class ShellWindowItemResult { Found, Absent, Invalid };
constexpr ShellWindowItemResult ClassifyShellWindowItem(HRESULT result, bool has_dispatch) noexcept {
    if (result == S_OK && has_dispatch) return ShellWindowItemResult::Found;
    if (result == S_FALSE && !has_dispatch) return ShellWindowItemResult::Absent;
    return ShellWindowItemResult::Invalid;
}

enum class ExplorerTakeoverStep { Wait, Take, Leave };

constexpr unsigned kExplorerViewTimeoutMs = 4000;     // admission observation bound
// Nonempty selections must remain unchanged across a short observation
// window; empty selections use one short grace window for late selection.
constexpr unsigned kExplorerSelectionGraceMs = 100;
constexpr unsigned kExplorerLateSelectionMs = 100;
// Changes within this interval may be a late programmatic selection; they
// still require a new presentation acknowledgement before any source close.
constexpr unsigned kExplorerProgrammaticSelectionMs = 1500;

// Pure observation state: unreadable results, folder changes and selection
// changes reset stability. Item enumeration order is not a selection change.
class ExplorerSelectionStability {
public:
    unsigned Observe(unsigned age_ms, bool readable, const std::wstring& folder,
                     const std::vector<std::wstring>& names);
private:
    bool valid_ = false;
    unsigned since_ms_ = 0;
    std::wstring folder_;
    std::vector<std::wstring> names_;
};

// Wait for stable contents before submitting, bounded by registration age.
ExplorerTakeoverStep DecideExplorerTakeover(const ExplorerWindowProbe& probe);

// The source window once Pulse has acknowledged the folder.
struct SourceCloseProbe {
    bool identity_kept = false;     // same window, process and folder; view still readable
    bool single_tab = false;        // exactly one tab and one shell view: nothing else to lose
    bool selection_changed = false; // differs from what was sent to Pulse
    bool sent_selection = false;    // the request carried names
    unsigned view_age_ms = 0;
};

enum class SourceCloseStep { Wait, Close, SelectAndKeep, Abort };

// Never closes a changed source. SelectAndKeep permits an optional follow-up,
// but cannot authorize Quit without a new presentation acknowledgement.
SourceCloseStep DecideSourceClose(const SourceCloseProbe& probe);

} // namespace pulse::app

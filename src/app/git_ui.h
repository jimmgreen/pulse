#pragma once
// In-app Git, the window side: status requests (throttled, off the UI
// thread), row marks for the list, the branch in the status bar, toolbar pins,
// details panel summary/actions, and the Branch & Sync / Source Control UI.
#include "../ui/fluent_menu.h"
#include "../ui/ui_view_model.h"
#include <windows.h>
#include <string>
#include <vector>

namespace pulse {
struct AppState;
namespace app { struct Tab; }

constexpr int kCmdGitPull = 38100;
constexpr int kCmdGitPush = 38101;
constexpr int kCmdGitRefresh = 38102;
constexpr int kCmdGitSourceControl = 38103;
constexpr int kCmdGitSync = 38104;
constexpr int kCmdGitBranches = 38105;
constexpr int kCmdGitHistory = 38106;

// Marks the rows of a folder inside a repository, and asks git again when
// the listing changed or the last answer is old.
void FillGitPane(AppState& s, const app::Tab& tab, ui::PaneViewModel& pane);
// Appends branch/ahead-behind/change summary to the status bar text.
void AppendGitStatusText(const AppState& s, const app::Tab& tab, std::wstring& text);
// A Git flyout after "Open in Terminal" for a folder in a repository.
void AppendGitMenu(const AppState& s, const app::Tab& tab, std::vector<ui::FluentMenuItem>& items);
// Runs a kCmdGit* command for the repository at `root`; false for any other.
bool HandleGitCommand(AppState& s, int command, const std::wstring& root);
// The sidebar Project row's menu; false when `path` is not that row's repository.
bool ShowGitProjectMenu(AppState& s, const std::wstring& path, POINT screen_pt);
// The details panel's Git section for the selected `dv.path` (async off UI thread).
void FillGitDetails(AppState& s, const app::Tab& tab, ui::DetailsPanelView& dv);
// Details panel quick actions for `item_full_path` inside `root`.
void GitDetailsToggleStage(AppState& s, const std::wstring& root, const std::wstring& item_full_path);
void GitDetailsDiscard(AppState& s, const std::wstring& root, const std::wstring& item_full_path);
void GitDetailsRestoreCommit(AppState& s, const std::wstring& root, const std::wstring& item_full_path, int history_index);
// The status-bar Git chip: shared Branch & Sync popover.
void ShowGitStatusMenu(AppState& s, POINT screen_pt);
// The toolbar's Git group for a folder in a repository (Branches, Sync, Source Control).
void AppendGitToolbarPins(AppState& s, const app::Tab& tab, std::vector<ui::SelectionChipAction>& pins, size_t cap);
// The Source Control / Version Center window (`tab_index`: 0=Changes, 1=History, 2=Conflict).
void OpenGitSourceControl(AppState& s, const std::wstring& root);
void OpenGitSourceControlTab(AppState& s, const std::wstring& root, int tab_index,
                             const std::wstring& focus_full_or_rel_path = {});
// WM_GIT_RESULT: finished status, remote op, branch/stash op, or details summary.
void ReceiveGitResult(AppState& s, LPARAM lparam);

}  // namespace pulse

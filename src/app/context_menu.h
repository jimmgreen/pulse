// context_menu.h — Built-in Fluent context menu verbs (stage 1B-2).
//
// Built-in verbs only; third-party IContextMenu stays in pulse_shell and is
// out of scope for 1B-2 (plan.md §11 pit 6). Item construction is windowless
// so the console self-test can assert the verb list, hit-test and dispatch
// without a GUI.
#pragma once
#include "../ui/fluent_menu.h"
#include "../ui/view_layout.h"
#include "context_menu_prefs.h"
#include "../index/index_engine.h"
#include <string>

namespace pulse::app {

// Command ids returned by FluentMenu::TrackPopup for the built-in verbs.
enum MenuCmd : int {
    CmdNone = 0,
    CmdOpen = 1,
    CmdCut,
    CmdCopy,
    CmdPaste,
    CmdDelete,
    CmdRename,
    CmdProperties,
    CmdOpenTerminal,
    CmdCopyPath,        // Ctrl+Shift+C
    CmdUndo,            // Ctrl+Z
    CmdNewFolder,
    CmdNewTextFile,
    CmdCopyAsPath,      // unused alias guard — keep ids stable
    CmdOpenPath,        // search results: reveal in containing folder
    CmdOpenInNewTab,    // folder: open selection in a new tab
    CmdLayoutSingle = 50,
    CmdLayoutTwoVertical,
    CmdLayoutTwoHorizontal,
    CmdLayoutThree,
    CmdLayoutFourGrid,
    CmdCopyToTarget,
    CmdMoveToTarget,
    CmdPinWorkspace,
    CmdPinNetwork,
    CmdSearchAll,           // palette: open pulse:search: as a folder
    CmdInstallFullIndex,    // palette: UAC-install PulseIndex service
    CmdSettings,            // palette / title-bar gear → settings tab
    CmdSettingsContextMenu, // 管理右键项… → 右键菜单 page
    CmdTags,
    CmdTagBase = 70,        // +0..6  Ctrl+Shift+1..7
    CmdViewBase = 80,       // + ViewModeIndex (8 stable view choices)
    CmdTabColorBase = 90,   // +0..7  tab-group color swatches
    CmdTabGroupNewTab = 98,
    CmdTabGroupUngroup,
    CmdTabGroupClose,
    CmdDetailsPanel,        // view menu: right details panel toggle
    CmdDetailsComputeSize,  // details "更多": compute folder size in place
    CmdDetailsShellMenu,    // details "更多": open the Explorer context menu
    CmdTabNewRight,         // tab menu: new tab to the right
    CmdTabDuplicate,        // tab menu: duplicate this tab
    CmdTabPin,              // tab menu: pin/unpin toggle
    CmdTabClose,            // tab menu: close this tab
    CmdTabAddToNewGroup,    // tab menu: create a group with this tab
    CmdTabRemoveFromGroup,  // tab menu: leave the group (group survives)
    CmdTabCloseOthers,
    CmdTabCloseRight,
    CmdTabJoinGroupBase = 130, // + index into Pane::tab_groups (clear of 98-129)
    CmdEditStarBadge = 170,
    CmdRemoveStarred,
    CmdRemoveRecent,
    CmdRecentBase = 200,
    CmdIndexBase = 1000,
    // Explorer integration (优化.md §7): registry static verbs bound to the
    // current selection (+index into the StaticVerb list) and pulse_shell
    // IContextMenu session items (+host menu id, up to 0x7FFF).
    CmdShellStaticBase = 6000,
    CmdShellComBase = 8000,
};

// One merged Explorer row (registry static verb or pulse_shell COM item),
// already mapped to its final command id. Software-owned submenus keep one
// level: command 0 + non-empty children => flyout header row.
struct ShellMenuEntry {
    int command = 0;
    std::wstring text;
    bool enabled = true;
    std::vector<ShellMenuEntry> children;
    std::wstring verb;      // canonical verb when known (registry / GetCommandString)
    bool from_com = false;  // pulse_shell IContextMenu row (vs registry static)
};

// Context menu for a selected entry: 打开 + icon strip (cut/copy/delete/
// rename) + built-in verbs + undo. Explorer rows are appended afterwards via
// AppendShellSection (they grow the menu downward so open rows never move).
std::vector<ui::FluentMenuItem> BuildItemMenu(bool can_undo, const std::wstring& undo_label,
                                              bool folder = false);

// Appends the merged Explorer section at the very bottom: separator, then one
// row per entry. Software submenus become one-level flyout headers (children
// on FluentMenuItem); other rows stay flat. Drops entries whose text
// duplicates an existing row (case-insensitive). Hard safety cap is 48; the
// user-facing default (12) is applied earlier by ApplyExplorerPrefs.
void AppendShellSection(std::vector<ui::FluentMenuItem>& items,
                        const std::vector<ShellMenuEntry>& entries);

// Factory denylist + per-item overrides + compress-into-flyout + order + cap.
// Host still returns the full list; call this in the UI process.
std::vector<ShellMenuEntry> ApplyExplorerPrefs(const ContextMenuPrefs& prefs,
                                               const std::vector<ShellMenuEntry>& entries);

// Context menu for empty list space (creation / paste / terminal / undo).
std::vector<ui::FluentMenuItem> BuildBackgroundMenu(bool can_paste, bool can_undo,
                                                    const std::wstring& undo_label);

// Toolbar "新建▾" dropdown (reuses the same FluentMenu component).
std::vector<ui::FluentMenuItem> BuildNewMenu();

// Split-button layout presets.
std::vector<ui::FluentMenuItem> BuildSplitMenu(int current_preset);
std::vector<ui::FluentMenuItem> BuildViewMenu(ui::ViewMode current_mode,
                                              bool details_panel = false);

// Ctrl+K / address omnibar: verbs + index hits + recent folders at the end.
struct OmnibarQuery {
    enum class Kind { Mixed, Command, Search, Project };
    Kind kind = Kind::Mixed;
    std::wstring needle;
    wchar_t prefix = 0;
};

OmnibarQuery ParseOmnibarQuery(const std::wstring& query, bool project_only = false);
bool LooksLikeFilesystemPath(const std::wstring& text);

std::vector<ui::FluentMenuItem> BuildCommandPalette(const std::vector<std::wstring>& recent_paths);
std::vector<ui::FluentMenuItem> BuildCommandPalette(const std::wstring& query,
                                                    const std::vector<std::wstring>& recent_paths,
                                                    const std::vector<index::Hit>& hits,
                                                    bool project_only,
                                                    size_t total = 0,
                                                    const std::wstring& current_path = {});

// "base.ext", "base (2).ext", ... — first name not colliding under dir.
std::wstring UniqueChildName(const std::wstring& dir, const std::wstring& base,
                             const std::wstring& ext);

} // namespace pulse::app

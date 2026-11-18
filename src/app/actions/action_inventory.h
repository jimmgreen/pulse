// action_inventory.h - Every action Pulse can do, as one list (全面打通 P0).
//
// The list the action registry (P1) is built from: each action's stable id
// and how much it may do without asking. Every keyboard action and every
// menu command id links to an action here, or says why it has none, so a new
// command cannot slip past the command palette, the assistant and MCP.
//   KeyAction: checked at compile time (action_inventory.cpp).
//   MenuCmd and the Git commands: tools/actions/check_actions.py.
//
// Risk, lowest first; the registry turns it into run / confirm / refuse:
//   Read      reads state only
//   View      changes what Pulse shows (panes, tabs, view, selection); undo
//   Organize  Pulse's own data (tags, favorites, tray, aliases); undo
//   Create    new files only, nothing overwritten; undo
//   Move      moves or renames files: a plan the user accepts once
//   System    changes Windows (startup, default file manager, drives)
//   External  effects Pulse cannot know (third-party menu items, Git, other
//             programs): confirmed every time, never "always allow"
//   Delete    to the Recycle Bin: confirmed every time
//   PermanentDelete  red second confirm; system folders never
//   UserOnly  needs the user (a password): the assistant only points there
// by_target: the call's target can raise the level (nav.open on a program
// is External; settings.set on a setting that changes Windows is System).
#pragma once

#include "../keymap.h"
#include "../context_menu.h"
#include "../git_ui.h"

#include <cstdint>
#include <string_view>

namespace pulse::app::actions {

enum class ActionRisk : uint8_t {
    Read, View, Organize, Create, Move, System, External, Delete, PermanentDelete, UserOnly
};

struct ActionEntry {
    std::string_view id;
    ActionRisk risk;
    bool by_target = false;
};

// A keyboard action or menu command and its action; an empty id names why
// it has none.
struct ActionLink {
    std::string_view id;
    std::string_view why_none = {};
};
struct KeyActionLink {
    KeyAction key;
    ActionLink link;
};
struct MenuCmdLink {
    int command;  // a MenuCmd, a *Base (the whole range) or a kCmdGit*
    ActionLink link;
};

inline constexpr ActionEntry kActions[] = {
    // 导航
    {"nav.open", ActionRisk::View, true},
    {"nav.go", ActionRisk::View},
    {"nav.back", ActionRisk::View},
    {"nav.forward", ActionRisk::View},
    {"nav.up", ActionRisk::View},
    {"nav.refresh", ActionRisk::View},
    {"nav.open_in_new_tab", ActionRisk::View},
    {"nav.reveal", ActionRisk::View},
    {"nav.recent_changes", ActionRisk::View},
    {"nav.recycle_bin", ActionRisk::View},
    // 标签页
    {"tab.new", ActionRisk::View},
    {"tab.close", ActionRisk::View},
    {"tab.close_others", ActionRisk::View},
    {"tab.duplicate", ActionRisk::View},
    {"tab.pin", ActionRisk::View},
    {"tab.rename", ActionRisk::View},
    {"tab.color", ActionRisk::View},
    {"tab.group", ActionRisk::View},
    {"tab.activate", ActionRisk::View},
    {"tab.to_window", ActionRisk::View},
    // 窗口与分栏
    {"window.layout", ActionRisk::View},
    {"pane.focus", ActionRisk::View},
    {"pane.mark_target", ActionRisk::View},
    {"pane.to_window", ActionRisk::View},
    {"window.new", ActionRisk::View},
    {"window.close", ActionRisk::View},
    {"window.reopen", ActionRisk::View},
    {"window.activate", ActionRisk::View},
    {"window.arrange", ActionRisk::View},
    {"window.merge", ActionRisk::View},
    {"window.overview", ActionRisk::View},
    {"window.maximize", ActionRisk::View},
    {"sidebar.toggle", ActionRisk::View},
    {"sidebar.section", ActionRisk::View},
    {"favbar.toggle", ActionRisk::View},
    {"details.toggle", ActionRisk::View},
    {"app.settings_page", ActionRisk::View},
    {"app.shortcut_help", ActionRisk::View},
    {"app.exit", ActionRisk::UserOnly},    // the palette only: the assistant never quits Pulse
    // 视图
    {"view.mode", ActionRisk::View},
    {"view.icon_size", ActionRisk::View},
    {"view.wall", ActionRisk::View},
    {"view.sort", ActionRisk::View},
    {"view.group", ActionRisk::View},
    {"view.folders_first", ActionRisk::View},
    {"view.folder_sizes", ActionRisk::View},
    {"view.columns", ActionRisk::View},
    {"view.apply_all", ActionRisk::Organize},
    {"view.filter", ActionRisk::View},
    {"view.compare", ActionRisk::View},
    // 选择
    {"select.items", ActionRisk::View},
    {"select.all", ActionRisk::View},
    {"select.invert", ActionRisk::View},
    {"select.pattern", ActionRisk::View},
    {"select.clear", ActionRisk::View},
    // 文件操作
    {"edit.undo", ActionRisk::View, true},
    {"file.copy", ActionRisk::Organize},
    {"file.cut", ActionRisk::Organize},
    {"file.copy_path", ActionRisk::Organize},
    {"file.paste", ActionRisk::Move},
    {"file.copy_to", ActionRisk::Move},
    {"file.move_to", ActionRisk::Move},
    {"file.new_folder", ActionRisk::Create},
    {"file.new_file", ActionRisk::Create},
    {"file.rename", ActionRisk::Move},
    {"file.batch_rename", ActionRisk::View},
    {"file.group_into_folder", ActionRisk::Move},
    {"file.dissolve", ActionRisk::Move},
    {"file.organize", ActionRisk::Move},          // AI apps: moves[{item, dest}], one plan dialog
    {"file.find_duplicates", ActionRisk::Read},   // reads files to compare them; opens a 重复文件 tab
    {"file.zip", ActionRisk::Create},
    {"file.recycle", ActionRisk::Delete},
    {"file.delete_permanent", ActionRisk::PermanentDelete},
    {"recycle.restore", ActionRisk::Move},
    {"recycle.empty", ActionRisk::PermanentDelete},
    {"file.properties", ActionRisk::View},
    {"file.context_menu", ActionRisk::View},
    {"file.terminal", ActionRisk::View},
    {"file.compute_size", ActionRisk::Read},
    {"shell.verbs", ActionRisk::Read},
    {"shell.run_verb", ActionRisk::External},
    {"cloud.keep", ActionRisk::System},
    {"cloud.free", ActionRisk::System},
    // 标签 · 收藏 · 别名
    {"tag.toggle", ActionRisk::Organize},
    {"tag.clear", ActionRisk::Organize},
    {"tag.panel", ActionRisk::View},
    {"tag.create", ActionRisk::Organize},
    {"tag.edit", ActionRisk::Organize},
    {"tag.delete", ActionRisk::Organize},
    {"tag.group", ActionRisk::Organize},
    {"smartview.save", ActionRisk::Organize},
    {"fav.add", ActionRisk::Organize},
    {"quick_access.pin", ActionRisk::Organize},
    {"recent.remove", ActionRisk::Organize},
    {"workspace.pin", ActionRisk::Organize},
    {"alias.set", ActionRisk::Organize},
    {"alias.display", ActionRisk::View},
    {"alias.manage", ActionRisk::View},
    {"alias.export", ActionRisk::Create},
    // 暂存托盘
    {"tray.add", ActionRisk::Organize},
    {"tray.remove", ActionRisk::Organize},
    {"tray.find_stale", ActionRisk::View},
    {"tray.send_to", ActionRisk::Move},
    {"tray.zip", ActionRisk::Create},
    {"tray.compare", ActionRisk::Read},
    {"tray.text_diff", ActionRisk::View},
    // 搜索与索引
    {"search.files", ActionRisk::Read},
    {"search.open", ActionRisk::View},
    {"search.advanced", ActionRisk::View},
    {"search.project", ActionRisk::View},
    {"search.global", ActionRisk::View},
    {"index.status", ActionRisk::Read},
    {"index.install_full", ActionRisk::System},
    // 预览
    {"preview.toggle", ActionRisk::View},
    {"preview.step", ActionRisk::View},
    {"preview.cloud_download", ActionRisk::System},
    // 磁盘 · 网络
    {"drive.eject", ActionRisk::System},
    {"drive.disconnect", ActionRisk::System},
    {"drive.unlock", ActionRisk::UserOnly},
    {"net.map_drive", ActionRisk::System},
    {"net.connect_server", ActionRisk::View},
    {"net.place", ActionRisk::Organize},
    {"net.recent", ActionRisk::Organize},
    {"net.rescan", ActionRisk::View},
    {"net.system_settings", ActionRisk::View},
    // 工具
    {"git.refresh", ActionRisk::Read},
    {"git.pull", ActionRisk::External},
    {"git.push", ActionRisk::External},
    {"toolbar.open_with", ActionRisk::External},
    {"toolbar.pin_app", ActionRisk::Organize},
    {"toolbar.customize", ActionRisk::View},
    {"toolbar.tools", ActionRisk::Organize},
    // 设置 · 快捷键
    {"settings.read", ActionRisk::Read},
    {"settings.set", ActionRisk::View, true},
    {"keymap.read", ActionRisk::Read},
    {"keymap.set", ActionRisk::Organize},
    {"assistant.prefs", ActionRisk::UserOnly},   // its own permissions and connections: only the user
    {"assistant.organize_here", ActionRisk::UserOnly},   // fills the assistant's box for the user to send
};

// In KeyAction order, one per action (checked at compile time).
inline constexpr KeyActionLink kKeyActionLinks[] = {
    {KeyAction::GlobalSearch, {"search.global"}},
    {KeyAction::Open, {"nav.open"}},
    {KeyAction::Back, {"nav.back"}},
    {KeyAction::Forward, {"nav.forward"}},
    {KeyAction::Up, {"nav.up"}},
    {KeyAction::GoToPath, {"nav.go"}},
    {KeyAction::Refresh, {"nav.refresh"}},
    {KeyAction::MoveFocus, {"", "keyboard focus moves; actions select with select.items"}},
    {KeyAction::QuickPreview, {"preview.toggle"}},
    {KeyAction::Copy, {"file.copy"}},
    {KeyAction::Cut, {"file.cut"}},
    {KeyAction::Paste, {"file.paste"}},
    {KeyAction::CopyPath, {"file.copy_path"}},
    {KeyAction::StageToTray, {"tray.add"}},
    {KeyAction::Undo, {"edit.undo"}},
    {KeyAction::Rename, {"file.rename"}},
    {KeyAction::BatchRename, {"file.batch_rename"}},
    {KeyAction::NewFolder, {"file.new_folder"}},
    {KeyAction::GroupIntoFolder, {"file.group_into_folder"}},
    {KeyAction::DissolveFolder, {"file.dissolve"}},
    {KeyAction::Delete, {"file.recycle"}},
    {KeyAction::PermanentDelete, {"file.delete_permanent"}},
    {KeyAction::Properties, {"file.properties"}},
    {KeyAction::ContextMenu, {"file.context_menu"}},
    {KeyAction::ToggleTag, {"tag.toggle"}},
    {KeyAction::ClearTags, {"tag.clear"}},
    {KeyAction::TagPanel, {"tag.panel"}},
    {KeyAction::SelectAll, {"select.all"}},
    {KeyAction::InvertSelection, {"select.invert"}},
    {KeyAction::SelectPattern, {"select.pattern"}},
    {KeyAction::Search, {"search.open"}},
    {KeyAction::CommandBar, {"", "opens the command palette itself"}},
    {KeyAction::Project, {"search.project"}},
    {KeyAction::Filter, {"view.filter"}},
    {KeyAction::AdvancedSearch, {"search.advanced"}},
    {KeyAction::NewTab, {"tab.new"}},
    {KeyAction::CloseTab, {"tab.close"}},
    {KeyAction::NextTab, {"tab.activate"}},
    {KeyAction::TabNumber, {"tab.activate"}},
    {KeyAction::Layout, {"window.layout"}},
    {KeyAction::ViewMode, {"view.mode"}},
    {KeyAction::NextPane, {"pane.focus"}},
    {KeyAction::MarkTarget, {"pane.mark_target"}},
    {KeyAction::CopyToTarget, {"file.copy_to"}},
    {KeyAction::MoveToTarget, {"file.move_to"}},
    {KeyAction::Sidebar, {"sidebar.toggle"}},
    {KeyAction::FavoritesBar, {"favbar.toggle"}},
    {KeyAction::Maximize, {"window.maximize"}},
    {KeyAction::NewWindow, {"window.new"}},
    {KeyAction::CloseWindow, {"window.close"}},
    {KeyAction::TabToWindow, {"tab.to_window"}},
    {KeyAction::ReopenClosed, {"window.reopen"}},
    {KeyAction::SwitchWindow, {"window.activate"}},
    {KeyAction::Overview, {"window.overview"}},
    {KeyAction::MergeWindows, {"window.merge"}},

    {KeyAction::AiAssistant, {"", "the assistant itself"}},
    {KeyAction::AiHistory, {"", "the assistant's own chat history"}},
    {KeyAction::ShortcutHelp, {"app.shortcut_help"}},
    {KeyAction::ArrangeSideBySide, {"window.arrange"}},
    {KeyAction::ArrangeStacked, {"window.arrange"}},
    {KeyAction::ArrangeColumns, {"window.arrange"}},
    {KeyAction::ArrangeTile, {"window.arrange"}},
    {KeyAction::ArrangeMain, {"window.arrange"}},
    {KeyAction::ArrangeCascade, {"window.arrange"}},
};

inline constexpr MenuCmdLink kMenuCmdLinks[] = {
    {CmdNone, {"", "no command"}},
    {CmdOpen, {"nav.open"}},
    {CmdCut, {"file.cut"}},
    {CmdCopy, {"file.copy"}},
    {CmdPaste, {"file.paste"}},
    {CmdDelete, {"file.recycle"}},
    {CmdRename, {"file.rename"}},
    {CmdProperties, {"file.properties"}},
    {CmdOpenTerminal, {"file.terminal"}},
    {CmdCopyPath, {"file.copy_path"}},
    {CmdUndo, {"edit.undo"}},
    {CmdNewFolder, {"file.new_folder"}},
    {CmdNewTextFile, {"file.new_file"}},
    {CmdCopyAsPath, {"", "reserved id, no command"}},
    {CmdOpenPath, {"nav.reveal"}},
    {CmdOpenInNewTab, {"nav.open_in_new_tab"}},
    {CmdSelectAll, {"select.all"}},
    {CmdInvertSelection, {"select.invert"}},
    {CmdSelectWildcard, {"select.pattern"}},
    {CmdRefresh, {"nav.refresh"}},
    {CmdFolderProperties, {"file.properties"}},
    {CmdSortName, {"view.sort"}},
    {CmdSortModified, {"view.sort"}},
    {CmdSortType, {"view.sort"}},
    {CmdSortSize, {"view.sort"}},
    {CmdSortPath, {"view.sort"}},
    {CmdSortAscending, {"view.sort"}},
    {CmdSortDescending, {"view.sort"}},
    {CmdFolderSortTop, {"view.folders_first"}},
    {CmdFolderSortFollow, {"view.folders_first"}},
    {CmdFolderSortMixed, {"view.folders_first"}},
    {CmdSortCreated, {"view.sort"}},
    {CmdSortAccessed, {"view.sort"}},
    {CmdLayoutSingle, {"window.layout"}},
    {CmdLayoutTwoVertical, {"window.layout"}},
    {CmdLayoutTwoHorizontal, {"window.layout"}},
    {CmdLayoutThree, {"window.layout"}},
    {CmdLayoutFourGrid, {"window.layout"}},
    {CmdCopyToTarget, {"file.copy_to"}},
    {CmdMoveToTarget, {"file.move_to"}},
    {CmdPinWorkspace, {"workspace.pin"}},
    {CmdPinNetwork, {"net.place"}},
    {CmdSearchAll, {"search.open"}},
    {CmdInstallFullIndex, {"index.install_full"}},
    {CmdSettings, {"app.settings_page"}},
    {CmdSettingsContextMenu, {"app.settings_page"}},
    {CmdTags, {"tag.panel"}},
    {CmdTagBase, {"tag.toggle"}},
    {CmdViewBase, {"view.mode"}},
    {CmdViewWall, {"view.wall"}},
    {CmdTabColorBase, {"tab.color"}},
    {CmdTabGroupNewTab, {"tab.group"}},
    {CmdTabGroupUngroup, {"tab.group"}},
    {CmdTabGroupClose, {"tab.group"}},
    {CmdDetailsPanel, {"details.toggle"}},
    {CmdDetailsComputeSize, {"file.compute_size"}},
    {CmdDetailsShellMenu, {"file.context_menu"}},
    {CmdTabNewRight, {"tab.new"}},
    {CmdTabDuplicate, {"tab.duplicate"}},
    {CmdTabPin, {"tab.pin"}},
    {CmdTabClose, {"tab.close"}},
    {CmdTabAddToNewGroup, {"tab.group"}},
    {CmdTabRemoveFromGroup, {"tab.group"}},
    {CmdTabCloseOthers, {"tab.close_others"}},
    {CmdTabCloseRight, {"tab.close_others"}},
    {CmdTabRename, {"tab.rename"}},
    {CmdTabNameSave, {"tab.rename"}},
    {CmdTabNameReset, {"tab.rename"}},
    {CmdTabColorNone, {"tab.color"}},
    {CmdTabJoinGroupBase, {"tab.group"}},
    {CmdEditStarBadge, {"fav.add"}},
    {CmdRemoveStarred, {"fav.add"}},
    {CmdRemoveRecent, {"recent.remove"}},
    {CmdBatchRename, {"file.batch_rename"}},
    {CmdAdvancedSearch, {"search.advanced"}},
    {CmdRestoreRecycle, {"recycle.restore"}},
    {CmdEmptyRecycle, {"recycle.empty"}},
    {CmdOpenRecycle, {"nav.recycle_bin"}},
    {CmdPinQuickAccess, {"quick_access.pin"}},
    {CmdUnpinQuickAccess, {"quick_access.pin"}},
    {CmdViewRecentChanges, {"nav.recent_changes"}},
    {CmdColumnLayout, {"view.columns"}},
    {CmdShortcutHelp, {"app.shortcut_help"}},
    {CmdCompareSideBySide, {"view.compare"}},
    {CmdCompareToggle, {"view.compare"}},
    {CmdCompareDiffOnly, {"view.compare"}},
    {CmdGroupNone, {"view.group"}},
    {CmdGroupName, {"view.group"}},
    {CmdGroupDate, {"view.group"}},
    {CmdGroupType, {"view.group"}},
    {CmdGroupSize, {"view.group"}},
    {CmdGroupTag, {"view.group"}},
    {CmdGroupLocation, {"view.group"}},
    {CmdRestoreAllRecycle, {"recycle.restore"}},
    {CmdApplyViewToAllFolders, {"view.apply_all"}},
    {CmdApplyGroupToAllFolders, {"view.apply_all"}},
    {CmdCustomizeToolbar, {"toolbar.customize"}},
    {CmdNewMarkdownFile, {"file.new_file"}},
    {CmdExitPulse, {"app.exit"}},
    {CmdRecentBase, {"nav.go"}},
    {CmdEjectDrive, {"drive.eject"}},
    {CmdDisconnectDrive, {"drive.disconnect"}},
    {CmdUnlockDrive, {"drive.unlock"}},
    {CmdMapNetworkDrive, {"net.map_drive"}},
    {CmdUnpinNetwork, {"net.place"}},
    {CmdCompressZip, {"file.zip"}},
    {CmdToggleFolderSizes, {"view.folder_sizes"}},
    {CmdStageToTray, {"tray.add"}},
    {CmdCloudKeep, {"cloud.keep"}},
    {CmdCloudFree, {"cloud.free"}},
    {CmdGroupIntoFolder, {"file.group_into_folder"}},
    {CmdDissolveFolder, {"file.dissolve"}},
    {CmdSetAlias, {"alias.set"}},
    {CmdAliasDisplayBase, {"alias.display"}},
    {CmdManageAliases, {"alias.manage"}},
    {CmdExportAliases, {"alias.export"}},
    {CmdImportAliases, {"alias.export"}},
    {CmdAliasBase, {"nav.go"}},
    {CmdAliasOpenBase, {"alias.manage"}},
    {CmdAliasRemoveBase, {"alias.set"}},
    {CmdIndexBase, {"", "command palette result rows"}},
    {CmdShellNewBase, {"file.new_file"}},
    {CmdCopyToWindowBase, {"file.copy_to"}},
    {CmdMoveToWindowBase, {"file.move_to"}},
    {CmdToolbarAppBase, {"toolbar.open_with"}},
    {CmdToolbarPinAppBase, {"toolbar.pin_app"}},
    {CmdToolbarToolBase, {"toolbar.tools"}},
    {CmdToolbarToolEnd, {"", "exclusive end of toolbar tool command range"}},
    {CmdNewPrimaryBase, {"toolbar.tools"}},
    {CmdConnectServer, {"net.connect_server"}},
    {CmdForgetNetworkRecent, {"net.recent"}},
    {CmdAddNetworkPlace, {"net.place"}},
    {CmdLanRescan, {"net.rescan"}},
    {CmdNetworkSharingSettings, {"net.system_settings"}},
    {CmdRenameNetwork, {"net.place"}},
    {CmdNetworkClearRecent, {"net.recent"}},
    {CmdNetworkHiddenShares, {"settings.set"}},
    {CmdNetworkDiscoverySettings, {"app.settings_page"}},
    {CmdFindDuplicates, {"file.find_duplicates"}},
    {CmdAiOrganizeHere, {"assistant.organize_here"}},
    {CmdDupKeepThis, {"file.find_duplicates"}},
    {CmdDupReapplyRules, {"file.find_duplicates"}},
    {CmdShellStaticBase, {"shell.run_verb"}},
    {CmdShellComBase, {"shell.run_verb"}},
    {CmdShellComLast, {"shell.run_verb"}},
    {kCmdGitPull, {"git.pull"}},
    {kCmdGitPush, {"git.push"}},
    {kCmdGitRefresh, {"git.refresh"}},
};

constexpr const ActionEntry* FindAction(std::string_view id) noexcept {
    for (const ActionEntry& a : kActions)
        if (a.id == id) return &a;
    return nullptr;
}

} // namespace pulse::app::actions

#pragma once
// explorer_shortcuts.h — keys that follow File Explorer (Windows 11), so muscle
// memory carries over. Pure mapping; HandleKeyDown performs the actions.
//
//   Ctrl+D               delete to Recycle Bin (Shift: permanently)
//   Ctrl+Alt+D           mark the target pane (was Ctrl+D)
//   Ctrl+Shift+N         new folder
//   Ctrl+E, F3           search / filter this folder (same as Ctrl+F)
//   Ctrl+1..8, Ctrl+9    go to tab N / the last tab
//   Ctrl+Alt+1..4        split layouts (were Ctrl+1..4)
//   Ctrl+Shift+1..8      view: extra large, large, medium, small icons,
//                        list, details, tiles, content
//   Alt+1..9             toggle favorite tag N (was Ctrl+Alt+Shift+1..7)
//   Alt+0, Alt+T         clear every tag / open the tag panel
//   Backspace            back (Alt+Up stays "up one level")
//   Shift+F10, Apps key  context menu
//   F11                  maximize / restore
//   Win+E                new window through Windows shell integration
//   Ctrl+Shift+W         close this window (Ctrl+W, without Shift, closes the tab)
//   Ctrl+Shift+M         move the current tab to a new window (multi-window M4e)
//   Ctrl+G, Ctrl+Shift+G 合并到文件夹 / 解散文件夹 (group / ungroup, as in design tools)

#include <windows.h>

#include <cstddef>
#include <optional>
#include <vector>

namespace pulse::app {

enum class ExplorerKeyAction {
    None, Delete, MarkTarget, NewFolder, Search, TabNumber, Layout, ViewMode, Tag, ClearTags,
    TagPanel, Back,
    ContextMenu, Maximize, NewWindow, CloseWindow, MoveTabToNewWindow, MergeWindows,
    GroupIntoFolder, DissolveFolder
};

struct ExplorerKeyCommand {
    ExplorerKeyAction action = ExplorerKeyAction::None;
    int index = 0;        // 0-based tab/layout/view number, favorite tag slot
    bool shift = false;   // Ctrl+Shift+D: delete permanently
};

inline ExplorerKeyCommand ExplorerShortcut(UINT key, bool ctrl, bool shift, bool alt) noexcept {
    using A = ExplorerKeyAction;
    const bool digit = key >= L'1' && key <= L'9';
    const int number = digit ? static_cast<int>(key - L'1') : -1;
    if (ctrl && alt && !shift && key == L'D') return {A::MarkTarget};
    if (ctrl && !alt && key == L'D') return {A::Delete, 0, shift};
    if (ctrl && !shift && !alt && key == L'N') return {A::NewWindow};
    if (ctrl && shift && !alt && key == L'N') return {A::NewFolder};
    if (ctrl && shift && !alt && key == L'W') return {A::CloseWindow};
    if (ctrl && shift && !alt && key == L'M') return {A::MoveTabToNewWindow};
    if (ctrl && shift && !alt && key == L'J') return {A::MergeWindows};
    if (ctrl && !alt && key == L'G') return {shift ? A::DissolveFolder : A::GroupIntoFolder};
    if ((ctrl && !shift && !alt && key == L'E') || (!ctrl && !shift && !alt && key == VK_F3))
        return {A::Search};
    if (digit && ctrl && !shift && !alt) return {A::TabNumber, number};
    if (digit && ctrl && alt && !shift && number < 4) return {A::Layout, number};
    if (digit && ctrl && shift && !alt && number < 8) return {A::ViewMode, number};
    if (alt && !ctrl && !shift) {
        if (digit) return {A::Tag, number};
        if (key == L'0') return {A::ClearTags};
        if (key == L'T') return {A::TagPanel};
    }
    if (key == VK_BACK && !ctrl && !shift && !alt) return {A::Back};
    if ((key == VK_F10 && shift && !ctrl && !alt) || (key == VK_APPS && !ctrl && !alt)) return {A::ContextMenu};
    if (key == VK_F11 && !ctrl && !shift && !alt) return {A::Maximize};
    return {};
}

// Ctrl+1..8 picks the Nth visible tab; Ctrl+9 always the last one.
inline std::optional<size_t> NthVisibleTab(const std::vector<size_t>& visible, int index) noexcept {
    if (visible.empty() || index < 0 || index > 8) return std::nullopt;
    if (index == 8) return visible.back();
    if (static_cast<size_t>(index) >= visible.size()) return std::nullopt;
    return visible[static_cast<size_t>(index)];
}

} // namespace pulse::app

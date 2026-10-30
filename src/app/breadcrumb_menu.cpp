#include "app_internal.h"
#include "context_menu.h"
#include "../fs/fs_enum.h"
#include "../fs/device_path.h"
#include "../ops/clipboard.h"
#include "../common/localization.h"

namespace pulse {

namespace {

constexpr int kHideCloud = 32400;

// Runs a BuildBreadcrumbMenu command on `path` (a breadcrumb or a sidebar row).
void RunPathCommand(AppState& s, const std::wstring& path, bool filesystem, int command) {
    const std::wstring shell_path = ClipboardPath(path);
    switch (command) {
    case app::CmdOpenInNewTab:
        OpenFolderTab(s, path);
        break;
    case app::CmdOpen:
        NavigateTo(s, path);
        break;
    case app::CmdCopyPath:
        // A phone folder: the path Explorer's address bar shows, not Pulse's.
        ops::WriteClipboardText(fs::IsDevicePath(path) ? DeviceFriendlyPath(path) : shell_path);
        break;
    case app::CmdCopy:
        if (filesystem) ops::WriteClipboard({shell_path}, false);
        break;
    case app::CmdOpenTerminal:
        if (filesystem) s.ops.OpenTerminal(shell_path, s.appPrefs.terminal_program);
        break;
    case app::CmdProperties:
        if (filesystem) s.ops.ShowProperties(shell_path);
        break;
    default:
        break;
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

} // namespace

void ShowBreadcrumbMenu(AppState& s, std::wstring path, POINT screen_pt) {
    if (path.empty() || !EnsureMenu(s)) return;
    s.context_menu.Close();
    const bool filesystem = !fs::IsVirtualPath(path);
    // Keep the clicked path across TrackPopup's nested message loop. Selection
    // commands target file rows and must not be used for breadcrumb ancestors.
    const int command = s.menu->TrackPopup(screen_pt, app::BuildBreadcrumbMenu(filesystem));
    RunPathCommand(s, path, filesystem, command);
}

// #80: cloud rows (OneDrive, WPS网盘, ...) form a header-less section, so the
// row carries what a header menu would: the usual folder commands, then hiding
// the section. The toast
// says where to bring it back.
void ShowCloudPlaceMenu(AppState& s, const std::wstring& path, POINT screen_pt) {
    if (!EnsureMenu(s)) return;
    s.context_menu.Close();
    const bool filesystem = !path.empty() && !fs::IsVirtualPath(path);
    std::vector<ui::FluentMenuItem> items;
    if (!path.empty()) items = app::BuildBreadcrumbMenu(filesystem);
    if (!items.empty()) items.back().separator_after = true;
    ui::FluentMenuItem hide;
    hide.command = kHideCloud;
    hide.text = l10n::Get(l10n::StringId::SidebarHideCloud);
    items.push_back(std::move(hide));
    const int command = s.menu->TrackPopup(screen_pt, std::move(items));
    if (command == kHideCloud) {
        s.sidebarHiddenMask |= 1u << static_cast<int>(app::SidebarSectionId::Cloud);
        s.notification_toast.Show(s.hwnd, l10n::Get(l10n::StringId::SidebarCloudDrives),
            l10n::Get(l10n::StringId::SidebarCloudHiddenHint), false);
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    if (!path.empty()) RunPathCommand(s, path, filesystem, command);
}

} // namespace pulse

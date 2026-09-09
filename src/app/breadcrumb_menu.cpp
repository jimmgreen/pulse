#include "app_internal.h"
#include "context_menu.h"
#include "../fs/fs_enum.h"
#include "../ops/clipboard.h"

namespace pulse {

void ShowBreadcrumbMenu(AppState& s, std::wstring path, POINT screen_pt) {
    if (path.empty() || !EnsureMenu(s)) return;
    s.context_menu.Close();
    const bool filesystem = !fs::IsVirtualPath(path);
    // Keep the clicked path across TrackPopup's nested message loop. Selection
    // commands target file rows and must not be used for breadcrumb ancestors.
    const int command = s.menu->TrackPopup(screen_pt, app::BuildBreadcrumbMenu(filesystem));
    const std::wstring shell_path = ClipboardPath(path);
    switch (command) {
    case app::CmdOpenInNewTab:
        NewTab(s, path);
        break;
    case app::CmdOpen:
        NavigateTo(s, path);
        break;
    case app::CmdCopyPath:
        ops::WriteClipboardText(shell_path);
        break;
    case app::CmdCopy:
        if (filesystem) ops::WriteClipboard({shell_path}, false);
        break;
    case app::CmdOpenTerminal:
        if (filesystem) s.ops.OpenTerminal(shell_path);
        break;
    case app::CmdProperties:
        if (filesystem) s.ops.ShowProperties(shell_path);
        break;
    default:
        break;
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

} // namespace pulse

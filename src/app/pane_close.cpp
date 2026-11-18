#include "pane_close.h"
#include "app_internal.h"
#include "content_results_ui.h"
#include <algorithm>
namespace pulse {
bool CloseWorkspacePane(AppState& s, app::Pane* pane) {
    auto* owner = s.window_tabs.Active();
    if (!owner || !pane || !pane->ActiveTab() || owner->VisiblePanes().size() < 2 || IsSettingsTab(pane->ActiveTab())) return false;
    auto visible = owner->VisiblePanes();
    if (std::find(visible.begin(), visible.end(), pane) == visible.end()) return false;
    if (s.renameIndex >= 0) HideRenameOverlay(s, false);
    if (s.addressEditing) HideAddressEditor(s, false);
    if (s.filterEditing) HideFilterEditor(s, true);
    if (s.quickPreview.visible()) s.quickPreview.Close();
    CancelFileClick(s);
    s.blankClickPane = nullptr;
    s.blankClickTab = nullptr;
    s.blankDoublePane = nullptr;
    s.blankDoubleTab = nullptr;
    s.blankDoublePending = false;
    s.listTypeAheadTab = nullptr;
    s.paneDragPending = false;
    s.paneDragPane = nullptr;
    CancelContentSelection(s, *pane->ActiveTab());
    CancelUnsharedDirectoryRequest(s, *pane->ActiveTab());
    DropLiveNetworkSearch(s, pane->view.search_session_id);
    CancelActiveContentSearch(s, pane->view);
    RememberLayoutFocus(s);
    ClosedPane saved;
    saved.owner = owner;
    saved.contents = app::TakePaneAsTab(*owner, pane, saved.origin);
    if (!saved.contents) return false;
    saved.remaining = owner->VisiblePanes();
    s.closedPane = std::move(saved);
    s.notification_toast.ShowSnack(s.hwnd, ui::SnackIcon::Undo,
        l10n::Get(l10n::StringId::WorkspacePaneClosed), {},
        {{l10n::Get(l10n::StringId::SnackRestore), WM_REOPEN_WORKSPACE_PANE}}, L"");
    BindCurrentLayout(s);
    RevalidateVisibleFolders(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return true;
}
bool ReopenWorkspacePane(AppState& s) {
    auto& saved = s.closedPane;
    if (!saved.contents) return false;
    auto it = std::find_if(s.window_tabs.items.begin(), s.window_tabs.items.end(),
        [&](const auto& tab) { return tab.get() == saved.owner; });
    if (it == s.window_tabs.items.end() || (*it)->VisiblePanes() != saved.remaining ||
        !app::CanPutPaneBack(**it, *saved.contents, saved.origin)) return false;
    RememberLayoutFocus(s);
    s.window_tabs.SwitchTab(static_cast<size_t>(it - s.window_tabs.items.begin()));
    if (!app::PutPaneBack(**it, std::move(saved.contents), saved.origin)) return false;
    saved = ClosedPane{};
    BindCurrentLayout(s);
    RevalidateVisibleFolders(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return true;
}
}

#include "file_click_controller.h"
#include "app_input.h"
#include "app_commands.h"
#include "app_navigation.h"
#include "rename_click_timing.h"

namespace pulse {
app::FileClickIdentity FileClickIdentityFor(AppState& s, int index) {
    auto* tab = ActiveTab(s);
    if (!tab || index < 0 || index >= tab->CountBound() ||
        index >= static_cast<int>(tab->EntryCount())) return {};
    auto path = EntryFullPath(*tab, index);
    if (path.empty()) path = tab->current_path + L"\n" + tab->EntryAt(index).name;
    return {s.pane, tab, tab->snapshot.get(), tab->view_generation, index, std::move(path)};
}

void CancelFileClick(AppState& s) { s.fileClick.Cancel(); }

void ExecuteFileClickOpen(AppState& s, const app::FileClickIdentity& item, bool shift, bool single) {
    if (!item.Valid() || FileClickIdentityFor(s, item.index) != item) return;
    ActiveTab(s)->SelectOnly(item.index);
    const auto message_time = static_cast<uint32_t>(GetMessageTime());
    OpenSelected(s, shift);
    if (single) s.fileClick.SuppressDoubleClick(message_time);
}

void TickFileClick(AppState& s, ULONGLONG now) {
    if (!s.fileClick.RenamePending()) return;
    auto* tab = ActiveTab(s);
    const auto item = s.fileClick.Item();
    const bool eligible = GetForegroundWindow() == GetAncestor(s.hwnd, GA_ROOT) &&
        tab && tab->SelectedCount() == 1 && tab->selected_index == item.index &&
        s.renameIndex < 0 && s.tagRenameId.empty() && s.workspaceRenameRoot.empty() &&
        !s.addressEditing && !s.addressSearching && !s.filterEditing &&
        !s.listSelection.PendingFile() && !s.listSelection.PendingBand() && !s.listSelection.BandActive() &&
        !s.scrollbarDragging && !s.splitterDragging;
    if (s.fileClick.Tick(FileClickIdentityFor(s, item.index), now, eligible,
        app::RenameClickReady(now, s.fileClick.RenameDue())) == app::FileClickAction::Rename)
        RenameSelected(s);
}
}

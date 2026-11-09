#include "app_input.h"
#include "app_commands.h"
#include "app_navigation.h"

namespace pulse {
bool HandleListCharacter(AppState& s, wchar_t ch) {
    // Native text editors own their input; shortcuts never become a prefix.
    if (GetFocus() != s.hwnd || (GetKeyState(VK_CONTROL) & 0x8000) ||
        (GetKeyState(VK_MENU) & 0x8000) || ch <= L' ' ||
        s.renameIndex >= 0 || s.filterEditing || !s.tagRenameId.empty()) return false;
    auto* tab = ActiveTab(s);
    if (!tab || tab->loading || !tab->snapshot || tab->content_results || IsSettingsTab(tab)) return false;
    if (s.listTypeAheadTab != tab || s.listTypeAheadFolder != tab->current_path) {
        s.listTypeAhead = {};
        s.listTypeAheadTab = tab;
        s.listTypeAheadFolder = tab->current_path;
    }
    const auto vm = BuildVm(s, false);
    // Details tree rows index past the snapshot (expanded children).
    const std::vector<fs::DirEntry>& rows = tab->TreeEntries() ? *tab->TreeEntries() : *tab->snapshot;
    const int current = vm.pane.ViewIndex(tab->selected_index);
    const int next = s.listTypeAhead.Find(ch, GetTickCount64(), current,
        static_cast<int>(vm.pane.EntryCount()), [&](int view) -> const std::wstring& {
            return rows[static_cast<size_t>(vm.pane.SourceIndex(view))].name;
        });
    if (next >= 0) {
        CancelFileClick(s);
        CancelScrollAnimation(s);
        tab->SelectOnly(vm.pane.SourceIndex(next));
        EnsureRowVisible(s, *tab, tab->selected_index);
        InvalidateRect(s.hwnd, nullptr, FALSE);
    }
    return true;
}

void TickQuickPreviewSelection(AppState& s, ULONGLONG now) {
    if (!s.quickPreview.visible()) { s.quickPreviewSelection.Reset(); return; }
    auto* tab = ActiveTab(s);
    if (!tab || tab->loading || s.quickPreviewAnchorView >= 0) return;
    ui::QuickPreviewItem item;
    SelectedQuickPreviewItem(s, item);
    if (!s.quickPreviewSelection.Observe(item.path, now)) return;
    // Empty selections leave the current preview available. The next selected
    // file will replace it. Update cancels old requests through its generation.
    if (!item.path.empty() && item.path != s.quickPreview.item().path)
        s.quickPreview.Update(item);
}
} // namespace pulse

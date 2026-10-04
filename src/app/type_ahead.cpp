// type_ahead.cpp — Keyboard type-ahead for the file list (#93).
#include "type_ahead.h"
#include "app_input.h"
#include "app_navigation.h"
#include "../ui/type_ahead.h"

#include <string>
#include <string_view>
#include <windows.h>

namespace pulse::app {

namespace {

// A printable character already bound to another WM_KEYDOWN shortcut must not
// reach type-ahead: TranslateMessage still posts WM_CHAR after the key is
// handled, so both would fire for one press.
bool IsReservedChar(wchar_t ch) {
    return ch == L' ';  // VK_SPACE toggles the quick preview.
}

bool TextEntryActive(const AppState& s) {
    // Hosted edits own their own HWND, but the flag also covers the window
    // regaining focus while an editor is still on screen.
    return s.filterEditing || s.addressEditing || s.renameIndex >= 0 ||
           !s.tagRenameId.empty() || (s.menu && s.menu->IsOpen());
}

} // namespace

bool HandleTypeAheadChar(AppState& s, wchar_t ch) {
    // Control characters (Ctrl+A arrives as 0x01), DEL, and anything typed
    // with Ctrl/Alt held belongs to a shortcut, not to type-ahead.
    if (ch < 0x20 || ch == 0x7F) return false;
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) return false;
    if ((GetKeyState(VK_MENU) & 0x8000) != 0) return false;
    if (IsReservedChar(ch)) return false;
    if (TextEntryActive(s)) return false;

    Tab* tab = ActiveTab(s);
    if (!tab) return false;
    if (IsSettingsTab(tab)) return false;

    ui::WindowViewModel vm = BuildVm(s);
    const int count = static_cast<int>(vm.pane.EntryCount());
    if (count <= 0) return true;  // nothing to jump to; still swallow the char
    const int from = vm.pane.ViewIndex(tab->selected_index);

    std::wstring scratch;  // only used by the content-results branch
    auto name_at = [tab, &vm, &scratch](int view_row) -> std::wstring_view {
        const int src = vm.pane.SourceIndex(view_row);
        if (src < 0) return {};
        if (tab->content_results) {
            index::ContentResultStore::Row row;
            if (!tab->content_results->Get(static_cast<size_t>(src), row)) return {};
            scratch = std::move(row.entry.name);
            return scratch;
        }
        if (tab->snapshot && static_cast<size_t>(src) < tab->snapshot->size())
            return (*tab->snapshot)[static_cast<size_t>(src)].name;
        return {};
    };

    const int next = ui::NextPrefixMatch(count, from, name_at, std::wstring_view(&ch, 1));
    if (next >= 0) {
        tab->MoveFocus(vm.pane.SourceIndex(next), false);
        EnsureRowVisible(s, *tab, tab->selected_index);
    }
    ClampScroll(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return true;
}

} // namespace pulse::app

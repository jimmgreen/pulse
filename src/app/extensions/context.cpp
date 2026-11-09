#include "context.h"
#include "../app_runtime.h"
#include "../../common/display_path.h"
#include "../../common/localization.h"

namespace pulse::extensions {
namespace {
std::wstring ViewModeLabel(ui::ViewMode mode) {
    using I = l10n::StringId;
    switch (mode) {
    case ui::ViewMode::ExtraLargeIcons: return l10n::Get(I::ViewExtraLarge);
    case ui::ViewMode::LargeIcons: return l10n::Get(I::ViewLarge);
    case ui::ViewMode::MediumIcons: return l10n::Get(I::MediumIcons);
    case ui::ViewMode::SmallIcons: return l10n::Get(I::ViewSmall);
    case ui::ViewMode::List: return l10n::Get(I::ViewList);
    case ui::ViewMode::Details: return l10n::Get(I::ViewDetails);
    case ui::ViewMode::Tiles: return l10n::Get(I::ViewTiles);
    case ui::ViewMode::Content: return l10n::Get(I::ViewContent);
    }
    return {};
}
std::wstring Bounded(std::wstring value, size_t limit) {
    if (value.size() <= limit) return value;
    value.resize(limit - 1);
    if (!value.empty() && value.back() >= 0xd800 && value.back() <= 0xdbff) value.pop_back();
    value.push_back(L'\x2026');
    return value;
}

}
PanelContext CapturePanelContext(AppState& state) {
    PanelContext context;
    const auto* tab = ActiveTab(state);
    if (!tab) return context;
    context.directory = Bounded(path::FriendlyPathText(tab->current_path), 2048);
    context.view_mode = ViewModeLabel(tab->view_mode);
    // Old selection/snapshot may still be present during navigation.
    if (tab->loading) return context;
    context.selection_count = static_cast<size_t>(tab->SelectedCount());
    if (context.selection_count != 1 || tab->content_results) return context;
    int selected = -1;
    if (tab->all_selected && tab->CountBound() == 1) selected = 0;
    else if (tab->selected.size() == 1) selected = *tab->selected.begin();
    if (selected >= 0 && static_cast<size_t>(selected) < tab->EntryCount() && tab->IsSelected(selected))
        context.selection_name = Bounded(tab->EntryAt(static_cast<size_t>(selected)).name, 256);
    return context;
}
}

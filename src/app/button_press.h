// Release-to-activate state for non-drag controls in the main window.
#pragma once
#include "../ui/ui_view_model.h"
#include "../ui/ai_settings_view.h"
#include "../ui/sysctx_view.h"

namespace pulse::app {
inline bool IsReleaseButton(const ui::HitTestResult& hit) {
    using H = ui::HitTestResult;
    switch (hit.region) {
    case H::SettingsToggle:
        return hit.index < ui::kSettingsMenuFixedHit;
    case H::SettingsSysCtx:
        return hit.index < ui::kSysCtxPreviewBase || hit.index >= ui::kSysCtxSlowSkipBase;
    case H::SettingsAssistant:
        return hit.index < ui::kAsstThinkBase || hit.index >= ui::kAsstOtherBase;
    case H::SettingsPackAction:
        return hit.index < static_cast<int>(ui::PackAction::SzLevelBase) ||
            hit.index >= static_cast<int>(ui::PackAction::SzLevelBase) + 6;
    case H::Close:
    case H::Minimize:
    case H::Maximize:
    case H::TabClose:
    case H::TabNew:
    case H::ThemeToggle:
    case H::WindowManageButton:
    case H::SettingsButton:
    case H::NavBack:
    case H::NavForward:
    case H::NavUp:
    case H::NavRefresh:
    case H::NewButton:
    case H::Cut:
    case H::Copy:
    case H::Paste:
    case H::Rename:
    case H::Delete:
    case H::SplitButton:
    case H::DetailsToggle:
    case H::PaneMediumIcons:
    case H::PaneViewButton:
    case H::PaneClose:
    case H::PaneDetails:
    case H::PaneSizeButton:
    case H::PaneWall:
    case H::ToolbarMore:
    case H::ToolbarSelAction:
    case H::FavoritesBarMore:
    case H::FavoritesBarChevron:
    case H::AddressFavorites:
    case H::AddressSearch:
    case H::AddressSearchScope:
    case H::AddressSearchMode:
    case H::AddressSearchContent:
    case H::AddressSearchOptions:
    case H::ContentIndexManage:
    case H::NetworkIndexAdd:
    case H::PaneBannerButton:
    case H::SettingsContentIndex:
    case H::SettingsSearchResult:
    case H::SettingsSearchClear:
    case H::SettingsDisclosure:
    case H::SettingsTheme:
    case H::SettingsDropdown:
    case H::SettingsContentAction:
    case H::AddressSearchClear:
    case H::AddressSearchClose:
    case H::BreadcrumbSegment:
    case H::BreadcrumbArrow:
    case H::FilterClear:
    case H::ChangeBadge:
    case H::ChangeOpen:
    case H::ChangeTimeFilter:
    case H::ChangeTypeFilter:
    case H::ChangeMore:
    case H::SidebarHeaderAction:
    case H::SidebarItemAction:
    case H::SidebarWorkspaceAction:
    case H::SidebarItemExpand:
    case H::TrayRelease:
    case H::TrayClose:
    case H::TrayItemRemove:
    case H::TrayIntent:
    case H::CompareDiffToggle:
    case H::CompareExit:
    case H::TrayClear:
    case H::RowStar:
    case H::RowTagDot:
    case H::RowFolderSize:
    case H::RowNewTab:
    case H::RowMore:
    case H::RecentFilter:
    case H::RecentClear:
    case H::TagQueryChip:
    case H::TagQueryChipRemove:
    case H::TagQueryRefine:
    case H::PaneEmptyNewFolder:
    case H::DetailsOpen:
    case H::DetailsStar:
    case H::DetailsMore:
    case H::DetailsRename:
    case H::DetailsTagAdd:
    case H::DetailsNewTab:
    case H::DetailsCopyPath:
    case H::DetailsSection:
    case H::DetailsAttrToggle:
    case H::DetailsSecurityChange:
    case H::DetailsPresetTag:
    case H::DetailsPreviewToggle:
    case H::DetailsPreviewEnable:
    case H::StatusUndo:
    case H::StatusUndoHistory:
    case H::StatusRedo:
    case H::UndoHistoryRow:
    case H::PaneExpand:
    case H::PaneLayoutPeek:
    case H::PaneLayoutPreset:
    case H::PaneLayoutZoom:
    case H::PaneLayoutRestore:
    case H::PaneLayoutCell:
    case H::PaneLayoutTray:
    case H::PaneLayoutCompare:
    case H::StatusBarTask:
    case H::StatusBarCancelSearch:
    case H::SettingsNav:
    case H::SettingsDialogManagement:
    case H::SettingsIntegration:
    case H::SettingsGlobalSearchHotkey:
    case H::SettingsChangeDays:
    case H::SettingsRestore:
    case H::SettingsEffect:
    case H::SettingsWallpaper:
    case H::SettingsDensity:
    case H::SettingsMtimeStyle:
    case H::SettingsMtimeRange:
    case H::SettingsMtimePalette:
    case H::SettingsFolderSort:
    case H::SettingsStartupOpen:
    case H::SettingsNotifyIcon:
    case H::SettingsTabLayout:
    case H::SettingsTabRows:
    case H::SettingsNewTabOpen:
    case H::SettingsOpenPulseIcon:
    case H::SettingsNewWindowShows:
    case H::SettingsArrangeGap:
    case H::SettingsExternalOpen:
    case H::SettingsBlankClick:
    case H::SettingsHomeFolder:
    case H::SettingsLanCache:
    case H::SettingsThumbCache:
    case H::SettingsTextRender:
    case H::SettingsUiFontSize:
    case H::SettingsTrayIcon:
    case H::SettingsDoubleCtrlGap:
    case H::SettingsLanguage:
    case H::SettingsIndexVolume:
    case H::SettingsIndexAction:
    case H::SettingsIndexExcludeAction:
    case H::SettingsIndexExcludeRemove:
    case H::SettingsNetworkAction:
    case H::SettingsNetworkRemove:
    case H::SettingsDiagnosticsAction:
    case H::SettingsUpdateAction:
    case H::SettingsAboutAction:
    case H::SettingsReleaseNote:
    case H::SettingsDupScope:
    case H::SettingsDupDrive:
    case H::SettingsDupBrowse:
    case H::SettingsDupScan:
    case H::SettingsDupCancel:
    case H::SettingsDupMinSize:
    case H::SettingsDupKeep:
    case H::SettingsDupOpen:
    case H::SettingsDupGroupDelete:
    case H::SettingsDupDeleteAll:
    case H::SearchFilter:
    case H::TrayPrev:
    case H::TrayNext:
    case H::PaneColumnLayout:
    case H::SettingsAccentCustom:
    case H::SidebarToggle:
    case H::WindowIdentity:
    case H::StatusHintAction:
    case H::SearchEmptyAction:
    case H::TeachPrimary:
    case H::TeachDismiss:
    case H::TeachNever:
    case H::FilterEmptyAction:
    case H::TrayDest:
    case H::TrayStale:
    case H::TrayCompare:
    case H::GroupHeader:
    case H::GroupSelect:
    case H::ToolbarGroupClear:
    case H::SettingsPreviewStore:
    case H::TabScroll:
    case H::NewButtonArrow:
    case H::SettingsCtxPivot:
    case H::SettingsShortcuts:
    case H::StagedBadge:
    case H::PaneWallHint:
    case H::StatusBarAi:
    case H::SettingsAi:
    case H::SettingsClickMode:
    case H::SettingsNameBand:
    case H::RowCheck:
    case H::HeaderCheck:
    case H::SidebarBarSwitch:
    case H::FavoritesBarDock:
        return true;
    default:
        return false; // File selection, text, sliders and drag handles keep their press semantics.
    }
}

struct ButtonPressContext {
    const void* pane = nullptr;
    const void* tab = nullptr;
    const void* target_tab = nullptr;
    uint64_t generation = 0;
    int settings_page = -1;
    bool operator==(const ButtonPressContext&) const = default;
};

struct ButtonPress {
    bool active = false;
    bool inside = false;
    ui::HitTestResult hit;
    ButtonPressContext context;

    bool Matches(const ui::HitTestResult& other, const ButtonPressContext& current) const {
        return active && context == current && hit.region == other.region &&
            hit.index == other.index && hit.sub_index == other.sub_index &&
            hit.pane_index == other.pane_index && hit.sidebar_section == other.sidebar_section &&
            hit.sidebar_item == other.sidebar_item && hit.sidebar_action == other.sidebar_action &&
            hit.column == other.column && hit.path == other.path && hit.label == other.label;
    }
    void Begin(const ui::HitTestResult& target, const ButtonPressContext& current) {
        hit = target;
        context = current;
        active = inside = true;
    }
    void Cancel() { *this = {}; }
};
} // namespace pulse::app

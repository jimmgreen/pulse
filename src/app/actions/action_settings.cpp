// action_settings.cpp - The settings rows (action_settings.h).
#include "action_settings.h"

#include "../app_internal.h"
#include "../app_network_page.h"
#include "../folder_alias.h"
#include "../../ui/window_material.h"

namespace pulse::app::actions {

namespace {

using I = l10n::StringId;

std::wstring Text(I id) { return l10n::Get(id).c_str(); }

void SetSwitchByClick(AppState& s, int index) {
    // app_input's ToggleSetting: network switches first, then the controller.
    if (HandleNetworkSettingToggle(s, index)) return;
    s.settings.ToggleUi(index);
}

int LanguageIndex(const std::wstring& id) {
    return id == L"zh-CN" ? 1 : id == L"zh-TW" ? 2 : id == L"en-US" ? 3 : 0;
}

#define SET_GET(expr) +[](AppState& s) -> int { return static_cast<int>(expr); }
#define SET_SW(id, label, page, flags, index, field) \
    {id, I::label, page, flags, index, SET_GET(s.appPrefs.field), nullptr, {}}

const SetDef kSettingDefs[] = {
    // 外观
    {"theme", I::SettingsTheme, 0, 0, -1,
     SET_GET(s.themeOverride == ui::ThemeMode::Light ? 1 : s.themeOverride == ui::ThemeMode::Dark ? 2 : 0),
     +[](AppState& s, int i) { static constexpr int modes[] = {1, 2, 0}; SetThemeMode(s, modes[i]); },
     {{{"light", I::SettingsThemeLight, 1}, {"dark", I::SettingsThemeDark, 2}, {"system", I::SettingsThemeSystem, 0}}}},
    {"window_effect", I::SettingsWindowEffect, 0, 0, -1, SET_GET(ui::WindowEffectFromId(s.appPrefs.window_effect)),
     +[](AppState& s, int i) { s.settings.WindowEffect(ui::WindowEffectId(static_cast<ui::WindowEffect>(i))); },
     {{{"none", I::EffectNone, 0}, {"acrylic", I::EffectAcrylic, 1}, {"mica", I::EffectMica, 2}, {"mica_alt", I::EffectMicaAlt, 3}}}},
    {"text_rendering", I::SettingsTextRender, 0, 0, -1, SET_GET(s.appPrefs.text_render),
     +[](AppState& s, int i) { s.settings.TextRendering(i); },
     {{{"auto", I::TextRenderAuto, 0}, {"sharp", I::TextRenderSharp, 1}, {"smooth", I::TextRenderSmooth, 2}}}},
    {"font_size", I::SettingsUiFontSize, 0, 0, -1, SET_GET(s.appPrefs.ui_font_scale),
     +[](AppState& s, int i) { s.settings.UiFontSize(i); },
     {{{"small", I::UiFontSmall, 90}, {"default", I::UiFontDefault, 100}, {"large", I::UiFontLarge, 112}, {"larger", I::UiFontLarger, 125}}}},
    {"language", I::SettingsLanguage, 0, 0, -1, SET_GET(LanguageIndex(s.appPrefs.language)),
     +[](AppState& s, int i) { static constexpr const wchar_t* ids[] = {L"system", L"zh-CN", L"zh-TW", L"en-US"}; s.settings.Language(ids[i]); },
     {{{"system", I::LanguageSystem, 0}, {"zh-CN", I::LanguageZhCN, 1}, {"zh-TW", I::LanguageZhTW, 2}, {"en-US", I::LanguageEnUS, 3}}}},
    SET_SW("menu_acrylic", MenuAcrylic, 0, 0, 36, menu_acrylic),
    // 文件列表
    {"row_height", I::SettingsRowHeight, 9, 0, -1, SET_GET(s.appPrefs.row_height),
     +[](AppState& s, int i) { s.settings.RowHeight(i); },
     {{{"compact", I::SettingsDensityCompact, 28}, {"standard", I::SettingsDensityStandard, 34}, {"roomy", I::SettingsDensityRoomy, 40}}}},
    {"click_to_open", I::SettingsClickOpen, 9, 0, -1, SET_GET(s.appPrefs.single_click_open ? 1 : 0),
     +[](AppState& s, int i) { s.settings.ClickMode(i); },
     {{{"double", I::ClickOpenDouble, 0}, {"single", I::ClickOpenSingle, 1}}}},
    {"name_highlight", I::SettingsNameHighlight, 9, 0, -1, SET_GET(s.appPrefs.name_highlight),
     +[](AppState& s, int i) { s.settings.ClickMode(2 + i); },
     {{{"hover", I::NameHighlightHover, 0}, {"always", I::NameHighlightAlways, 1}}}},
    SET_SW("hover_select", SettingsHoverSelect, 9, 0, 77, hover_select),
    SET_SW("item_checkboxes", SettingsItemCheckboxes, 9, 0, 78, item_checkboxes),
    {"blank_double_click", I::SettingsBlankClickBack, 9, 0, -1, SET_GET(s.appPrefs.blank_click_action),
     +[](AppState& s, int i) { s.settings.BlankClick(i); },
     {{{"nothing", I::SettingsBlankClickOff, 0}, {"back", I::Back, 1}, {"up", I::Up, 2}}}},
    SET_SW("show_hidden", SettingsShowHidden, 9, 0, 5, show_hidden_files),
    SET_SW("show_protected", SettingsShowProtected, 9, 0, 16, show_protected_os_files),
    {"folders_first", I::SettingsFolderSort, 9, 0, -1, SET_GET(s.appPrefs.folder_sort_mode),
     +[](AppState& s, int i) { s.settings.FolderSort(i); },
     {{{"top", I::FolderSortTop, 0}, {"follow", I::FolderSortFollow, 1}, {"mixed", I::FolderSortMixed, 2}}}},
    SET_SW("smart_dates", ListSmartDate, 9, 0, 17, list_smart_date),
    SET_SW("zebra_rows", ListZebraRows, 9, 0, 18, list_zebra_rows),
    SET_SW("size_bars", ListSizeBar, 9, 0, 19, list_size_bar),
    SET_SW("tag_name_color", ListTagNameColor, 9, 0, 22, list_tag_name_color),
    SET_SW("selection_outline", ListSelectionOutline, 9, 0, 33, list_selection_outline),
    SET_SW("thumbnail_app_badges", ListThumbnailBadges, 9, 0, 34, list_thumbnail_badges),
    SET_SW("tree_sticky", SettingsTreeSticky, 9, 0, 79, list_tree_sticky),
    {"alias_mark", I::AliasMark, 9, 0, -1, +[](AppState&) -> int { return app::AliasStore::Instance().ShowMark() ? 1 : 0; },
     +[](AppState&, int i) { app::AliasStore::Instance().SetShowMark(i != 0); }, {}},
    {"mtime_marks", I::ListMtimeHint, 9, 0, -1, SET_GET(s.appPrefs.list_mtime_hint),
     +[](AppState& s, int i) { s.settings.MtimeStyle(i); },
     {{{"off", I::MtimeStyleOff, 0}, {"dots", I::MtimeStyleDot, 1}, {"bars", I::MtimeStyleBar, 2}, {"text", I::MtimeStyleText, 3}}}},
    {"mtime_range", I::ListMtimeRange, 9, 0, -1, SET_GET(s.appPrefs.list_mtime_range),
     +[](AppState& s, int i) { s.settings.MtimeRange(i); },
     {{{"today", I::MtimeRangeToday, 1}, {"3", I::MtimeRange3, 3}, {"7", I::MtimeRange7, 7}, {"30", I::MtimeRange30, 30}}}},
    SET_SW("mtime_dim_older", ListMtimeFade, 9, 0, 70, list_mtime_fade),
    SET_SW("mtime_bold_today", ListMtimeBold, 9, 0, 71, list_mtime_bold),
    SET_SW("mtime_folders", ListMtimeTree, 9, 0, 72, list_mtime_tree),
    SET_SW("mtime_other_views", ListMtimeOtherViews, 9, 0, 73, list_mtime_other_views),
    SET_SW("mtime_flash", ListMtimeFlash, 9, 0, 74, list_mtime_flash),
    SET_SW("onedrive_status", SyncOneDrive, 9, 0, 75, sync_onedrive),
    SET_SW("svn_status", SyncSvn, 9, 0, 76, sync_svn),
    // 标签页与窗口
    {"startup_open", I::SettingsStartupOpen, 10, 0, -1, SET_GET(s.appPrefs.startup_open),
     +[](AppState& s, int i) { s.settings.StartupOpen(i); },
     {{{"last_tabs", I::StartupOpenLastTabs, 0}, {"default_location", I::OpenDefaultLocation, 1}}}},
    {"new_tab_open", I::SettingsNewTabOpen, 10, 0, -1, SET_GET(s.appPrefs.new_tab_open),
     +[](AppState& s, int i) { s.settings.NewTabOpen(i); },
     {{{"current", I::NewTabOpenCurrent, 0}, {"default_location", I::OpenDefaultLocation, 1}}}},
    {"external_open", I::SettingsExternalOpen, 10, 0, -1, SET_GET(s.appPrefs.external_open),
     +[](AppState& s, int i) { s.settings.ExternalOpen(i); },
     {{{"smart", I::ExternalOpenSmart, 0}, {"tab", I::ExternalOpenTab, 1}, {"window", I::ExternalOpenWindow, 2}}}},
    SET_SW("win_e_activate", SettingsWinEActivate, 10, 0, 47, win_e_activate_existing),
    SET_SW("vertical_tabs", SettingsVerticalTabs, 10, 0, 23, vertical_tabs),
    SET_SW("pinned_tab_names", PinnedNames, 10, 0, 6, show_pinned_tab_names),
    SET_SW("double_click_closes_tab", SettingsCloseTabDoubleClick, 10, 0, 35, close_tab_on_double_click),
    SET_SW("close_window_with_last_tab", SettingsCloseLastTab, 10, 0, 26, close_window_with_last_tab),
    // 启动与集成
    SET_SW("launch_at_signin", SettingsLaunch, 11, kSetSys, 1, launch_on_startup),
    SET_SW("start_in_tray", SettingsStartInTray, 11, 0, 27, start_in_tray),
    SET_SW("keep_running", SettingsKeepRunning, 11, 0, 2, keep_running_on_close),
    {"tray_icon", I::SettingsNotifyIcon, 11, 0, -1, SET_GET(s.appPrefs.notify_icon_mode),
     +[](AppState& s, int i) { s.settings.NotifyIcon(i); },
     {{{"always", I::NotifyIconAlways, 0}, {"background", I::NotifyIconBackground, 1}, {"never", I::NotifyIconNever, 2}}}},
    SET_SW("explorer_tag_menu", SettingsShellTags, 11, kSetSys, 21, shell_tag_menu),
    // The integration card's master switch (H::SettingsIntegration 0).
    {"open_folders_with_pulse", I::IntegrationMaster, 11, kSetSys, -1, SET_GET(s.appPrefs.integration_enabled),
     +[](AppState& s, int i) { if ((i != 0) != s.appPrefs.integration_enabled) s.settings.IntegrationAction(0); }, {}},
    // 文件操作
    {"staging_icon_size", I::SettingsTrayIcon, 12, 0, -1, SET_GET(s.appPrefs.tray_icon_size),
     +[](AppState& s, int i) { s.settings.TrayIconSize(i); },
     {{{"small", I::SettingsTraySmall, 40}, {"standard", I::SettingsTrayStandard, 48}, {"large", I::SettingsTrayLarge, 56}}}},
    SET_SW("staging_hide_when_empty", TrayHideWhenEmpty, 12, 0, 37, tray_hide_when_empty),
    SET_SW("change_tracking", SettingsChangeTracking, 12, 0, 8, change_tracking_enabled),
    {"change_days", I::SettingsChangeDays, 12, 0, -1, SET_GET(s.appPrefs.change_tracking_days),
     +[](AppState& s, int i) { static constexpr int days[] = {1, 3, 7}; s.settings.ChangeTrackingDays(days[i]); },
     {{{"today", I::ChangeToday, 1}, {"3", I::ChangeLast3Days, 3}, {"7", I::ChangeLast7Days, 7}}}},
    SET_SW("lan_discovery", SettingsLanDiscovery, 12, 0, 40, lan_discovery),
    SET_SW("network_recent", SettingsNetworkRecent, 12, 0, 41, network_recent_enabled),
    SET_SW("hidden_shares", NetworkShowHiddenShares, 12, 0, 42, show_hidden_shares),
    SET_SW("confirm_delete", SettingsConfirmDelete, 12, 0, 32, confirm_recycle_delete),
    // 搜索与索引
    SET_SW("global_search", GlobalSearch, 1, kSetSys, 15, global_search_enabled),
    SET_SW("pinyin_search", SearchPinyin, 1, 0, 9, search_pinyin),
};

#undef SET_SW
#undef SET_GET

} // namespace

std::span<const SetDef> SettingDefs() { return kSettingDefs; }

const SetDef* FindSetting(std::string_view id) {
    for (const SetDef& d : kSettingDefs)
        if (id == d.id) return &d;
    return nullptr;
}

bool IsSwitch(const SetDef& d) { return d.choices[0].key == nullptr; }


std::wstring SettingText(const SetDef& d, int v) {
    if (IsSwitch(d)) return Text(v ? I::AiSetOn : I::AiSetOff);
    for (const SetChoice& c : d.choices)
        if (c.key && c.value == v) return Text(c.label);
    return std::to_wstring(v);
}

std::wstring SettingsPageTitle(int page) {
    switch (page) {
    case 0: return Text(I::SettingsAppearance);
    case 1: return Text(I::SettingsSearchIndex);
    case 9: return Text(I::SettingsFileList);
    case 10: return Text(I::SettingsPageTabs);
    case 11: return Text(I::SettingsPageSystem);
    case 12: return Text(I::SettingsPageFileOps);
    default: return Text(I::SettingsGeneral);
    }
}

void ApplySetting(AppState& s, const SetDef& d, int index) {
    if (IsSwitch(d) && d.toggle >= 0) {
        if ((d.get(s) != 0) != (index != 0)) SetSwitchByClick(s, d.toggle);
    } else {
        d.set(s, index);
    }
}

int ChoiceIndex(const SetDef& d, int v) {
    if (IsSwitch(d)) return v != 0 ? 1 : 0;
    for (size_t i = 0; i < d.choices.size(); ++i)
        if (d.choices[i].key && d.choices[i].value == v) return static_cast<int>(i);
    return -1;
}

} // namespace pulse::app::actions

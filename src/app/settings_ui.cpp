#include "app_internal.h"
#include "../common/windows_compat.h"
#include "../common/localization.h"
#include "../ui/settings_page_ids.h"
#include "about_info.h"
#include "app_hover_preview.h"
#include "../ui/preview_format_catalog.h"
#include "../ui/folder_picker_dialog.h"
#include "../common/preview_packs.h"
#include "system_ctx_model.h"
#include "shortcut_page.h"
#include "content_scope_mode.h"
#include "content_pack_ui.h"
#include "ai_pack_ui.h"
#include "ai_settings_ui.h"
#include "../ui/ui_renderer.h"
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>
#include <cwctype>
#include <cmath>

namespace pulse {
namespace {
using I=l10n::StringId;
using H=ui::HitTestResult;
ui::FluentMenuItem Item(int command,I title,bool checked=false) {
    ui::FluentMenuItem item;item.command=command;item.text=l10n::Get(title);item.checked=checked;
    return item;
}
int Popup(AppState& s,std::vector<ui::FluentMenuItem> items) {
    if(!EnsureMenu(s)) return 0;
    s.menu->SetTheme(s.darkMode,s.accentColor);
    POINT pt{};GetCursorPos(&pt);return s.menu->TrackPopup(pt,std::move(items));
}
int Dropdown(AppState& s, int index, std::vector<ui::FluentMenuItem> items) {
    if(!EnsureMenu(s)) return 0;
    s.menu->SetTheme(s.darkMode,s.accentColor);
    const auto r=s.renderer.SettingsDropdownBounds(BuildVm(s,false),index,
        static_cast<float>(s.compositor.Width()),static_cast<float>(s.compositor.Height()));
    POINT corners[]={{static_cast<LONG>(std::lround(r.left)),static_cast<LONG>(std::lround(r.top))},
        {static_cast<LONG>(std::lround(r.right)),static_cast<LONG>(std::lround(r.bottom))}};
    MapWindowPoints(s.hwnd,nullptr,corners,2);
#ifdef PULSE_WITH_SELFTEST
    wchar_t snapshot[32768]{};
    if(s.isolatedTest && GetEnvironmentVariableW(L"PULSE_TEST_DROPDOWN_SHOT",snapshot,ARRAYSIZE(snapshot))) {
        s.menu->SetDropdownRect({corners[0].x,corners[0].y,corners[1].x,corners[1].y});
        for(auto& item:items) if(item.checked) item.glyph=L"\xE73E";
        s.menu->SaveDebugSnapshot(snapshot,items);
    }
#endif
    return s.menu->TrackDropdown(
        {corners[0].x,corners[0].y,corners[1].x,corners[1].y},std::move(items));
}
struct SettingDestination { I title;int page;unsigned expanded; };
constexpr SettingDestination destinations[]={
    // 0 外观
    {I::SettingsTheme,0,0},{I::SettingsThemeColor,0,0},{I::AccentCustomTitle,0,32},{I::SettingsWindowEffect,0,0},{I::SettingsFrameLook,0,0},{I::MenuAcrylic,0,0},
    {I::SettingsWallpaper,0,0},{I::SettingsWallpaperLook,0,0},{I::SettingsWallpaperBlur,0,0},
    {I::SettingsLanguage,0,0},{I::SettingsTextRender,0,0},{I::SettingsUiFontSize,0,0},
    // 9 文件列表
    {I::SettingsClickOpen,9,0},{I::SettingsItemCheckboxes,9,0},{I::SettingsBlankClickBack,9,0},
    {I::SettingsShowHidden,9,0},{I::SettingsShowProtected,9,0},{I::SettingsFolderSort,9,0},
    {I::SettingsRowHeight,9,0},{I::ListZebraRows,9,0},{I::ListSmartDate,9,0},{I::ListSizeBar,9,0},{I::ListTagNameColor,9,0},
    {I::ListSelectionOutline,9,0},{I::ListThumbnailBadges,9,0},{I::ThumbCache,9,0},{I::AliasMark,9,0},{I::SettingsTreeSticky,9,0},
    {I::ListMtimeHint,9,0},{I::ListMtimeRange,9,0},{I::ListMtimePalette,9,0},{I::SyncOneDrive,9,0},{I::SyncSvn,9,0},
    // 10 标签页与窗口
    {I::SettingsHomeFolder,10,0},{I::SettingsStartupOpen,10,0},{I::SettingsNewTabOpen,10,0},{I::SettingsExternalOpen,10,0},{I::SettingsWinEActivate,10,0},
    {I::SettingsVerticalTabs,10,0},{I::PinnedNames,10,0},{I::SettingsCloseTabDoubleClick,10,0},{I::SettingsCloseLastTab,10,0},
    // 11 启动与集成
    {I::SettingsIntegration,11,0},{I::SettingsDefaultManager,11,0},{I::SettingsOpenFolders,11,0},{I::SettingsWinE,11,0},{I::SettingsThisPc,11,0},
    {I::SettingsRecycleBin,11,0},{I::SettingsExplorerWindows,11,0},
    {I::SettingsLaunch,11,0},{I::SettingsStartInTray,11,0},{I::SettingsKeepRunning,11,0},{I::SettingsNotifyIcon,11,0},{I::SettingsShellTags,11,0},
    // 12 文件操作
    {I::SettingsConfirmDelete,12,0},{I::SettingsTrayIcon,12,0},{I::TrayHideWhenEmpty,12,0},
    {I::SettingsLanDiscovery,12,0},{I::SettingsLanCache,12,0},{I::SettingsNetworkRecent,12,0},{I::NetworkShowHiddenShares,12,0},
    {I::SettingsClearNetwork,12,0},{I::SettingsChangeTracking,12,0},
    {I::GlobalSearch,1,0},{I::GlobalSearchHotkey,1,0},{I::GlobalSearchDoubleCtrl,1,0},{I::GlobalSearchDoubleCtrlGap,1,0},{I::GlobalSearchApps,1,0},{I::DialogJumpBar,1,0},{I::DialogJumpAuto,1,0},{I::SearchPinyin,1,0},{I::JumpHistory,1,0},{I::ContentIndexManage,1,0},{I::IndexLocation,1,2},{I::LocalDrives,1,2},
    {I::Exclusions,1,2},{I::ServerFolders,1,2},{I::SettingsContextMenu,2,0},{I::SettingsDuplicates,4,0},{I::QuickPreview,5,4},{I::PreviewPacks,8,0},
    {I::SettingsAboutDiagnostics,3,0},{I::SettingsShowPerformance,3,0},{I::SettingsHints,3,0},{I::SettingsHintsReset,3,0},{I::SettingsTransfer,3,0},{I::SettingsAutoUpdate,3,0},
};
std::vector<ui::FluentMenuItem> FilterSettings(const std::wstring& query) {
    std::wstring needle=query;std::transform(needle.begin(),needle.end(),needle.begin(),towlower);
    std::vector<ui::FluentMenuItem> items;
    const I pages[]={I::SettingsAppearance,I::SettingsSearchIndex,I::SettingsContextMenu,I::SettingsAboutDiagnostics,I::SettingsDuplicates,I::QuickPreview,
                     I::SettingsContextMenu,I::KeyboardShortcuts,I::PreviewPacks,I::SettingsFileList,I::SettingsPageTabs,
                     I::SettingsPageSystem,I::SettingsPageFileOps};
    for(size_t i=0;i<std::size(destinations);++i) {
        auto item=Item(static_cast<int>(i)+1,destinations[i].title);
        item.shortcut=l10n::Get(pages[destinations[i].page]);
        std::wstring haystack=item.text+L" "+item.shortcut;
        if (destinations[i].page == 5 || destinations[i].page == 8) {
            // Translate each visible heading separately: the Traditional Chinese
            // phrase table deliberately does not translate unknown concatenations.
            const std::wstring aliases[] = {
                l10n::Get(l10n::StringId::SupportedFormats).c_str(),
                l10n::Get(l10n::StringId::PreviewPacks).c_str(),
                l10n::Get(l10n::StringId::SystemExtensions).c_str(),
                l10n::Get(l10n::StringId::ModernImageFormats).c_str(),
                l10n::Get(l10n::StringId::RAWCameraPhotos).c_str(),
                l10n::Get(l10n::StringId::ArcFamArchive).c_str()};
            for (const auto& alias : aliases) haystack += L" " + alias;
        }
        std::transform(haystack.begin(),haystack.end(),haystack.begin(),towlower);
        if(needle.empty() || haystack.find(needle)!=std::wstring::npos) items.push_back(std::move(item));
    }
    if(items.empty()) {auto item=Item(0,I::SettingsNoMatches);item.enabled=false;items.push_back(std::move(item));}
    return items;
}
void FindSetting(AppState& s) {
    if(!EnsureMenu(s)) return;
    s.menu->SetTheme(s.darkMode,s.accentColor);
    s.menu->SetFilterPlaceholder(l10n::Get(I::SettingsFind));
    s.menu->SetFilterMinWidth(420);
    POINT pt{static_cast<LONG>(s.compositor.Width()/2),static_cast<LONG>(s.renderer.TitleBarHeight()+20*s.scale)};
    ClientToScreen(s.hwnd,&pt);
    const int command=s.menu->TrackPopup(pt,FilterSettings(L""),FilterSettings,true);
    s.menu->SetFilterPlaceholder(L"");
    if(command>0 && command<=static_cast<int>(std::size(destinations))) {
        const auto& target=destinations[command-1];
        OpenSettingsTab(s,target.page);s.settingsExpanded|=target.expanded;
        // Scroll to the selected setting using the renderer's shared layout.
        auto vm=BuildVm(s,false);
        const float offset=s.renderer.SettingsDestinationOffset(vm,static_cast<int>(target.title),static_cast<float>(s.compositor.Width()),static_cast<float>(s.compositor.Height()));
        const float maximum=s.renderer.SettingsMaxScroll(vm,static_cast<float>(s.compositor.Width()),static_cast<float>(s.compositor.Height()));
        s.settings.SetScroll(offset,maximum);
    }
}
void ContentOptions(AppState& s) {
    auto config = s.contentSearch.GetConfig();
    if (config.roots.empty()) return;
    const I encodings[]={I::ContentIndexAutoEncoding,I::ContentIndexUtf8Encoding,I::ContentIndexSystemEncoding,I::ContentIndexGbEncoding};
    std::vector<ui::FluentMenuItem> items;
    // Where unscoped searches look (content_scope_mode.h). Only the shared
    // all-drives configuration has a choice; picked roots are the scope.
    if (config.shared_scope) {
        auto zh=[](const wchar_t* chinese,const wchar_t* english) {
            return l10n::IsChinese() ? l10n::HantText(chinese) : std::wstring(english);
        };
        auto scope=[&](int command,std::wstring text,bool selected) {
            ui::FluentMenuItem item;item.command=command;item.text=std::move(text);
            item.radio_group=true;item.radio=selected;items.push_back(std::move(item));
        };
        const bool all=s.appPrefs.content_scope==static_cast<int>(app::ContentScopeMode::AllDrives);
        scope(11,zh(L"搜索个人文件夹（桌面、文档、下载）",L"Search personal folders (Desktop, Documents, Downloads)"),!all);
        scope(12,zh(L"搜索所有驱动器",L"Search all drives"),all);
        items.back().separator_after=true;
    }
    for(int i=0;i<4;++i) items.push_back(Item(i+1,encodings[i],std::all_of(config.roots.begin(), config.roots.end(),
        [&](const auto& root) { return static_cast<int>(root.encoding) == i; })));
    const int command=Popup(s,std::move(items));
    if(command==11 || command==12) {
        s.appPrefs.content_scope=command==11 ? static_cast<int>(app::ContentScopeMode::UserFolders)
                                             : static_cast<int>(app::ContentScopeMode::AllDrives);
        if(!s.isolatedTest) s.appPrefs.Save();
        app::SetContentScopeMode(s.appPrefs.content_scope);
        InvalidateRect(s.hwnd,nullptr,FALSE);
        return;
    }
    if(command<1 || command>4) return;
    config=s.contentSearch.GetConfig();
    config.default_encoding=static_cast<text::Encoding>(command-1);
    for (auto& root : config.roots) root.encoding=static_cast<text::Encoding>(command-1);
    s.contentSearch.Configure(config);
}
// 系统右键菜单 "只在 Pulse 隐藏": ContextMenuPrefs only, Windows is unchanged.
// Queued with the registry switches; SysCtxApply saves it.
void SysCtxTogglePulse(AppState& s,int row) {
    s.settings.SystemCtx().TogglePulseRow(row);
    InvalidateRect(s.hwnd,nullptr,FALSE);
}
// 应用更改: Pulse's own switches go to ContextMenuPrefs (the same calls as the
// Pulse menu's rows, SettingsController::ToggleUi), the registry ones to the job.
void SysCtxApply(AppState& s) {
    auto& model=s.settings.SystemCtx();
    const auto pulse=model.TakePulseChanges();
    if(!pulse.empty()) {
        for(const auto& [key,hidden]:pulse.hidden) s.ctxMenuPrefs.SetItemEnabled(key,!hidden);
        for(const auto& clsid:pulse.skipped) s.ctxMenuPrefs.SetComDisabled(ipc::HandlerCatalogKey(clsid),true);
        s.ctxMenuPrefs.Save();
    }
    model.ApplyPending();
}
// 系统右键菜单 row "…": copy the registry path, open the program's folder,
// hide it in Pulse only, delete a leftover (queued like the switches).
// row -1: the selected entry (details panel).
void SysCtxRowMenu(AppState& s,int row) {
    auto& model=s.settings.SystemCtx();
    const std::wstring key=model.RowRegistryPath(row),file=model.RowFile(row);
    const std::wstring pulse_key=model.RowPulseKey(row);
    std::vector<ui::FluentMenuItem> items;
    auto add=[&](int command,const wchar_t* text,bool enabled) {
        ui::FluentMenuItem item;item.command=command;item.text=text;item.enabled=enabled;items.push_back(std::move(item));
    };
    const size_t slash=file.find_last_of(L"\\/");
    const std::wstring folder=slash==std::wstring::npos ? std::wstring() : file.substr(0,slash);
    add(1,l10n::Get(l10n::StringId::CopyRegistryPath).c_str(),!key.empty());
    add(2,l10n::Get(l10n::StringId::CopyProgramPath).c_str(),!file.empty());
    add(3,l10n::Get(l10n::StringId::OpenItsFolderInANewTab).c_str(),!folder.empty());
    if(!pulse_key.empty()) {
        items.back().separator_after=true;
        add(4,l10n::Get(l10n::StringId::HideInPulseSMenuOnly).c_str(),true);
        items.back().checked=model.RowPulseHidden(row);
    }
    if(model.RowDeletable(row)) {
        items.back().separator_after=true;
        const bool deleting=model.RowDeleting(row);
        add(5,deleting ? l10n::Get(l10n::StringId::KeepIt).c_str()
                       : l10n::Get(l10n::StringId::DeleteLeftoverOnApplyChanges).c_str(),true);
    }
    const int command=Popup(s,std::move(items));
    if(command==1) app::CopyTextToClipboard(s.hwnd,key);
    else if(command==2) app::CopyTextToClipboard(s.hwnd,file);
    else if(command==3 && GetFileAttributesW(folder.c_str())!=INVALID_FILE_ATTRIBUTES) NewTab(s,folder);
    else if(command==4) SysCtxTogglePulse(s,row);
    else if(command==5) {model.DeleteRow(row);InvalidateRect(s.hwnd,nullptr,FALSE);}
}
void SysCtxOpenFolder(AppState& s,const std::wstring& file) {
    const size_t slash=file.find_last_of(L"\\/");
    if(slash==std::wstring::npos) return;
    const std::wstring folder=file.substr(0,slash);
    if(GetFileAttributesW(folder.c_str())!=INVALID_FILE_ATTRIBUTES) NewTab(s,folder);
}
// 全部还原…: confirm in a menu first; it may need one UAC.
void SysCtxRestoreAll(AppState& s) {
    std::vector<ui::FluentMenuItem> items;
    ui::FluentMenuItem item;item.command=1;
    item.text=l10n::Get(l10n::StringId::UndoEveryChangeMadeSincePulseStarted).c_str();
    items.push_back(std::move(item));
    ui::FluentMenuItem cancel;cancel.command=0;cancel.text=l10n::Get(l10n::StringId::Cancel).c_str();items.push_back(std::move(cancel));
    if(Popup(s,std::move(items))==1) s.settings.SystemCtx().RestoreAll();
}
void SysCtxAction(AppState& s,int index) {
    auto& model=s.settings.SystemCtx();
    if(index==ui::kSysCtxLocalLayer) {OpenSettingsTab(s,2);return;}
    if(index==ui::kSysCtxDetailPulse) {SysCtxTogglePulse(s,-1);return;}
    if(index==ui::kSysCtxLog) {
        const std::wstring dir=model.DataDirectory();
        if(!dir.empty() && SHCreateDirectoryExW(nullptr,dir.c_str(),nullptr)<=ERROR_ALREADY_EXISTS) NewTab(s,dir);
        return;
    }
    if(index>=ui::kSysCtxRemoveBase) {
        // A leftover row's own 删除 / 撤回 button: queued like its switch.
        model.DeleteRow(index-ui::kSysCtxRemoveBase);
        InvalidateRect(s.hwnd,nullptr,FALSE);
        return;
    }
    if(index>=ui::kSysCtxSlowSkipBase) {
        // 概览 buttons. Pulse 跳过 only stops Pulse loading the handler; Windows is unchanged.
        if(index>=ui::kSysCtxMarkRestoreBase) model.RestoreMarkRow(index-ui::kSysCtxMarkRestoreBase);
        else if(index>=ui::kSysCtxSlowToggleBase) model.ToggleSlowRow(index-ui::kSysCtxSlowToggleBase);
        else model.QueueSlowSkip(index-ui::kSysCtxSlowSkipBase);  // saved by 应用更改
        InvalidateRect(s.hwnd,nullptr,FALSE);
        return;
    }
    if(index>=ui::kSysCtxPreviewBase) {
        // Live preview item: arm a drag; a release without moving selects it.
        const int item=index-ui::kSysCtxPreviewBase;
        const auto vm=BuildVm(s,false);
        const auto rect=D2D1::RectF(0,0,static_cast<float>(s.compositor.Width()),static_cast<float>(s.compositor.Height()));
        POINT pt{};GetCursorPos(&pt);ScreenToClient(s.hwnd,&pt);
        const float top=s.renderer.SysCtxPreviewItemTop(vm,rect,item);
        if(std::isnan(top)) return;
        model.BeginPreviewDrag(item,static_cast<float>(pt.y),static_cast<float>(pt.y)-top);
        SetCapture(s.hwnd);
        InvalidateRect(s.hwnd,nullptr,FALSE);
        return;
    }
    if(index>=ui::kSysCtxMoreBase) {SysCtxRowMenu(s,index-ui::kSysCtxMoreBase);return;}
    if(index==ui::kSysCtxDetailCopy) {app::CopyTextToClipboard(s.hwnd,model.RowRegistryPath(-1));return;}
    if(index==ui::kSysCtxDetailFolder) {SysCtxOpenFolder(s,model.RowFile(-1));return;}
    if(index==ui::kSysCtxDetailDelete) model.DeleteRow(-1);
    else if(index==ui::kSysCtxClean) model.DeleteSceneLeftovers();
    else if(index==ui::kSysCtxRestoreAll) SysCtxRestoreAll(s);
    else if(index==ui::kSysCtxRefresh) model.Refresh();
    else if(index==ui::kSysCtxApply) SysCtxApply(s);
    else if(index==ui::kSysCtxDiscard) model.DiscardPending();
    else if(index==ui::kSysCtxUndo) model.Undo();
    else if(index==ui::kSysCtxTabPreview || index==ui::kSysCtxTabDetail) model.SetSideTab(index==ui::kSysCtxTabDetail ? 1 : 0);
    else if(index==ui::kSysCtxDetailSwitch) model.ToggleRow(-1);
    else if(index==ui::kSysCtxOverview) model.ShowOverview();
    else if(index>=ui::kSysCtxSelectBase) model.SelectRow(index-ui::kSysCtxSelectBase);
    else if(index>=ui::kSysCtxRowBase) model.ToggleRow(index-ui::kSysCtxRowBase);
    else if(index>=ui::kSysCtxSceneBase) model.SelectScene(index-ui::kSysCtxSceneBase);
    else if(index>=ui::kSysCtxSubBase) model.SelectSub(index-ui::kSysCtxSubBase);
    else if(index>=ui::kSysCtxFilterBase) model.SetFilter(index-ui::kSysCtxFilterBase);
    // Scenes, filters and details change the page height.
    auto vm=BuildVm(s,false);
    const float maximum=s.renderer.SettingsMaxScroll(vm,static_cast<float>(s.compositor.Width()),static_cast<float>(s.compositor.Height()));
    s.settings.SetScroll(s.settings.scroll(),maximum);
    InvalidateRect(s.hwnd,nullptr,FALSE);
}
}

#ifdef PULSE_WITH_SELFTEST
bool TestSettingsFilter(const std::wstring& query,I expected) {
    const auto items=FilterSettings(query);
    for(const auto& item:items) if(item.command>0 && destinations[item.command-1].title==expected) return true;
    return false;
}
#endif

bool HandleSettingsControl(AppState& s,const H& hit) {
    switch(hit.region) {
    case H::SettingsFind: FindSetting(s);break;
    case H::SettingsToggle: {
        // Pulse menu preview: pressing a movable row arms a drag (app_input
        // follows the pointer and drops); the fixed rows do nothing.
        if(hit.index==45) {  // 清除记录 beside 记住常去的位置
            s.core.jumpHistory->Clear();
            InvalidateRect(s.hwnd,nullptr,FALSE);
            return true;
        }
        if(hit.index<ui::kSettingsMenuFixedHit) return false;
        if(hit.index<ui::kSettingsMenuRowHit) return true;
        const auto vm=BuildVm(s,false);
        const auto rect=D2D1::RectF(0,0,static_cast<float>(s.compositor.Width()),static_cast<float>(s.compositor.Height()));
        POINT pt{};GetCursorPos(&pt);ScreenToClient(s.hwnd,&pt);
        const float pos=static_cast<float>(vm.settings_context_tab==2 ? pt.x : pt.y);
        const int item=hit.index-ui::kSettingsMenuRowHit;
        const float start=s.renderer.SettingsMenuRowStart(vm,rect,item);
        if(std::isnan(start)) return true;
        s.settings.BeginMenuDrag(item,pos,pos-start);
        SetCapture(s.hwnd);
        break;
    }
    case H::SettingsDisclosure: {
        if((hit.index<0 || hit.index>3) && (hit.index<8 || hit.index>13)) return true; // 8-13: 右键菜单 cards
        s.settingsExpanded^=1u<<hit.index;
        if (hit.index == 2 && (s.settingsExpanded & 4u)) ui::DetectPreviewCodecs(true, s.hwnd);
        auto vm=BuildVm(s,false);
        const float maximum=s.renderer.SettingsMaxScroll(vm,static_cast<float>(s.compositor.Width()),static_cast<float>(s.compositor.Height()));
        s.settings.SetScroll(s.settings.scroll(),maximum);break;
    }
    case H::SettingsIntegration: s.settings.IntegrationAction(hit.index);break;
    case H::SettingsAccentCustom: {
        // 主题色 › 自定义: start the sliders from the colour in use.
        s.settingsExpanded^=32u;
        if(s.settingsExpanded & 32u) {
            const auto c=s.accentColor;
            const auto channel=[](float v) { return static_cast<uint32_t>(std::lround(std::clamp(v,0.0f,1.0f)*255.0f)); };
            s.settings.SyncCustomAccent((channel(c.r)<<16)|(channel(c.g)<<8)|channel(c.b));
        }
        auto vm=BuildVm(s,false);
        const float maximum=s.renderer.SettingsMaxScroll(vm,static_cast<float>(s.compositor.Width()),static_cast<float>(s.compositor.Height()));
        s.settings.SetScroll(s.settings.scroll(),maximum);break;
    }
    case H::SettingsPreviewStore: ui::OpenPreviewCodecStore(s.hwnd,hit.index);break;
    case H::SettingsSysCtx: SysCtxAction(s,hit.index);break;
    case H::SettingsShortcuts: ShortcutsAction(s,hit.index);break;
    case H::SettingsAi: pulse::AiSettingsAction(s,hit.index);break;
    case H::SettingsPackAction: {
        bool refresh=false;
        switch(static_cast<ui::PackAction>(hit.index)) {
        case ui::PackAction::Install: refresh=s.settings.InstallMediaPack(s.hwnd);break;
        case ui::PackAction::Remove: refresh=s.settings.RemoveMediaPack();break;
        case ui::PackAction::Enable: refresh=s.settings.ToggleMediaPack();break;
        case ui::PackAction::ImagesInstall: refresh=s.settings.InstallImagePack(s.hwnd);break;
        case ui::PackAction::ImagesRemove: refresh=s.settings.RemoveImagePack();break;
        case ui::PackAction::ImagesEnable: refresh=s.settings.ToggleImagePack();break;
        case ui::PackAction::RawInstall: refresh=s.settings.InstallRawPack(s.hwnd);break;
        case ui::PackAction::RawRemove: refresh=s.settings.RemoveRawPack();break;
        case ui::PackAction::RawEnable: refresh=s.settings.ToggleRawPack();break;
        case ui::PackAction::ArchiveInstall: refresh=s.settings.InstallArchivePack(s.hwnd);break;
        case ui::PackAction::ArchiveRemove: refresh=s.settings.RemoveArchivePack();break;
        case ui::PackAction::ArchiveEnable: refresh=s.settings.ToggleArchivePack();break;
        case ui::PackAction::Update: refresh=s.settings.UpdatePack(packs::PackId::Media,s.hwnd);break;
        case ui::PackAction::ImagesUpdate: refresh=s.settings.UpdatePack(packs::PackId::Images,s.hwnd);break;
        case ui::PackAction::RawUpdate: refresh=s.settings.UpdatePack(packs::PackId::Raw,s.hwnd);break;
        case ui::PackAction::ArchiveUpdate: refresh=s.settings.UpdatePack(packs::PackId::Archives,s.hwnd);break;
        case ui::PackAction::UseCustom: refresh=s.settings.ToggleCustomFfmpeg();break;
        case ui::PackAction::UseDetected: refresh=s.settings.UseDetectedFfmpeg();break;
        case ui::PackAction::RemoveOnUninstall: s.settings.ToggleRemovePacksOnUninstall();break;
        case ui::PackAction::AutoUpdate: s.settings.ToggleAutoUpdatePacks();break;
        case ui::PackAction::UpdateAll: {
            const auto state=s.settings.Packs();   // a copy: each update refreshes it
            if(!state.update_version.empty()) refresh|=s.settings.UpdatePack(packs::PackId::Media,s.hwnd);
            if(!state.images_update_version.empty()) refresh|=s.settings.UpdatePack(packs::PackId::Images,s.hwnd);
            if(!state.raw_update_version.empty()) refresh|=s.settings.UpdatePack(packs::PackId::Raw,s.hwnd);
            if(!state.archive_update_version.empty()) refresh|=s.settings.UpdatePack(packs::PackId::Archives,s.hwnd);
            break;
        }
        case ui::PackAction::ManagePacks: OpenSettingsTab(s,8); return true;
        case ui::PackAction::ContentInstall: OfferContentPack(s); return true;
        case ui::PackAction::ContentManage: OpenSettingsTab(s,1); return true;
        case ui::PackAction::AiInstall: OfferAiPack(s); InvalidateRect(s.hwnd,nullptr,FALSE); return true;
        case ui::PackAction::AiRemove: ConfirmRemoveAiPack(s); InvalidateRect(s.hwnd,nullptr,FALSE); return true;
        case ui::PackAction::AiCancel: CancelAiPackInstall(); return true;
        case ui::PackAction::AiManage: OpenSettingsTab(s,ui::kSettingsPageAi); return true;
        case ui::PackAction::TriggerHover:
        case ui::PackAction::TriggerSpace:
            s.settings.PreviewTrigger(hit.index-static_cast<int>(ui::PackAction::TriggerHover));
            if(s.appPrefs.preview_trigger!=app::kPreviewTriggerHover) HideHoverPreview(s);
            break;
        case ui::PackAction::HoverDelayShort:
        case ui::PackAction::HoverDelayMedium:
        case ui::PackAction::HoverDelayLong:
            s.settings.HoverPreviewDelay(hit.index-static_cast<int>(ui::PackAction::HoverDelayShort));break;
        case ui::PackAction::HoverSizeSmall:
        case ui::PackAction::HoverSizeMedium:
        case ui::PackAction::HoverSizeLarge:
            s.settings.HoverCardSize(hit.index-static_cast<int>(ui::PackAction::HoverSizeSmall));break;
        case ui::PackAction::Browse: {
            ui::FolderPickerSpec spec;spec.mode=ui::PickerMode::File;
            spec.title=l10n::Get(l10n::StringId::ChooseFfmpegExe).c_str();
            spec.filters={{L"ffmpeg.exe",L"ffmpeg.exe"}};
            const auto& state=s.settings.Packs();
            const std::wstring& current=!state.custom_path.empty() ? state.custom_path : state.detected_path;
            if(!current.empty()) spec.initial_path=packs::DirectoryOf(current);
            ui::FilePickerResult picked;
            if(ui::ShowFilePicker(s.hwnd,spec,s.darkMode,s.accentColor,picked) && !picked.paths.empty())
                refresh=s.settings.SetCustomFfmpeg(picked.paths.front());
            break;
        }
        case ui::PackAction::OpenFolder: {
            const std::wstring root=s.settings.Packs().root;
            if(!root.empty() && packs::CreateDirectoryChain(root)) NewTab(s,root);
            return true;
        }
        }
        // Thumbnails that failed (or came from the other FFmpeg) are decoded again.
        if(refresh) s.renderer.EvictThumbnails();
        auto vm=BuildVm(s,false);
        const float maximum=s.renderer.SettingsMaxScroll(vm,static_cast<float>(s.compositor.Width()),static_cast<float>(s.compositor.Height()));
        s.settings.SetScroll(s.settings.scroll(),maximum);break;
    }
    case H::SettingsGlobalSearchHotkey: SetFocus(s.hwnd);s.settings.BeginGlobalSearchHotkeyCapture();break;
    case H::SettingsTheme: SetThemeMode(s,hit.index);break;
    case H::SettingsDropdown: {
        std::vector<ui::FluentMenuItem> items;
        if(hit.index==0) {
            const I labels[]={I::EffectNone,I::EffectAcrylic,I::EffectMica,I::EffectMicaAlt};
            for(int i=0;i<ui::kWindowEffectCount;++i) {
                auto item=Item(i+1,labels[i],s.appPrefs.window_effect==ui::WindowEffectId(static_cast<ui::WindowEffect>(i)));
                item.enabled=i==0 || compat::ModernWindows();items.push_back(std::move(item));
            }
            int command=Dropdown(s,hit.index,std::move(items));if(command>=1 && command<=ui::kWindowEffectCount) s.settings.WindowEffect(ui::WindowEffectId(static_cast<ui::WindowEffect>(command-1)));
        } else {
            const I labels[]={I::LanguageSystem,I::LanguageZhCN,I::LanguageZhTW,I::LanguageEnUS};const wchar_t* ids[]={L"system",L"zh-CN",L"zh-TW",L"en-US"};
            constexpr int count=static_cast<int>(std::size(ids));
            for(int i=0;i<count;++i) items.push_back(Item(i+1,labels[i],s.appPrefs.language==ids[i]));
            int command=Dropdown(s,hit.index,std::move(items));if(command>=1 && command<=count) s.settings.Language(ids[command-1]);
        }
        break;
    }
    case H::SettingsContentAction:
        if(hit.index==1 && !s.contentSearch.InstantMode()) s.contentSearch.Pause(!s.contentSearch.GetStatus().paused);
        else if(hit.index==2) ContentOptions(s);
        else if(hit.index==3 && !s.contentSearch.InstantMode() && !s.contentSearch.GetConfig().roots.empty()) s.contentSearch.Rebuild();
        break;
    case H::SettingsAboutAction:
        if(hit.index==0) {
            if(app::CopyTextToClipboard(s.hwnd,app::AboutRowsText(BuildVm(s,false).settings_about_rows)))
                s.renderer.NotifyCopied(static_cast<int>(H::SettingsAboutAction),0);
        } else if(hit.index>=1 && hit.index<=4) {
            static constexpr const wchar_t* kPages[]={app::kPulseHomepage,app::kPulseReleasesPage,
                                                      app::kLumenPdfHomepage,app::kLumaShotHomepage};
            ShellExecuteW(s.hwnd,L"open",kPages[hit.index-1],nullptr,nullptr,SW_SHOWNORMAL);
        }
        break;
    case H::SettingsReleaseNote: {
        s.settingsReleaseExpanded=s.settingsReleaseExpanded==hit.index?-1:hit.index;
        auto vm=BuildVm(s,false);
        const float maximum=s.renderer.SettingsMaxScroll(vm,static_cast<float>(s.compositor.Width()),static_cast<float>(s.compositor.Height()));
        s.settings.SetScroll(s.settings.scroll(),maximum);break;
    }
    default: return false;
    }
    InvalidateRect(s.hwnd,nullptr,FALSE);return true;
}
// 系统右键菜单 live preview drag, from app_input (pointer captured on press).
bool SysCtxPreviewDragMove(AppState& s,int x,int y,WPARAM buttons) {
    (void)x;
    auto& model=s.settings.SystemCtx();
    if(model.PreviewDragItem()<0) return false;
    if(!(buttons&MK_LBUTTON)) {  // capture lost without WM_LBUTTONUP
        model.EndPreviewDrag();
        if(GetCapture()==s.hwnd) ReleaseCapture();
        InvalidateRect(s.hwnd,nullptr,FALSE);
        return true;
    }
    if(model.PreviewDragMove(static_cast<float>(y))) InvalidateRect(s.hwnd,nullptr,FALSE);
    // Captured: no WM_SETCURSOR arrives, so set the drag cursor here.
    if(model.PreviewDragLive()) SetCursor(LoadCursorW(nullptr,IDC_SIZENS));
    return true;
}

bool SysCtxPreviewDragEnd(AppState& s,int x,int y) {
    auto& model=s.settings.SystemCtx();
    const int item=model.PreviewDragItem();
    if(item<0) return false;
    if(model.PreviewDragLive()) {
        const auto vm=BuildVm(s,false);
        const auto rect=D2D1::RectF(0,0,static_cast<float>(s.compositor.Width()),static_cast<float>(s.compositor.Height()));
        const int zone=s.renderer.SysCtxDropZone(vm,rect,static_cast<float>(x),static_cast<float>(y));
        model.EndPreviewDrag();
        if(zone!=ui::kSysCtxZoneNone) model.DropPreview(item,zone);
    } else {
        model.EndPreviewDrag();
        model.SelectPreview(item);
    }
    if(GetCapture()==s.hwnd) ReleaseCapture();
    InvalidateRect(s.hwnd,nullptr,FALSE);
    return true;
}

bool SysCtxPreviewDragCancel(AppState& s) {
    auto& model=s.settings.SystemCtx();
    if(model.PreviewDragItem()<0) return false;
    model.EndPreviewDrag();
    if(GetCapture()==s.hwnd) ReleaseCapture();
    InvalidateRect(s.hwnd,nullptr,FALSE);
    return true;
}
} // namespace pulse

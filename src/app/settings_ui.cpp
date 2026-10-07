#include "app_internal.h"
#include "../common/windows_compat.h"
#include "../common/localization.h"
#include "about_info.h"
#include "../ui/preview_format_catalog.h"
#include "../ui/folder_picker_dialog.h"
#include "../common/preview_packs.h"
#include <shellapi.h>
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
    {I::SettingsTheme,0,0},{I::SettingsThemeColor,0,0},{I::SettingsWindowEffect,0,0},{I::SettingsLanguage,0,0},{I::SettingsTextRender,0,0},{I::SettingsUiFontSize,0,0},
    {I::SettingsIntegration,0,0},{I::SettingsDefaultManager,0,0},{I::SettingsLaunch,0,0},{I::SettingsStartInTray,0,0},{I::SettingsKeepRunning,0,0},{I::SettingsNotifyIcon,0,0},
    {I::SettingsHomeFolder,0,0},{I::SettingsStartupOpen,0,0},{I::SettingsNewTabOpen,0,0},{I::SettingsCloseLastTab,0,0},{I::SettingsCloseTabDoubleClick,0,0},{I::SettingsRowHeight,0,0},{I::SettingsShowPerformance,0,0},
    {I::ListSmartDate,0,0},{I::ListZebraRows,0,0},{I::ListSizeBar,0,0},{I::ListTagNameColor,0,0},{I::ListSelectionOutline,0,0},{I::ListThumbnailBadges,0,0},{I::SettingsFolderSort,0,0},{I::SettingsConfirmDelete,0,0},
    {I::SettingsWallpaper,0,1},{I::SettingsWallpaperLook,0,1},{I::SettingsWallpaperBlur,0,1},{I::SettingsTrayIcon,0,1},{I::SettingsShowHidden,0,1},{I::SettingsShowProtected,0,1},{I::PinnedNames,0,1},{I::SettingsVerticalTabs,0,1},{I::SettingsHints,0,1},{I::SettingsHintsReset,0,1},
    {I::SettingsBlankClickBack,0,1},{I::SettingsChangeTracking,0,1},{I::SettingsOpenFolders,0,0},{I::SettingsWinE,0,0},{I::SettingsThisPc,0,0},{I::SettingsExplorerWindows,0,0},{I::SettingsShellTags,0,1},
    {I::GlobalSearch,1,0},{I::GlobalSearchHotkey,1,0},{I::SearchPinyin,1,0},{I::ContentIndexManage,1,0},{I::IndexLocation,1,2},{I::LocalDrives,1,2},
    {I::Exclusions,1,2},{I::ServerFolders,1,2},{I::SettingsContextMenu,2,0},{I::SettingsDuplicates,4,0},{I::QuickPreview,5,4},{I::SettingsAboutDiagnostics,3,0},{I::SettingsAutoUpdate,3,0},
};
std::vector<ui::FluentMenuItem> FilterSettings(const std::wstring& query) {
    std::wstring needle=query;std::transform(needle.begin(),needle.end(),needle.begin(),towlower);
    std::vector<ui::FluentMenuItem> items;
    const I pages[]={I::SettingsGeneral,I::SettingsSearchIndex,I::SettingsContextMenu,I::SettingsAboutDiagnostics,I::SettingsDuplicates,I::QuickPreview};
    for(size_t i=0;i<std::size(destinations);++i) {
        auto item=Item(static_cast<int>(i)+1,destinations[i].title);
        item.shortcut=l10n::Get(pages[destinations[i].page]);
        std::wstring haystack=item.text+L" "+item.shortcut;
        if (destinations[i].page == 5) {
            // Translate each visible heading separately: the Traditional Chinese
            // phrase table deliberately does not translate unknown concatenations.
            const std::wstring aliases[] = {
                l10n::Pick(L"支持的格式", L"Supported formats"),
                l10n::Pick(L"预览增强包", L"Preview packs"),
                l10n::Pick(L"系统扩展", L"System extensions"),
                l10n::Pick(L"现代图像格式", L"Modern image formats"),
                l10n::Pick(L"RAW 相机照片", L"RAW camera photos"),
                l10n::Pick(L"压缩包", L"Archives")};
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
    for(int i=0;i<4;++i) items.push_back(Item(i+1,encodings[i],std::all_of(config.roots.begin(), config.roots.end(),
        [&](const auto& root) { return static_cast<int>(root.encoding) == i; })));
    const int command=Popup(s,std::move(items));
    if(command<1 || command>4) return;
    config=s.contentSearch.GetConfig();
    config.default_encoding=static_cast<text::Encoding>(command-1);
    for (auto& root : config.roots) root.encoding=static_cast<text::Encoding>(command-1);
    s.contentSearch.Configure(config);
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
    case H::SettingsDisclosure: {
        if((hit.index<0 || hit.index>3) && (hit.index<8 || hit.index>13)) return true; // 8-13: 右键菜单 cards
        s.settingsExpanded^=1u<<hit.index;
        if (hit.index == 2 && (s.settingsExpanded & 4u)) ui::DetectPreviewCodecs(true, s.hwnd);
        auto vm=BuildVm(s,false);
        const float maximum=s.renderer.SettingsMaxScroll(vm,static_cast<float>(s.compositor.Width()),static_cast<float>(s.compositor.Height()));
        s.settings.SetScroll(s.settings.scroll(),maximum);break;
    }
    case H::SettingsIntegration: s.settings.IntegrationAction(hit.index);break;
    case H::SettingsPreviewStore: ui::OpenPreviewCodecStore(s.hwnd,hit.index);break;
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
        case ui::PackAction::UseCustom: refresh=s.settings.ToggleCustomFfmpeg();break;
        case ui::PackAction::UseDetected: refresh=s.settings.UseDetectedFfmpeg();break;
        case ui::PackAction::RemoveOnUninstall: s.settings.ToggleRemovePacksOnUninstall();break;
        case ui::PackAction::Browse: {
            ui::FolderPickerSpec spec;spec.mode=ui::PickerMode::File;
            spec.title=l10n::Pick(L"选择 ffmpeg.exe",L"Choose ffmpeg.exe");
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
} // namespace pulse

#include "app_internal.h"
#include "../common/windows_compat.h"
#include "../common/localization.h"
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
    {I::SettingsTheme,0,0},{I::SettingsThemeColor,0,0},{I::SettingsWindowEffect,0,0},{I::SettingsLanguage,0,0},
    {I::SettingsLaunch,0,0},{I::SettingsKeepRunning,0,0},{I::SettingsRowHeight,0,0},{I::SettingsShowPerformance,0,0},
    {I::ListSmartDate,0,0},{I::ListZebraRows,0,0},{I::ListSizeBar,0,0},
    {I::SettingsWallpaper,0,1},{I::SettingsTrayIcon,0,1},{I::SettingsShowHidden,0,1},{I::SettingsShowProtected,0,1},{I::PinnedNames,0,1},
    {I::SettingsBlankClickBack,0,1},{I::SettingsChangeTracking,0,1},{I::SettingsOpenFolders,0,1},
    {I::GlobalSearch,1,0},{I::GlobalSearchHotkey,1,0},{I::SearchPinyin,1,0},{I::ContentIndexManage,1,0},{I::IndexLocation,1,2},{I::LocalDrives,1,2},
    {I::Exclusions,1,2},{I::ServerFolders,1,2},{I::SettingsContextMenu,2,0},{I::SettingsDuplicates,4,0},{I::SettingsAboutDiagnostics,3,0},
};
std::vector<ui::FluentMenuItem> FilterSettings(const std::wstring& query) {
    std::wstring needle=query;std::transform(needle.begin(),needle.end(),needle.begin(),towlower);
    std::vector<ui::FluentMenuItem> items;
    const I pages[]={I::SettingsGeneral,I::SettingsSearchIndex,I::SettingsContextMenu,I::SettingsAboutDiagnostics,I::SettingsDuplicates};
    for(size_t i=0;i<std::size(destinations);++i) {
        auto item=Item(static_cast<int>(i)+1,destinations[i].title);
        item.shortcut=l10n::Get(pages[destinations[i].page]);
        std::wstring haystack=item.text+L" "+item.shortcut;
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
        if(hit.index!=0 && hit.index!=1 && (hit.index<8 || hit.index>12)) return true;
        s.settingsExpanded^=1u<<hit.index;
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
            const I labels[]={I::LanguageSystem,I::LanguageZhCN,I::LanguageEnUS};const wchar_t* ids[]={L"system",L"zh-CN",L"en-US"};
            for(int i=0;i<3;++i) items.push_back(Item(i+1,labels[i],s.appPrefs.language==ids[i]));
            int command=Dropdown(s,hit.index,std::move(items));if(command>=1 && command<=3) s.settings.Language(ids[command-1]);
        }
        break;
    }
    case H::SettingsContentAction:
        if(hit.index==1 && !s.contentSearch.InstantMode()) s.contentSearch.Pause(!s.contentSearch.GetStatus().paused);
        else if(hit.index==2) ContentOptions(s);
        else if(hit.index==3 && !s.contentSearch.InstantMode() && !s.contentSearch.GetConfig().roots.empty()) s.contentSearch.Rebuild();
        break;
    default: return false;
    }
    InvalidateRect(s.hwnd,nullptr,FALSE);return true;
}
} // namespace pulse

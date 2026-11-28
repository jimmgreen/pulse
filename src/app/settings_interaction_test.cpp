#include "app_internal.h"
#include "../ui/ui_renderer_internal.h"
#include <filesystem>
#include <fstream>

#ifdef PULSE_WITH_SELFTEST
using namespace pulse;
void Render(pulse::AppState&);
int RunSettingsInteractionTest(AppState& s,const wchar_t* output) {
    using H=ui::HitTestResult;using I=l10n::StringId;
    std::ofstream log{std::filesystem::path(output)};int failures=0;
    auto check=[&](bool ok,const char* label) {log<<(ok ? "[PASS] " : "[FAIL] ")<<label<<std::endl;if(!ok)++failures;};
    auto wait=[&](auto predicate) {
        const auto deadline=GetTickCount64()+8000;
        while(GetTickCount64()<deadline) {
            if(predicate()) return true;
            MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {TranslateMessage(&msg);DispatchMessageW(&msg);}
            Sleep(20);
        }
        return predicate();
    };
    s.appPrefs.persist=false;
    const auto base=std::filesystem::path(output).parent_path();
    const auto docs=base/L"Content fixture with a longer folder name";
    std::filesystem::create_directories(docs);
    std::ofstream(docs/L"notes.txt")<<"Pulse settings interaction fixture.";
    index::ContentIndexConfig config;
    config.roots={{docs.wstring(),text::Encoding::Auto},{(base/L"missing-folder").wstring(),text::Encoding::Auto}};
    s.contentSearch.Configure(config);
    check(wait([&]{return s.contentSearch.GetConfig().roots.size()==2;}),"inline content view receives configured roots");
    check(wait([&]{return s.contentSearch.GetStatus().indexed_files>=1;}),"isolated content folder finishes indexing");
    OpenSettingsTab(s,1);s.settingsExpanded=0;s.settings.SetScroll(0,0);
    auto vm=BuildVm(s,false);
    check(vm.settings_content_folders.size()==2 && !vm.settings_content_summary.empty(),"inline summary and folder rows use live index data");
    bool error_visible=false;for(const auto& f:vm.settings_content_folders) error_visible|=f.error;
    check(error_visible,"unavailable content folder has an explicit error state");
    auto snapshot=[&](const wchar_t* name) { Render(s);check(s.compositor.SaveSnapshot((base/name).c_str()),"live content settings snapshot captured"); };
    snapshot(L"content-populated.png");
    H action;action.region=H::SettingsContentAction;action.index=1;
    HandleSettingsControl(s,action);check(wait([&]{return s.contentSearch.GetStatus().paused;}),"inline pause reaches content index agent");
    snapshot(L"content-paused.png");
    HandleSettingsControl(s,action);check(wait([&]{return !s.contentSearch.GetStatus().paused;}),"inline resume reaches content index agent");
    auto select=[&](H control,int down) {
        EnsureMenu(s);s.menu->SetHoverFirstOnOpen(true);
        for(int i=0;i<down;++i) PostThreadMessageW(GetCurrentThreadId(),WM_KEYDOWN,VK_DOWN,0);
        PostThreadMessageW(GetCurrentThreadId(),WM_KEYDOWN,VK_RETURN,0);
        HandleSettingsControl(s,control);
    };
    H dropdown;dropdown.region=H::SettingsDropdown;dropdown.index=1;
    select(dropdown,2);check(s.appPrefs.language==L"zh-TW","language dropdown offers Traditional Chinese after Simplified");
    select(dropdown,3);check(s.appPrefs.language==L"en-US","language dropdown keyboard selection applies immediately");
    dropdown.index=0;select(dropdown,0);check(s.appPrefs.window_effect==ui::WindowEffectId(ui::WindowEffect::None),"window effect dropdown applies selected material");
    action.index=2;select(action,1);
    check(wait([&]{auto c=s.contentSearch.GetConfig();return c.roots.size()==2 &&
        std::all_of(c.roots.begin(),c.roots.end(),[](const auto& root){return root.encoding==text::Encoding::Utf8;});}),
        "text encoding applies to every shared root without changing scope");
    action.index=0;HandleSettingsControl(s,action);
    check((s.settingsExpanded&2u)!=0 && s.contentSearch.GetConfig().roots.size()==2,
        "scope action opens common index maintenance without adding or removing folders");
    s.settingsExpanded=0;s.settings.SetScroll(0,0);
    static std::wstring search_query=l10n::Get(I::SettingsWallpaper);
    s.menu->SetInitialFilterText(L"");
    SetTimer(s.hwnd,0x5346,10,[](HWND hwnd,UINT,UINT_PTR id,DWORD) {
        KillTimer(hwnd,id);
        SetWindowTextW(GetFocus(),search_query.c_str());
        PostThreadMessageW(GetCurrentThreadId(),WM_KEYDOWN,VK_RETURN,0);
    });
    H find;find.region=H::SettingsFind;HandleSettingsControl(s,find);
    vm=BuildVm(s,false);
    log<<"search page="<<vm.settings_page<<" expanded="<<s.settingsExpanded<<" scroll="<<s.settings.scroll()<<std::endl;
    check(vm.settings_page==0 && s.settings.scroll()>0,"settings search opens the option's page and scrolls to it");
    OpenSettingsTab(s,2);s.settings.SetScroll(0,0);
    H expand;expand.region=H::SettingsDisclosure;expand.index=8;
    const bool group_on=s.ctxMenuPrefs.GroupEnabled(ipc::CtxMenuGroup::Software);
    HandleSettingsControl(s,expand);
    check((s.settingsExpanded&(1u<<8)) && s.ctxMenuPrefs.GroupEnabled(ipc::CtxMenuGroup::Software)==group_on,"expanding context group does not toggle its preference");
    // 随机背景图: each settings control dispatches into the controller and lands
    // in the live preferences. The sliders are driven by the drag chain instead.
    s.appPrefs.background_folder=docs.wstring();
    H wall_source;wall_source.region=H::SettingsWallpaperSource;wall_source.index=1;
    check(HandleSettingsControl(s,wall_source) && s.appPrefs.background_source==1,
        "background source row reaches the controller");
    H wall_position;wall_position.region=H::SettingsWallpaperPosition;wall_position.index=0;
    check(HandleSettingsControl(s,wall_position) && s.appPrefs.background_position==0,
        "background position cell reaches the controller");
    H wall_region;wall_region.region=H::SettingsWallpaperRegion;wall_region.index=1;
    check(HandleSettingsControl(s,wall_region) && s.appPrefs.background_region==1,
        "background region row reaches the controller");
    H wall_switch;wall_switch.region=H::SettingsWallpaperSwitch;wall_switch.index=2;
    check(HandleSettingsControl(s,wall_switch) && s.appPrefs.background_switch==2,
        "background switch row reaches the controller");
    H wall_fit;wall_fit.region=H::SettingsWallpaperFit;wall_fit.index=2;
    check(HandleSettingsControl(s,wall_fit) && s.appPrefs.background_fit==2,
        "background fit row reaches the controller");
    s.settings.BackgroundOpacity(55);
    check(s.appPrefs.background_opacity==55,"background opacity value reaches the controller");
    s.settings.BackgroundRatio(30);
    check(s.appPrefs.background_ratio==30,"background ratio value reaches the controller");
    s.contentSearch.Configure(index::ContentIndexConfig{});
    check(wait([&]{return s.contentSearch.GetConfig().roots.empty();}),"isolated content fixture configuration cleared");
    log<<"failures="<<failures<<'\n';return failures ? 1 : 0;
}
#endif

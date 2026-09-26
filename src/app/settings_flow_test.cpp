#include "app_internal.h"
#include "../ui/ui_renderer_internal.h"
#include <fstream>
#include <filesystem>

#ifdef PULSE_WITH_SELFTEST
namespace pulse { bool TestSettingsFilter(const std::wstring&,l10n::StringId); }
using namespace pulse;
int RunSettingsWidthBorderTest(AppState& s, const wchar_t* output) {
    using H = ui::HitTestResult;
    std::ofstream log{std::filesystem::path(output)};
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        log << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
        if (!ok) ++failures;
    };
    check((s.settingsExpanded & ui::kSettingsContextExpandedMask) == ui::kSettingsContextExpandedMask,
        "all context-menu groups start expanded");
    const float original_scale = s.scale;
    for (float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
        s.compositor.RecreateTextFormats(scale);
        s.renderer.SetScale(scale);
        for (float width : {760.0f, 1280.0f, 3045.0f}) {
            auto vm = BuildVm(s, false);
            vm.settings_open = true;
            vm.settings_scroll = 0;
            const auto window = D2D1::RectF(0, 0, width*scale, 1464*scale);
            for (int page = 0; page < 5; ++page) {
                vm.settings_page = page;
                const auto l = ui::MakeSettingsLayout(vm, window, scale,
                    s.renderer.TitleBarHeight(), 28*scale, nullptr);
                check(l.content.left == l.nav.right && l.content.right == window.right,
                    "settings use available width at every window size and DPI");
                const auto hit = s.renderer.HitTest(vm, window,
                    (l.nav_row[page].left+l.nav_row[page].right)/2,
                    (l.nav_row[page].top+l.nav_row[page].bottom)/2);
                check(hit.region == H::SettingsNav && hit.index == page,
                    "page navigation matches the resized layout");
            }
        }
    }
    s.compositor.RecreateTextFormats(original_scale);
    s.renderer.SetScale(original_scale);
    OpenSettingsTab(s, 2);
    const auto group = ipc::CtxMenuGroup::Software;
    const bool enabled = s.ctxMenuPrefs.GroupEnabled(group);
    H hit; hit.region = H::SettingsDisclosure; hit.index = 8;
    HandleSettingsControl(s, hit);
    check(!(s.settingsExpanded & (1u<<8)), "default-expanded group can be collapsed");
    HandleSettingsControl(s, hit);
    check((s.settingsExpanded & ui::kSettingsContextExpandedMask) == ui::kSettingsContextExpandedMask &&
        s.ctxMenuPrefs.GroupEnabled(group) == enabled, "expansion preserves context-menu preferences");

    // Render the real switch painter at fractional offsets and common DPI scales.
    s.compositor.Resize(640, 420);
    auto* dc = s.compositor.Dc();
    dc->BeginDraw();
    const auto theme = ui::MakeTheme(true, s.accentColor);
    dc->Clear(theme.bg);
    ui::fluent::Painter painter(&s.compositor);
    int row = 0;
    for (float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
        s.compositor.RecreateTextFormats(scale);
        painter.SetScale(scale);
        painter.BeginFrame(theme);
        const float top = 20.25f + row*94.0f;
        painter.DrawText(std::to_wstring(static_cast<int>(scale*100)) + L"%",
            D2D1::RectF(16, top, 100, top+40), s.compositor.TextFormat(), theme.text);
        for (int state = 0; state < 4; ++state) {
            ui::fluent::ControlState control{};
            control.checked = state==1;
            control.hovered = state==2;
            control.enabled = state!=3;
            painter.DrawSwitch(D2D1::RectF(120.25f+state*126,top,232.0f+state*126.0f,top+40*scale),L"",control);
        }
        ++row;
    }
    check(SUCCEEDED(dc->EndDraw()), "switch states render without Direct2D errors");
    const auto png = std::filesystem::path(output).parent_path()/L"switch-borders.png";
    check(s.compositor.SaveSnapshot(png.c_str()), "switch border DPI comparison captured");
    log << "failures=" << failures << '\n';
    return failures ? 1 : 0;
}

void Render(pulse::AppState&);
int RunSettingsDropdownTest(AppState& s,const wchar_t* output) {
    using H=ui::HitTestResult;
    std::ofstream log{std::filesystem::path(output)};int failures=0;
    auto check=[&](bool ok,const char* label) {log<<(ok ? "[PASS] " : "[FAIL] ")<<label<<std::endl;if(!ok)++failures;};
    const auto base=std::filesystem::path(output).parent_path();
    s.appPrefs.persist=false;
    OpenSettingsTab(s,0);s.settings.SetScroll(0,0);
    s.settings.WindowEffect(ui::WindowEffectId(ui::WindowEffect::None));
    s.settings.Language(L"en-US");
    Render(s);check(s.compositor.SaveSnapshot((base/L"closed.png").c_str()),"closed dropdown screenshot captured");
    for(int index=0;index<2;++index) {
        const auto vm=BuildVm(s,false);
        const auto control=s.renderer.SettingsDropdownBounds(vm,index,
            static_cast<float>(s.compositor.Width()),static_cast<float>(s.compositor.Height()));
        const auto hit=s.renderer.HitTest(vm,D2D1::RectF(0,0,static_cast<float>(s.compositor.Width()),static_cast<float>(s.compositor.Height())),
            control.left+4*s.scale,(control.top+control.bottom)/2);
        check(hit.region==H::SettingsDropdown && hit.index==index,"dropdown hit target matches the rendered control");
        POINT origin{static_cast<LONG>(std::lround(control.left)),static_cast<LONG>(std::lround(control.bottom))};
        ClientToScreen(s.hwnd,&origin);
        static RECT popup{};popup={};
        static int down=0;down=index;
        const auto png=base/(index==0 ? L"effect-menu.png" : L"language-menu.png");
        SetEnvironmentVariableW(L"PULSE_TEST_DROPDOWN_SHOT",png.c_str());
        SetTimer(s.hwnd,0x5347,350,[](HWND hwnd,UINT,UINT_PTR id,DWORD) {
            KillTimer(hwnd,id);
            EnumThreadWindows(GetCurrentThreadId(),[](HWND window,LPARAM)->BOOL {
                wchar_t title[64]{};GetWindowTextW(window,title,64);
                if(IsWindowVisible(window) && wcscmp(title,L"PulseMenu")==0) GetWindowRect(window,&popup);
                return TRUE;
            },0);
            for(int i=0;i<down;++i) PostThreadMessageW(GetCurrentThreadId(),WM_KEYDOWN,VK_DOWN,0);
            PostThreadMessageW(GetCurrentThreadId(),WM_KEYDOWN,VK_RETURN,0);
        });
        HandleSettingsControl(s,hit);
        SetEnvironmentVariableW(L"PULSE_TEST_DROPDOWN_SHOT",nullptr);
        log<<"control left="<<origin.x<<" bottom="<<origin.y<<" popup="<<popup.left<<","<<popup.top<<","<<popup.right<<","<<popup.bottom<<std::endl;
        check(popup.left+ui::FluentMenu::kShadowMargin==origin.x,"popup left edge aligns with control instead of click position");
        check(popup.top+ui::FluentMenu::kShadowMargin==origin.y+static_cast<int>(std::lround(4*s.scale)),"popup opens below control with a consistent gap");
        check(popup.right-popup.left-2*ui::FluentMenu::kShadowMargin>=static_cast<int>(std::lround(control.right-control.left)),"popup is at least as wide as the control");
    }
    check(s.appPrefs.language==L"zh-CN","language selection still applies");
    check(s.appPrefs.window_effect==ui::WindowEffectId(ui::WindowEffect::None),"window effect selection still applies");
    Render(s);check(s.compositor.SaveSnapshot((base/L"closed-zh.png").c_str()),"Chinese selected value screenshot captured");
    log<<"failures="<<failures<<std::endl;return failures ? 1 : 0;
}

int RunSearchColumnsTest(AppState& s,const wchar_t* output) {
    using H=ui::HitTestResult;
    std::ofstream log{std::filesystem::path(output)};int failures=0;
    auto check=[&](bool ok,const char* label) {log<<(ok ? "[PASS] " : "[FAIL] ")<<label<<std::endl;if(!ok)++failures;};
    const auto base=std::filesystem::path(output).parent_path();
    check((s.settingsExpanded&2u)!=0,"storage and maintenance starts expanded");
    OpenSettingsTab(s,1);
    H disclosure;disclosure.region=H::SettingsDisclosure;disclosure.index=1;
    HandleSettingsControl(s,disclosure);check(!(s.settingsExpanded&2u),"storage can still collapse");
    HandleSettingsControl(s,disclosure);check((s.settingsExpanded&2u)!=0,"storage can reopen");
    Render(s);check(s.compositor.SaveSnapshot((base/L"storage-expanded.png").c_str()),"default expanded storage screenshot captured");
    // Pre-1.0.39 sessions stored divider ratios; they read back as automatic widths.
    const std::array<float,4> saved{0.31f,0.43f,0.62f,0.82f};
    using K=ui::MainRenderer::ColumnKind;
    for(float scale:{1.0f,1.25f,1.5f,2.0f}) {
        s.renderer.SetScale(scale);
        bool compact=true,positive=true,filled=true,dragged=true,legacy=true;
        for(float width:{500.0f,800.0f,1440.0f,2400.0f}) {
            const auto pane=D2D1::RectF(0,0,width*scale,600*scale);
            const auto automatic=s.renderer.DetailsColumns(pane,{},true,{});
            for(const auto& dividers:{std::array<float,4>{},saved}) {
                const auto c=s.renderer.DetailsColumns(pane,{},true,dividers);
                compact &= c.Width(K::Date)<=176*scale+0.01f && c.Width(K::Type)<=196*scale+0.01f && c.Width(K::Size)<=112*scale+0.01f;
                float sum=0;for(int i=0;i<c.count;++i){positive &= c.widths[i]>0;sum+=c.widths[i];}
                filled &= std::abs(sum-(c.right-c.left))<0.05f;
                legacy &= c.count==automatic.count && std::abs(c.widths[0]-automatic.widths[0])<0.05f;
                if(c.Has(K::Path)) {
                    const auto moved=s.renderer.ResizeSearchColumnDivider(pane,dividers,0,c.DividerX(0)-40*scale);
                    const auto after=s.renderer.DetailsColumns(pane,{},true,moved);
                    dragged &= std::abs(after.widths[1]-c.widths[1]-40*scale)<0.05f;
                }
                if(width>=1440) compact &= c.widths[0]+c.widths[1]>c.Width(K::Date)+c.Width(K::Type)+c.Width(K::Size);
            }
        }
        check(compact,"fitted metadata columns stay compact and leave room for name/path");
        check(positive && filled,"columns stay positive and fill the available width");
        check(dragged,"name/path divider drag keeps its requested width");
        check(legacy,"legacy divider ratios fall back to fitted widths");
    }
    s.renderer.SetScale(s.scale);
    ui::WindowViewModel vm;vm.dark=s.darkMode;vm.window_effect=ui::WindowEffect::None;
    vm.tabs.push_back({L"Search results",true});
    const float width=static_cast<float>(s.compositor.Width()),height=static_cast<float>(s.compositor.Height());
    ui::PaneSlotView slot;slot.rect=D2D1::RectF(220*s.scale,100*s.scale,width-8*s.scale,height-32*s.scale);slot.focused=true;
    slot.pane.header_text=L"Search results";slot.pane.is_search=true;slot.pane.search_query=L"setup";
    slot.pane.search_column_dividers=saved;
    for(int i=0;i<5;++i) {
        ui::ListEntryView row;row.name=L"setup-diagnostic-025d13-project-report-"+std::to_wstring(i)+L".log";
        row.path=L"C:\\Program Files\\Pulse\\Diagnostics\\September\\"+row.name;
        row.type_text=L"Text document";row.date_text=L"2026-09-11 12:50";row.size_text=L"539 B";
        slot.pane.entries.push_back(std::move(row));
    }
    vm.pane_slots.push_back(std::move(slot));
    auto* dc=s.compositor.Dc();dc->BeginDraw();
    s.renderer.Render(vm,D2D1::RectF(0,0,width,height),ui::MakeTheme(s.darkMode,s.accentColor));
    check(SUCCEEDED(dc->EndDraw()),"search result columns render successfully");
    check(s.compositor.SaveSnapshot((base/L"search-columns.png").c_str()),"long name/path and compact metadata screenshot captured");
    log<<"failures="<<failures<<std::endl;return failures ? 1 : 0;
}

int RunSettingsFlowTest(AppState& s,const wchar_t* output) {
    if (GetEnvironmentVariableW(L"PULSE_TEST_SEARCH_COLUMNS", nullptr, 0))
        return RunSearchColumnsTest(s, output);
    if (GetEnvironmentVariableW(L"PULSE_TEST_SETTINGS_DROPDOWN", nullptr, 0))
        return RunSettingsDropdownTest(s, output);
    if (GetEnvironmentVariableW(L"PULSE_TEST_SETTINGS_WIDTH_BORDER", nullptr, 0))
        return RunSettingsWidthBorderTest(s, output);
    using H=ui::HitTestResult;using I=l10n::StringId;
    std::ofstream log{std::filesystem::path(output)};int failures=0;
    auto check=[&](bool ok,const char* label){log<<(ok ? "[PASS] " : "[FAIL] ")<<label<<'\n';if(!ok)++failures;};
    s.appPrefs.persist=false;
    app::AppPrefs prefs; prefs.persist=false;
    prefs.FromJson(L"{\"language\":\"en-US\"}");check(prefs.theme_mode==-1,"old profiles retain session theme");
    for(int mode=0;mode<3;++mode) {
        prefs.theme_mode=mode;const auto json=prefs.ToJson();app::AppPrefs loaded;loaded.persist=false;
        check(loaded.FromJson(json) && loaded.theme_mode==mode,"theme preference round trip");
    }
    prefs.FromJson(L"{\"theme_mode\":99}");check(prefs.theme_mode==-1,"invalid theme preference has legacy fallback");
    for(const auto* language:{L"zh-CN",L"en-US"}) {
        l10n::SetLanguage(language);bool complete=true;
        for(int id=1900;id<=1929;++id) complete &= !l10n::Get(static_cast<I>(id)).empty();
        check(complete,"new settings labels exist in both languages");
        check(TestSettingsFilter(l10n::Get(I::SettingsWallpaper),I::SettingsWallpaper),"settings search finds hidden advanced settings");
        check(TestSettingsFilter(l10n::Get(I::SettingsSearchIndex),I::IndexLocation),"settings search finds page and subsettings");
        check(!TestSettingsFilter(L"no-such-setting-123",I::SettingsTheme),"settings search has empty results");
    }
    l10n::SetLanguage(L"zh-CN");
    const float original_scale=s.scale;
    ui::fluent::Painter painter(&s.compositor);
    for(float scale:{1.0f,1.5f,2.0f}) {
        s.compositor.RecreateTextFormats(scale);s.renderer.SetScale(scale);painter.SetScale(scale);
        for(float width:{720.0f,820.0f,1280.0f,1920.0f}) {
            const auto window=D2D1::RectF(0,0,width*scale,1000*scale);
            auto vm=BuildVm(s,false);vm.settings_open=true;vm.settings_scroll=0;
            vm.settings_content_folders={{LR"(C:\Projects\Very long project folder name)",L"Ready",false},{LR"(D:\Unavailable)",L"Unavailable (3)",true}};
            for(int page=0;page<5;++page) {
                vm.settings_page=page;vm.settings_expanded=0;
                const auto layout=ui::MakeSettingsLayout(vm,window,scale,s.renderer.TitleBarHeight(),28*scale,&painter);
                auto hit=[&](D2D1_RECT_F r){return s.renderer.HitTest(vm,window,(r.left+r.right)/2,(r.top+r.bottom)/2);};
                bool nav=true;for(int i=0;i<5;++i) {auto h=hit(layout.nav_row[i]);nav &= h.region==H::SettingsNav && h.index==i;}
                check(nav,"fixed navigation preserves all page destinations across widths and DPI");
                check(layout.nav_row[0].top == layout.nav.top + 20*scale,
                    "settings navigation starts without a search-box gap");
                check(layout.nav_row[4].bottom<layout.nav_row[3].top,"about is pinned below primary navigation");
                if(page==0) {
                    check(hit(layout.theme_tile[0]).region==H::SettingsTheme && hit(layout.theme_tile[0]).index==1,"light theme preview hit target");
                    check(hit(layout.accent_picker).region==H::SettingsAccent,"original color wheel remains interactive");
                    check(hit(layout.effect_choice).region==H::SettingsDropdown && hit(layout.language_choice).index==1,"dropdown controls match layout");
                    check(hit(layout.density_row[2]).region==H::SettingsDensity,"density segments remain reachable");
                    check(layout.wallpaper_card.bottom==0 && layout.startup_row[2].bottom==0,"collapsed advanced settings have no invisible hit targets");
                    for(const auto* locale:{L"zh-CN",L"en-US"}) {
                        l10n::SetLanguage(locale);bool fits=true;
                        const I labels[]={I::SettingsDensityCompact,I::SettingsDensityStandard,I::SettingsDensityRoomy,I::SettingsTraySmall,I::SettingsTrayStandard,I::SettingsTrayLarge};
                        for(I id:labels) {
                            const auto& label=l10n::Get(id);ui::ComPtr<IDWriteTextLayout> measured;
                            s.compositor.DwriteFactory()->CreateTextLayout(label.c_str(),static_cast<UINT32>(label.size()),s.compositor.TextFormat(),2000*scale,100*scale,&measured);
                            DWRITE_TEXT_METRICS metrics{};if(measured.get()) measured->GetMetrics(&metrics);
                            fits &= measured.get() && metrics.widthIncludingTrailingWhitespace<=layout.density_row[0].right-layout.density_row[0].left-6*scale;
                        }
                        check(fits,"density and tray labels fit their segments in both languages");
                    }
                    l10n::SetLanguage(L"zh-CN");
                }
                if(page==2) {
                    bool headers=true;
                    for(int g=0;g<5;++g) {
                        auto h=hit(layout.context_header[g]);headers &= h.region==H::SettingsDisclosure && h.index==g+8;
                        h=hit(layout.context_toggle[g]);headers &= h.region==H::SettingsToggle && h.index==g+10;
                    }
                    check(headers,"context disclosure and group toggle have distinct hit targets");
                    bool hidden=true;for(const auto& r:layout.context_rows) hidden &= r.bottom==0;
                    check(hidden,"collapsed context entries do not receive clicks");
                }
                if(page==1) {
                    check(layout.index_action[0].bottom==0 && layout.index_volume_rows.empty(),"collapsed maintenance has no invisible hit targets");
                    check(hit(layout.disclosure[1]).region==H::SettingsDisclosure,"maintenance disclosure hit target");
                    check(hit(layout.content_header).region!=H::SettingsContentAction,"shared scope header has no maintenance hit target");
                    check(hit(layout.content_options).region==H::SettingsContentAction && hit(layout.content_options).index==2,"shared text encoding hit target");
                    check(layout.content_rebuild.bottom<=layout.group[1].bottom && layout.section[2].bottom==0,"rebuild lives inside content card without a redundant more-options section");
                }
            }
        }
    }
    s.compositor.RecreateTextFormats(original_scale);s.renderer.SetScale(original_scale);painter.SetScale(original_scale);
    OpenSettingsTab(s,0);s.settingsExpanded=0;s.settings.SetScroll(0,0);
    auto window=D2D1::RectF(0,0,static_cast<float>(s.compositor.Width()),static_cast<float>(s.compositor.Height()));
    auto layout=[&] {return ui::MakeSettingsLayout(BuildVm(s,false),window,s.scale,s.renderer.TitleBarHeight(),28*s.scale,&painter);};
    auto click=[&](D2D1_RECT_F r) {
        const int x=static_cast<int>((r.left+r.right)/2),y=static_cast<int>((r.top+r.bottom)/2);
        SendMessageW(s.hwnd,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));
        SendMessageW(s.hwnd,WM_LBUTTONUP,0,MAKELPARAM(x,y));
    };
    click(layout().theme_tile[0]);check(!s.darkMode && s.appPrefs.theme_mode==1,"mouse click applies and saves light theme");
    click(layout().theme_tile[1]);check(s.darkMode && s.appPrefs.theme_mode==2,"mouse click applies and saves dark theme");
    click(layout().theme_tile[2]);check(s.themeOverride==ui::ThemeMode::Auto && s.appPrefs.theme_mode==0,"system theme is a persistent explicit selection");
    click(layout().density_row[0]);check(s.appPrefs.row_height==28,"mouse click changes density through existing controller");
    auto vm=BuildVm(s,false);
    const auto collapsed_max=s.renderer.SettingsMaxScroll(vm,window.right,window.bottom);
    H toggle;toggle.region=H::SettingsDisclosure;toggle.index=0;HandleSettingsControl(s,toggle);
    vm=BuildVm(s,false);const auto expanded_max=s.renderer.SettingsMaxScroll(vm,window.right,window.bottom);
    check((s.settingsExpanded&1u) && expanded_max>collapsed_max,"expanding advanced settings updates scroll range");
    {
        // The advanced group owns both the hidden-files switch and the protected
        // operating system files switch; both need a reachable row and fitting text.
        auto advanced=BuildVm(s,false);advanced.settings_expanded|=1u;advanced.settings_scroll=0;
        const auto lay=ui::MakeSettingsLayout(advanced,window,s.scale,s.renderer.TitleBarHeight(),28*s.scale,&painter);
        check(lay.protected_files_row.top>=lay.hidden_files_row.bottom &&
            lay.protected_files_row.bottom-lay.protected_files_row.top>=64*s.scale-1.0f &&
            lay.protected_files_row.bottom<=lay.footer.top,
            "protected system files row sits between the hidden row and the footer");
        const float text_width=lay.hidden_files_row.right-lay.hidden_files_row.left-126*s.scale;
        for(const auto* locale:{L"zh-CN",L"en-US"}) {
            l10n::SetLanguage(locale);bool fits=true;
            // An out-of-range id resolves to an empty string, which would also fit.
            check(!l10n::Get(I::SettingsShowProtected).empty() &&
                !l10n::Get(I::SettingsShowProtectedDesc).empty(),
                "protected system files labels exist in both languages");
            const I descriptions[]={I::SettingsShowHiddenDesc,I::SettingsShowProtectedDesc};
            for(I id:descriptions) {
                const auto& value=l10n::Get(id);ui::ComPtr<IDWriteTextLayout> measured;
                s.compositor.DwriteFactory()->CreateTextLayout(value.c_str(),static_cast<UINT32>(value.size()),
                    s.compositor.SmallFormat(),4000*s.scale,100*s.scale,&measured);
                DWRITE_TEXT_METRICS metrics{};if(measured.get()) measured->GetMetrics(&metrics);
                fits &= measured.get() && metrics.widthIncludingTrailingWhitespace<=text_width;
            }
            check(fits,"hidden and protected descriptions fit the row in both languages");
        }
        l10n::SetLanguage(L"zh-CN");
        // Scrolled into view, both rows must answer with their own toggle target.
        s.settings.SetScroll(lay.hidden_files_row.top-lay.content.top,10000);
        auto scrolled=BuildVm(s,false);
        const auto visible=ui::MakeSettingsLayout(scrolled,window,s.scale,s.renderer.TitleBarHeight(),28*s.scale,&painter);
        auto row_hit=[&](D2D1_RECT_F r) {
            return s.renderer.HitTest(scrolled,window,(r.left+r.right)/2,(r.top+r.bottom)/2); };
        const auto hidden=row_hit(visible.hidden_files_row);
        const auto guarded=row_hit(visible.protected_files_row);
        check(hidden.region==H::SettingsToggle && hidden.index==5,"hidden files row keeps its toggle target");
        check(guarded.region==H::SettingsToggle && guarded.index==16,"protected system files row exposes its own toggle");
        Render(s);
        check(s.compositor.SaveSnapshot((std::filesystem::path(output).parent_path()/L"settings-advanced.png").c_str()),
            "advanced settings screenshot captured");
    }
    s.settings.SetScroll(expanded_max,expanded_max);HandleSettingsControl(s,toggle);
    check(s.settings.scroll()<=collapsed_max,"collapsing advanced settings clamps existing scroll");
    OpenSettingsTab(s,1);s.settings.SetScroll(0,0);
    const bool before=s.appPrefs.search_pinyin;click(layout().search_pinyin_row);
    check(s.appPrefs.search_pinyin!=before,"mouse click changes pinyin setting");
    toggle.index=1;HandleSettingsControl(s,toggle);vm=BuildVm(s,false);
    const float offset=s.renderer.SettingsDestinationOffset(vm,static_cast<int>(I::IndexLocation),window.right,window.bottom);
    check(offset>0 && (s.settingsExpanded&2u),"search destination locates expanded index storage section");
    log<<"failures="<<failures<<'\n';return failures ? 1 : 0;
}
#endif

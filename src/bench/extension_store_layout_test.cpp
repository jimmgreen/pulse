// Standalone geometry/hit regression test; no AppCore, HWND or installation service.
// Minimum link inputs: this file, fluent_components.cpp, typography.cpp,
// ui_compositor.cpp, lumatext_renderer.cpp (without PULSE_HAS_LUMATEXT), tab_shape.cpp,
// preview_format_catalog.cpp, localization.cpp, zh_hant.cpp,
// text resource tables; /Gy /OPT:REF allows unrelated renderer helpers to be discarded.
// Null painter deliberately tests deterministic geometry, not rasterized visuals.
#include "../common/windows_compat.h"
#include "../ui/ui_renderer_internal.h"
#include <cstdio>

namespace {
using namespace pulse::ui;
int failures=0, cases=0;
void Check(bool ok,const char* name) {
    ++cases; if(!ok) ++failures;
    std::printf("[%s] %s\n",ok?"PASS":"FAIL",name);
}
int Hit(const SettingsLayout& l,const WindowViewModel& vm,D2D1_RECT_F r) {
    return HitTestSettingsStore(l,vm,(r.left+r.right)/2,(r.top+r.bottom)/2);
}
bool Has(D2D1_RECT_F r) { return r.right>r.left && r.bottom>r.top; }
bool Inside(D2D1_RECT_F r,D2D1_RECT_F outer) { return !Has(r) || (r.left>=outer.left-.1f && r.right<=outer.right+.1f && r.top>=outer.top-.1f && r.bottom<=outer.bottom+.1f); }
bool Apart(D2D1_RECT_F a,D2D1_RECT_F b) { return !Has(a)||!Has(b)||a.right<=b.left+.1f||b.right<=a.left+.1f||a.bottom<=b.top+.1f||b.bottom<=a.top+.1f; }
}
int main() {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    using namespace pulse;
    using S=ExtensionStoreView::State;
    l10n::Initialize(GetModuleHandleW(nullptr),L"en-US");
    WindowViewModel vm;
    Check(vm.settings_store_browse && !vm.settings_store_installed,"store defaults to browse only");
    Check(static_cast<int>(PackAction::StoreItemBase)==10000,"item action base is stable");
    Check(static_cast<int>(PackAction::InstalledEnableBase)==20000 && static_cast<int>(PackAction::InstalledDisableBase)==30000 &&
        static_cast<int>(PackAction::InstalledUninstallBase)==40000 && static_cast<int>(PackAction::InstalledRollbackBase)==50000,"installed action bases are stable and disjoint");
    for(const auto* lang:{L"zh-CN",L"zh-TW",L"en-US"}) {
        l10n::SetLanguage(lang);
        Check(!l10n::Get(l10n::StringId::StoreInstalledEmpty).empty() && !store_text::Tab(1).empty(),"installed localized resources are actually linked");
        for(float scale:{1.0f,1.5f}) for(float width:{360.0f,680.0f,1080.0f}) {
            vm.settings_page=8; vm.settings_store_browse=true; vm.settings_store_installed=false;
            vm.settings_ui_font_scale=125;
            auto layout=[&]() {
                SettingsLayout l; l.content=D2D1::RectF(0,0,width*scale,1000*scale);
                l.page_heading=D2D1::RectF(20*scale,16*scale,(width-20)*scale,52*scale);
                LayoutSettingsPacks(l,vm,scale,60*scale,nullptr); return l;
            };
            vm.settings_store_view={};
            vm.settings_store_view.state=S::Online;
            auto empty=layout();
            Check(empty.store_items.empty() && Has(empty.store_empty),"real empty catalog has no fabricated cards");
            Check(Hit(empty,vm,empty.store_tabs[0])==static_cast<int>(PackAction::StoreBrowse),"browse tab is reachable");
            Check(Hit(empty,vm,empty.store_tabs[1])==static_cast<int>(PackAction::StoreInstalled),"installed tab is reachable");
            Check(Hit(empty,vm,empty.store_tabs[2])==static_cast<int>(PackAction::StoreManage),"manage tab is reachable");
            bool tabs_fit=true;
            for (int t=0;t<3;++t) {
                const auto r=empty.store_tabs[t];
                tabs_fit &= Has(r) && r.left>=20*scale && r.right<=(width-20)*scale;
                for (int u=0;u<t;++u) { const auto q=empty.store_tabs[u]; tabs_fit &= r.left>=q.right || r.top>=q.bottom; }
            }
            Check(tabs_fit && empty.store_refresh.top>empty.store_tabs[2].bottom,"three localized tabs flow without overlap above content");
            const int expected_columns=width>=844?3:width>=572?2:1;
            Check(store_view::GridColumns((width-40)*scale,scale)==expected_columns,"official grid uses 260 DIP minimum and twelve DIP gaps");
            if(width>=680) Check(Apart(empty.page_heading,empty.store_search) && empty.store_search.top<empty.store_tabs[0].top,"wide search shares the heading row without overlapping title");
            else Check(empty.store_search.top>=empty.store_tabs[2].bottom,"narrow search keeps its own row below tabs");
            Check(Hit(empty,vm,empty.store_manage)==static_cast<int>(PackAction::StoreManage),"official packs management entry is reachable");
            for(S state:{S::Idle,S::Loading,S::Failed,S::Offline,S::Online}) {
                vm.settings_store_view.state=state;
                vm.settings_store_view.notice=L"A detailed network or verification notice.";
                vm.settings_store_view.pending_restart=true;
                auto l=layout();
                Check(Has(l.store_empty)==(state==S::Online || state==S::Offline),"only a loaded catalog may assert that it is empty");
                Check(Has(l.store_state) && Has(l.store_notice) && Has(l.store_restart),"all catalog states retain status notice and restart message");
                Check(Apart(l.store_restart,l.store_notice) && l.store_manage.top>(std::max)(l.store_restart.bottom,l.store_notice.bottom),"notice restart and management surfaces remain separated in each state");
                Check((Hit(l,vm,l.store_refresh)==static_cast<int>(PackAction::StoreRefresh))==(state!=S::Loading),"refresh disabled while loading only");
            }
            vm.settings_store_view.busy=true;
            vm.settings_store_view.items={
                {L"A real item",L"A long summary provided by the application.",L"Publisher",L"1.0",L"Ready",L"Install",true},
                {L"Disabled item",L"Unavailable release",L"Publisher",L"2.0",L"Incompatible",L"Unavailable",false},
                {L"No action",L"No action label",L"Publisher",L"3.0",L"Installed",L"",true}};
            auto l=layout();
            Check(l.store_items.size()==3 && !Has(l.store_empty),"only supplied items appear in original order");
            Check(Hit(l,vm,l.store_cancel)==static_cast<int>(PackAction::StoreCancel) && Hit(l,vm,l.store_refresh)<0,"busy exposes cancel and disables refresh");
            Check(Hit(l,vm,l.store_items[0].button)==10000,"enabled item emits base plus index");
            Check(Hit(l,vm,l.store_items[1].button)<0,"disabled item never emits a fake click");
            vm.settings_store_view.items[1].action_enabled=true;
            Check(Hit(l,vm,l.store_items[1].button)==10001,"enabled later item preserves catalog index");
            vm.settings_store_view.items[1].action_enabled=false;
            Check(!Has(l.store_items[2].button),"empty action label has no button");
            bool fits=true;
            for(const auto& c:l.store_items) {
                fits &= c.bounds.left>=20*scale && c.bounds.right<=(width-20)*scale &&
                    c.name.bottom<=c.publisher.top && c.publisher.bottom<=c.summary.top &&
                    c.summary.bottom<=(Has(c.status)?c.status.top:c.version.top) &&
                    (!Has(c.status)||c.status.bottom<=c.version.top);
                for(auto r:{c.icon,c.name,c.publisher,c.summary,c.status,c.version,c.button,c.settings}) fits &= Inside(r,c.bounds);
                fits &= Apart(c.version,c.button) && Apart(c.button,c.settings);
            }
            Check(fits,"card fields stay contained and vertically separated with side-by-side footer actions");
            {
                // An installed extension with a settings page leads its section, full width, with 设置.
                auto featured=vm.settings_store_view;
                featured.seven_zip.item=2; featured.seven_zip.available=true;
                featured.items[2].settings_package=0; featured.items[2].official=false;
                featured.items[2].button_label=L"Update"; featured.items[2].action_enabled=true;
                auto fvm=vm; fvm.settings_store_view=featured;
                SettingsLayout f; f.content=D2D1::RectF(0,0,width*scale,1000*scale);
                LayoutSettingsPacks(f,fvm,scale,60*scale,nullptr);
                const auto& c=f.store_feature;
                Check(c.feature && Has(c.settings) && c.bounds.left<=20*scale+0.5f && c.bounds.right>=(width-20)*scale-0.5f,
                      "settings card is full width and offers its settings action");
                Check(Hit(f,fvm,c.settings)==static_cast<int>(PackAction::StoreItemSettingsBase)+2 &&
                      Hit(f,fvm,c.button)==static_cast<int>(PackAction::StoreItemBase)+2,"settings and update actions hit their own targets");
                const bool apart=Apart(c.settings,c.button);
                Check(apart && c.settings.left>=c.bounds.left && c.settings.right<=c.bounds.right && c.settings.bottom<=c.bounds.bottom &&
                      (c.version.right<=c.settings.left+0.5f || c.settings.top>=c.version.bottom),"settings button stays inside the card beside or below the version");
                Check(f.store_items[0].bounds.top>=c.bounds.bottom || f.store_items[0].bounds.bottom<=c.bounds.top,"grid cards do not overlap the featured card");
                Check(Has(f.store_items[2].bounds) && Apart(f.store_items[2].bounds,c.bounds) && !f.store_items[2].feature &&
                    Hit(f,fvm,f.store_items[2].settings)==Hit(f,fvm,c.settings),"featured extension also has a separate grid card sharing the same settings action");
                Check(f.store_trust.top>=c.bounds.bottom && f.store_manage.top>=f.store_trust.bottom,"trust note and official link follow the cards");
            }
            {
                vm.settings_store_view={}; vm.settings_store_view.state=S::Online;
                Check(layout().store_packs.size()==5,"unfiltered browse contains all five real official pack models");
                vm.settings_pack_images_installed=true;
                vm.settings_pack_images_version=L"2026.11-long-installed-version";
                const auto installed_packs=layout();
                bool footer_ok=true;
                for(const auto& pack:installed_packs.store_packs) {
                    footer_ok &= Apart(pack.meta,pack.state)&&Apart(pack.state,pack.secondary)&&Apart(pack.state,pack.button);
                    footer_ok &= Inside(pack.meta,pack.bounds)&&Inside(pack.state,pack.bounds)&&Inside(pack.secondary,pack.bounds);
                }
                Check(footer_ok,"installed long-version pack footer uses one label and never overlaps actions");
                vm.settings_pack_images_installing=true; vm.settings_pack_images_progress=.5f;
                const auto downloading_packs=layout();
                Check(!downloading_packs.store_packs.empty()&&Apart(downloading_packs.store_packs[0].state,downloading_packs.store_packs[0].progress),"downloading footer separates status text from progress track");
                vm.settings_pack_images_installing=false;
                vm.settings_pack_images_installed=false;
                vm.settings_store_view.category=3;
                auto filtered=layout();
                Check(filtered.store_packs.size()==1 && filtered.store_packs[0].pack==3,"search category contains only content search pack");
                for(int chip=0;chip<5;++chip) Check(Hit(filtered,vm,filtered.store_chips[chip])==static_cast<int>(PackAction::StoreChipBase)+chip,"category chip remains reachable");
                vm.settings_store_view.search=L"__no_matching_extension__";
                Check(layout().store_packs.empty(),"unmatched search removes official cards");
                vm.settings_store_view={}; vm.settings_store_view.state=S::Online;
                ExtensionStoreItem item; item.name=L"7-Zip"; item.summary=L"Archive support";
                item.id="pulse.7zip"; item.source=L"https://example.com/very/long/source/location";
                item.settings_package=0; item.version=L"1.0"; item.enabled=true; item.history={L"1.0",L"0.9"};
                {
                    ExtensionStoreView unknown; InstalledExtensionItem installed; installed.id=L"pulse.7zip";
                    unknown.installed.push_back(installed);
                    Check(store_view::AboutValue(unknown,item,1)==l10n::Get(l10n::StringId::OpUnknown),"unknown installed version never becomes latest catalog version or manifest hash");
                }
                vm.settings_store_view.items.push_back(item);
                auto& z=vm.settings_store_view.seven_zip; z.item=0; z.available=true; z.location=1;
                z.menu_available=true; z.formats_available=true;
                auto quick=layout(); const auto& q=quick.store_quick;
                bool quick_fits=q.on;
                for(auto r:{q.title,q.saved,q.mode,q.format,q.level,q.remember,q.preview,q.preview_menu,quick.store_feature.enabled_badge}) quick_fits &= Inside(r,quick.store_feature.bounds);
                quick_fits &= Apart(q.title,q.saved) && Apart(q.format,q.level) && Apart(q.preview,q.rows[2]);
                Check(quick_fits,"quick controls and preview remain contained and separated");
                Check(Hit(quick,vm,q.mode)==static_cast<int>(PackAction::StoreQuickMode) && Hit(quick,vm,q.remember)==static_cast<int>(PackAction::StoreQuickRemember),"quick controls emit precise actions");
                z.installed=true; z.available=false;
                vm.settings_store_view.items[0].settings_package=-1;
                vm.settings_store_view.items[0].enabled=false;
                const auto disabled=layout();
                Check(disabled.store_feature.feature && Hit(disabled,vm,disabled.store_feature.settings)==static_cast<int>(PackAction::StoreItemSettingsBase),"disabled installed 7zip keeps feature and detail entry without a running settings page");
                Check(Hit(disabled,vm,disabled.store_quick.mode)<0,"disabled plugin quick edits remain inactive");
                vm.settings_store_view.detail=true;
                const auto disabled_detail=layout();
                Check(disabled_detail.sz.on && Has(disabled_detail.sz.disabled_banner) && Hit(disabled_detail,vm,disabled_detail.sz.enable)==static_cast<int>(PackAction::StoreDetailEnable),"disabled detail exposes enable without starting the plugin");
                z.available=true; vm.settings_store_view.items[0].enabled=true;
                z.menu_available=false; z.formats_available=false;
                for(int legacy_tab:{1,2}) {
                    vm.settings_store_view.detail_tab=legacy_tab;
                    const auto legacy=layout();
                    Check(!Has(legacy.sz.command_rows[0]) && !Has(legacy.sz.placement_opts[0]) && legacy.sz.chips_rw.empty() && legacy.sz.chips_ro.empty(),
                        "legacy extension does not expose nonfunctional switches or empty format groups");
                    Check(Has(legacy.sz.groups[0]),"legacy extension has a visible capability explanation");
                }
                z.menu_available=true; z.formats_available=true;
                for(int tab=0;tab<4;++tab) {
                    vm.settings_store_view.detail=true; vm.settings_store_view.detail_tab=tab;
                    auto detail=layout(); const auto& d=detail.sz;
                    if(tab==0) Check(!Has(d.saved),"detail settings omit the saved status label");
                    Check(d.on && !Has(detail.store_tabs[0]) && Hit(detail,vm,d.crumb)==static_cast<int>(PackAction::StoreDetailBack),"detail owns its navigation and suppresses browse tabs");
                    bool detail_fits=true;
                    const auto canvas=D2D1::RectF(0,0,width*scale,100000*scale);
                    for(auto r:{d.icon,d.name,d.sub,d.enable,d.enable_label,d.more,d.body,d.side,d.folder,d.browse,d.preview_title,d.level_value}) detail_fits &= Inside(r,canvas);
                    for(auto r:d.badges) detail_fits &= Inside(r,canvas);
                    detail_fits &= Apart(d.enable,d.enable_label) && Apart(d.enable,d.more) && Apart(d.body,d.side) && Apart(d.folder,d.browse) && Apart(d.preview_title,d.preview_kinds[0]) && Apart(d.level_row,d.level_value);
                    Check(detail_fits,"detail header body controls and preview fit without overlap");
                    const auto check_options=[&](const auto& rects,PackAction base) {
                        bool ok=true;
                        for(size_t i=0;i<std::size(rects);++i) if(Has(rects[i])) {
                            ok &= Inside(rects[i],d.body) && Hit(detail,vm,rects[i])==static_cast<int>(base)+static_cast<int>(i);
                            for(size_t j=0;j<i;++j) ok &= Apart(rects[i],rects[j]);
                        }
                        return ok;
                    };
                    Check(check_options(d.mode_opts,PackAction::SzModeBase) && check_options(d.location_opts,PackAction::SzLocationBase) &&
                        check_options(d.format_opts,PackAction::SzFormatBase) && check_options(d.level_stops,PackAction::SzLevelBase) &&
                        check_options(d.placement_opts,PackAction::SzPlacementBase) && check_options(d.command_switch,PackAction::SzCommandBase),"detail option targets remain disjoint contained and correctly indexed");
                    for(int i=0;i<4;++i) Check(Hit(detail,vm,d.tabs[i])==static_cast<int>(PackAction::StoreDetailTabBase)+i,"detail tab remains reachable");
                    Check(Hit(detail,vm,d.enable)==static_cast<int>(PackAction::StoreDetailEnable) && Hit(detail,vm,d.more)==static_cast<int>(PackAction::StoreDetailMore),"detail management controls remain reachable");
                    for(int i=0;i<2;++i) Check(Hit(detail,vm,d.preview_kinds[i])==static_cast<int>(PackAction::SzPreviewBase)+i,"preview segment remains reachable");
                    if(tab==0) {
                        bool stops_hit=true,tar_disabled=true;
                        auto tar_vm=vm; tar_vm.settings_store_view.seven_zip.format=2;
                        for(int stop=0;stop<6;++stop) {
                            const float x=fluent::StepSliderStopX(d.level_track,6,static_cast<float>(stop),scale);
                            const float y=(d.level_track.top+d.level_track.bottom)/2;
                            stops_hit &= HitTestSettingsStore(detail,vm,x,y)==static_cast<int>(PackAction::SzLevelBase)+stop;
                            tar_disabled &= HitTestSettingsStore(detail,tar_vm,x,y)<0;
                        }
                        Check(stops_hit && d.level_track.bottom-d.level_track.top==fluent::StepSliderHeight(scale),"shared six-stop slider geometry agrees with every level hit target");
                        Check(tar_disabled,"tar disables all six shared slider stop targets");
                    }
                    if(tab==1) {
                        bool row_hits=true;
                        for(int i=0;i<6;++i)row_hits &= HitTestSettingsStore(detail,vm,d.command_rows[i].left+20*scale,(d.command_rows[i].top+d.command_rows[i].bottom)/2)==static_cast<int>(PackAction::SzCommandBase)+i;
                        Check(row_hits,"entire command row toggles the corresponding command");
                    }
                    if(tab==2) {
                        bool chips_fit=true;
                        for(auto r:d.chips_rw)chips_fit &= Inside(r,d.groups[0])&&Apart(r,d.formats_rw_label)&&Apart(r,d.formats_ro_label);
                        for(auto r:d.chips_ro)chips_fit &= Inside(r,d.groups[0])&&Apart(r,d.formats_rw_label)&&Apart(r,d.formats_ro_label);
                        Check(chips_fit&&d.formats_ro_label.top>d.formats_rw_label.bottom,"two real format groups and their labels remain separated");
                    }
                    if(tab==3) {
                        bool about_fits=true;
                        for(int i=0;i<6;++i)about_fits &= Inside(d.about_keys[i],d.groups[0])&&Inside(d.about_values[i],d.groups[0])&&Apart(d.about_keys[i],d.about_values[i]);
                        for(size_t i=0;i<d.history_rows.size();++i)about_fits &= Inside(d.history_versions[i],d.history_rows[i])&&Inside(d.history_notes[i],d.history_rows[i])&&Inside(d.history_current[i],d.history_rows[i])&&Apart(d.history_versions[i],d.history_notes[i])&&Apart(d.history_current[i],d.history_notes[i]);
                        Check(about_fits,"about key/value and history columns are separated in narrow and wide layouts");
                        Check(Hit(detail,vm,d.about_source)==static_cast<int>(PackAction::SzSource),"real HTTP source link has a hit target");
                        Check(d.history_rollback.size()>1&&!Has(d.history_rollback[1]),"catalog second release is not fabricated as registry rollback target");
                    }
                    if(tab==0) Check(Hit(detail,vm,d.browse)==static_cast<int>(PackAction::SzBrowse) && Hit(detail,vm,d.remember)==static_cast<int>(PackAction::SzRemember),"fixed folder and remember controls are reachable");
                }
            }
            vm.settings_store_browse=false; vm.settings_store_installed=true;
            vm.settings_store_view={};
            auto installed_empty=layout();
            Check(installed_empty.installed_items.empty() && Has(installed_empty.store_empty),"installed snapshot empty shows real empty state");
            Check(Hit(installed_empty,vm,installed_empty.store_refresh)==static_cast<int>(PackAction::InstalledRefresh),"installed refresh has independent action");
            vm.settings_store_view.installed_loading=true;
            auto installed_loading=layout();
            Check(Has(installed_loading.store_state) && !Has(installed_loading.store_empty) && Hit(installed_loading,vm,installed_loading.store_refresh)<0,"loading is not an asserted empty registry and cannot refresh again");
            vm.settings_store_view.installed_loading=false;
            vm.settings_store_view.installed_error=L"Registry read failed with a detailed diagnostic.";
            vm.settings_store_view.pending_restart=true;
            auto installed_error=layout();
            Check(Has(installed_error.store_state) && Has(installed_error.store_notice) && !Has(installed_error.store_empty) && Has(installed_error.store_restart),"installed failure retains diagnostic and pending restart without false empty");
            Check(installed_error.store_restart.top-installed_error.store_notice.bottom>=19.9f*scale,"installed diagnostic and restart surfaces remain separated");
            vm.settings_store_view.installed={
                {L"Disabled real package",L"org.test.disabled",L"1.0",L"Actual publisher",L"Ready",L"Change is awaiting restart","",false,true,true,false,true,true},
                {L"Enabled real package",L"org.test.enabled",L"2.0",L"Actual publisher",L"Active",L"","",true,true,false,true,true,false},
                {L"Unavailable operations",L"org.test.locked",L"3.0",L"Publisher",L"Locked",L"Cannot change this package","",false,false,false,false,false,false}};
            ExtensionStoreItem installed_catalog;
            installed_catalog.id="org.test.enabled"; installed_catalog.settings_package=0;
            vm.settings_store_view.items.push_back(installed_catalog);
            auto installed=layout();
            Check(installed.installed_items.size()==3 && installed.store_items.empty() && !Has(installed.store_empty),"installed renders supplied registry rows including disabled records, not runtime or catalog");
            const auto& a=installed.installed_items[0]; const auto& b=installed.installed_items[1]; const auto& c=installed.installed_items[2];
            Check(Has(a.badge) && Has(a.enable) && !Has(a.disable) && Has(a.native_code),"disabled package keeps badge and enable action");
            Check(Hit(installed,vm,a.enable)==20000 && Hit(installed,vm,a.more)==80000,"disabled package emits authorized enable and indexed management menu");
            Check(Has(b.disable) && !Has(b.enable) && Hit(installed,vm,b.disable)==30001 && Hit(installed,vm,b.more)==80001,"enabled package uses displayed index and disable action");
            Check(Has(b.settings) && Hit(installed,vm,b.settings)==static_cast<int>(PackAction::StoreItemSettingsBase),"installed settings reuses matching catalog settings action");
            Check(Has(b.more) && Hit(installed,vm,b.more)==80001,"management menu remains available when rollback is unavailable");
            Check(Has(c.enable) && Has(c.more) && Hit(installed,vm,c.enable)<0 && Hit(installed,vm,c.more)==80002 && !Has(c.native_code),"denied switch stays disabled while management menu is separately reachable");
            vm.settings_store_view.installed[0].can_enable=false;
            vm.settings_store_view.installed[0].can_uninstall=false;
            vm.settings_store_view.installed[0].can_rollback=false;
            vm.settings_store_view.installed[1].can_disable=false;
            Check(Hit(installed,vm,a.enable)<0 && Hit(installed,vm,b.disable)<0,"switch authorization flags are rechecked against latest model before emitting");
            bool installed_fits=true;
            for (const auto& row:installed.installed_items) {
                const D2D1_RECT_F fields[]={row.icon,row.name,row.id,row.publisher,row.version,row.badge,row.native_code,row.status,row.notice,row.enable,row.disable,row.more,row.settings};
                for(size_t x=0;x<std::size(fields);++x) {
                    installed_fits &= Inside(fields[x],row.bounds);
                    for(size_t z=0;z<x;++z) installed_fits &= Apart(fields[x],fields[z]);
                }
            }
            Check(installed_fits,"installed wrapped fields and flowing actions are contained and separated");
            vm.settings_store_view.search=L"org.test.enabled";
            auto searched=layout();
            Check(searched.installed_items.size()==3 && !Has(searched.installed_items[0].bounds) && Has(searched.installed_items[1].bounds) && !Has(searched.installed_items[2].bounds) &&
                Hit(searched,vm,searched.installed_items[1].more)==80001,"installed search preserves registry indices and management action mapping");
            vm.settings_store_view.search=L"__absent_installed__";
            Check(Has(layout().store_no_match),"installed unmatched search displays honest no-match state");
            vm.settings_store_view.search.clear();
            Check(a.bounds.top>installed.store_restart.bottom,"restart warning does not overlap installed cards");
            vm.settings_store_view.installed_loading=true;
            auto stale=layout();
            Check(stale.installed_items.size()==3 && Has(stale.store_state) && !Has(stale.store_empty),"loading preserves supplied snapshot with honest loading status");
            vm.settings_store_view.busy=true;
            vm.settings_store_view.installed_loading=false;
            vm.settings_store_view.installed[0].can_enable=true;
            auto network_busy=layout();
            Check(Hit(network_busy,vm,network_busy.store_refresh)==static_cast<int>(PackAction::InstalledRefresh) && Hit(network_busy,vm,network_busy.installed_items[0].enable)==20000,"catalog network busy does not disable local registry management");
            vm.settings_store_view.installed_busy=true;
            auto busy_installed=layout();
            Check(Hit(busy_installed,vm,busy_installed.store_refresh)<0 && Hit(busy_installed,vm,busy_installed.installed_items[0].enable)<0 && Hit(busy_installed,vm,busy_installed.installed_items[0].more)<0,"installed mutation busy disables refresh and operations");
            vm.settings_store_installed=false;
            auto manage=layout();
            Check(manage.pack_native_settings.top>manage.store_tabs[2].bottom && Has(manage.pack_ai_card) &&
                Has(manage.pack_dl_choice) && Has(manage.pack_card) && Has(manage.pack_images_card) && Has(manage.pack_raw_card) &&
                Has(manage.pack_content_card) && Has(manage.pack_ocr_card) && Has(manage.pack_source_group) &&
                Has(manage.pack_auto_update_row) && Has(manage.pack_remove_row) && Has(manage.pack_open) &&
                manage.store_items.empty() && manage.installed_items.empty(),"management keeps full preview native AI source and advanced download layout below all tabs");
            Check(Hit(manage,vm,manage.store_tabs[0])==static_cast<int>(PackAction::StoreBrowse) &&
                Hit(manage,vm,manage.store_tabs[1])==static_cast<int>(PackAction::StoreInstalled) &&
                Hit(manage,vm,manage.store_tabs[2])==static_cast<int>(PackAction::StoreManage),"management retains navigation to all three tabs");
            vm.settings_page=5;
            auto quick=layout(); vm.settings_store_browse=true; auto quick2=layout();
            vm.settings_store_browse=false; vm.settings_store_installed=true; auto quick3=layout();
            Check(!Has(quick.store_tabs[0]) && !Has(quick2.store_tabs[0]) && !Has(quick3.store_tabs[0]) &&
                quick.trigger_row.top==quick2.trigger_row.top && quick.pack_manage.bottom==quick2.pack_manage.bottom &&
                quick.trigger_row.top==quick3.trigger_row.top && quick.pack_manage.bottom==quick3.pack_manage.bottom &&
                Has(quick.pack_manage) && Has(quick.preview_codec_group),"page5 geometry ignores all three store tabs and preserves quick preview entry");
        }
    }
    std::printf("%d cases, %d failures\n",cases,failures);
    return failures?1:0;
}

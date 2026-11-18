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
            Check(Hit(empty,vm,empty.store_manage)==static_cast<int>(PackAction::StoreManage),"official packs management entry is reachable");
            for(S state:{S::Idle,S::Loading,S::Failed,S::Offline,S::Online}) {
                vm.settings_store_view.state=state;
                vm.settings_store_view.notice=L"A detailed network or verification notice.";
                vm.settings_store_view.pending_restart=true;
                auto l=layout();
                Check(Has(l.store_empty)==(state==S::Online || state==S::Offline),"only a loaded catalog may assert that it is empty");
                Check(Has(l.store_state) && Has(l.store_notice) && Has(l.store_restart),"all catalog states retain status notice and restart message");
                Check(l.store_restart.top-l.store_notice.bottom>=19.9f*scale && l.store_manage.top-l.store_restart.bottom>=19.9f*scale,"notice restart and management surfaces retain a visible gap");
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
            for(const auto& c:l.store_items) fits &= c.bounds.left>=20*scale && c.bounds.right<=(width-20)*scale &&
                c.name.bottom<=c.publisher.top && c.publisher.bottom<=c.summary.top && c.summary.bottom<=c.version.top &&
                c.version.bottom<=c.status.top && (!Has(c.button) || (c.status.bottom<=c.button.top && c.button.bottom<c.bounds.bottom));
            Check(fits,"card fields stay contained and vertically separated");
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
                {L"Disabled real package",L"org.test.disabled",L"1.0",L"Actual publisher",L"Ready",L"Change is awaiting restart",false,true,true,false,true,true},
                {L"Enabled real package",L"org.test.enabled",L"2.0",L"Actual publisher",L"Active",L"",true,true,false,true,true,false},
                {L"Unavailable operations",L"org.test.locked",L"3.0",L"Publisher",L"Locked",L"Cannot change this package",false,false,false,false,false,false}};
            auto installed=layout();
            Check(installed.installed_items.size()==3 && installed.store_items.empty() && !Has(installed.store_empty),"installed renders supplied registry rows including disabled records, not runtime or catalog");
            const auto& a=installed.installed_items[0]; const auto& b=installed.installed_items[1]; const auto& c=installed.installed_items[2];
            Check(Has(a.badge) && Has(a.enable) && !Has(a.disable) && Has(a.native_code),"disabled package keeps badge and enable action");
            Check(Hit(installed,vm,a.enable)==20000 && Hit(installed,vm,a.uninstall)==40000 && Hit(installed,vm,a.rollback)==50000,"disabled package emits independent authorized operation bases");
            Check(Has(b.disable) && !Has(b.enable) && Hit(installed,vm,b.disable)==30001 && Hit(installed,vm,b.uninstall)==40001,"enabled package uses displayed index and disable action");
            Check(Has(b.rollback) && Hit(installed,vm,b.rollback)<0,"unavailable rollback is visible but not clickable");
            Check(Has(c.enable) && Has(c.uninstall) && Has(c.rollback) && Hit(installed,vm,c.enable)<0 && Hit(installed,vm,c.uninstall)<0 && Hit(installed,vm,c.rollback)<0 && !Has(c.native_code),"all denied actions remain visible disabled with no hits");
            vm.settings_store_view.installed[0].can_enable=false;
            vm.settings_store_view.installed[0].can_uninstall=false;
            vm.settings_store_view.installed[0].can_rollback=false;
            vm.settings_store_view.installed[1].can_disable=false;
            Check(Hit(installed,vm,a.enable)<0 && Hit(installed,vm,a.uninstall)<0 && Hit(installed,vm,a.rollback)<0 && Hit(installed,vm,b.disable)<0,"all can flags are rechecked against latest model before emitting");
            bool installed_fits=true;
            for (const auto& row:installed.installed_items) {
                const D2D1_RECT_F fields[]={row.name,row.id,row.publisher,row.version,row.badge,row.native_code,row.status,row.notice};
                float previous=row.bounds.top;
                for (const auto r:fields) if (Has(r)) { installed_fits &= r.left>=row.bounds.left && r.right<=row.bounds.right && r.top>=previous; previous=r.bottom; }
                const D2D1_RECT_F actions[]={row.enable,row.disable,row.uninstall,row.rollback};
                for (const auto r:actions) if (Has(r)) installed_fits &= r.top>=previous && r.bottom<row.bounds.bottom && r.left>=row.bounds.left && r.right<=row.bounds.right;
                for (int x=0;x<4;++x) for (int z=0;z<x;++z) if(Has(actions[x]) && Has(actions[z]))
                    installed_fits &= actions[x].left>=actions[z].right || actions[x].top>=actions[z].bottom;
            }
            Check(installed_fits,"installed wrapped fields and flowing actions are contained and separated");
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
            Check(Hit(busy_installed,vm,busy_installed.store_refresh)<0 && Hit(busy_installed,vm,busy_installed.installed_items[0].enable)<0,"installed mutation busy disables refresh and operations");
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

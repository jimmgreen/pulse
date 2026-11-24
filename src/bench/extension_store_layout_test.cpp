// Standalone geometry/hit regression test; no AppCore, HWND or installation service.
// Minimum link inputs: this file, fluent_components.cpp, typography.cpp,
// ui_compositor.cpp, lumatext_renderer.cpp (without PULSE_HAS_LUMATEXT), tab_shape.cpp,
// preview_format_catalog.cpp, localization.cpp, zh_hant.cpp,
// text resource tables; /Gy /OPT:REF allows unrelated renderer helpers to be discarded.
// Null painter deliberately tests deterministic geometry, not rasterized visuals.
#include "../common/windows_compat.h"
#include "../ui/ui_renderer_internal.h"
#include <cstdio>
#include <cstring>
#include "../app/extension_store_item_state.h"

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
int main(int argc, char** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    using namespace pulse;
    using S=ExtensionStoreView::State;
    l10n::Initialize(GetModuleHandleW(nullptr),L"en-US");
    if (argc == 2 && std::strcmp(argv[1], "--status-notices") == 0) {
        using K = fluent::InfoBarKind;
        ExtensionStoreView notices;
        notices.state = S::Offline;
        Check(store_text::Notice(notices, true, false).kind == K::Warning, "offline fallback is a warning");
        notices.notice = L"Installation failed signature validation";
        notices.notice_kind = ExtensionStoreView::NoticeKind::Error;
        Check(store_text::Notice(notices, true, false).kind == K::Error &&
            store_text::Notice(notices, true, false).text == notices.notice,
            "operation failure remains visible and red even with an offline catalog");
        notices.notice = L"Working";
        notices.notice_kind = ExtensionStoreView::NoticeKind::Information;
        Check(store_text::Notice(notices, true, false).kind == K::Informational, "progress uses the information tone");
        notices.notice = L"Installed";
        notices.notice_kind = ExtensionStoreView::NoticeKind::Success;
        Check(store_text::Notice(notices, true, false).kind == K::Success, "installation without restart uses the success tone");
        notices.notice.clear();
        notices.pending_restart = true;
        notices.install_completed = true;
        Check(store_text::Notice(notices, true, false).text.empty() && store_text::RestartNotice(notices).kind == K::Success,
            "completed install needing restart uses one success notice with its action, not an offline duplicate");
        notices.install_completed = false;
        Check(store_text::RestartNotice(notices).kind == K::Warning, "other pending restarts use the warning tone");
        for (float scale : {1.0f, 1.5f}) for (float width : {360.0f, 1080.0f}) {
            for (int page = 0; page < 4; ++page) {
                WindowViewModel fixture;
                fixture.settings_page = 8;
                fixture.settings_ui_font_scale = 125;
                fixture.settings_store_browse = page == 0 || page == 3;
                fixture.settings_store_installed = page == 1;
                auto& view = fixture.settings_store_view;
                view.state = S::Online;
                view.notice = L"A separate installation failed signature validation and must stay visible.";
                if (page == 3) {
                    view.detail = true;
                    view.detail_item = 0;
                    view.items.emplace_back();
                    view.items.back().id = "pulse.7zip";
                    view.items.back().name = L"7-Zip";
                    view.items.back().enabled = false;
                }
                auto layout = [&] {
                    SettingsLayout l;
                    l.content = D2D1::RectF(0, 0, width * scale, 5000 * scale);
                    LayoutSettingsPacks(l, fixture, scale, 60 * scale, nullptr);
                    return l;
                };
                Check(!Has(layout().store_restart_button), "no restart action for a failure alone");
                view.pending_restart = true;
                auto l = layout();
                Check(Has(l.store_restart) && Has(l.store_restart_button) &&
                    Hit(l, fixture, l.store_restart_button) == static_cast<int>(PackAction::StoreRestartNow),
                    "pending restart has one clickable action in every store view, including a disabled package detail");
                Check(Inside(l.store_restart_button, l.store_restart_surface) && Inside(l.store_restart_surface, l.content) &&
                    Apart(l.store_notice_surface, l.store_restart_surface) && Apart(l.store_restart, l.store_restart_button),
                    "restart action fits without covering the notice");
                view.installed_busy = true;
                l = layout();
                Check(Has(l.store_restart_button) && Hit(l, fixture, l.store_restart_button) < 0,
                    "an in-progress mutation keeps the restart button visible but not actionable");
            }
        }
        std::printf("Store status: %d cases, %d failures\n", cases, failures);
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--update-actions") == 0) {
        for (bool available : {false, true}) for (int revision : {0, 1, 2}) {
            ExtensionStoreItem item;
            item.action_enabled = available; item.version = L"catalog"; item.size = L"1 MB";
            app::ApplyInstalledStoreItem(item, revision != 0, revision == 2, L"local");
            Check(item.button_label == l10n::Get(revision == 1 ? l10n::StringId::StoreUpdateItem : l10n::StringId::Installed),
                  "installed label reflects local/current/older revision");
            Check(item.action_enabled == (available && revision == 1), "only available older revisions allow update");
            Check(revision != 0 || (item.version == L"local" && item.size.empty()), "local build keeps its identity");
        }
        for (float scale : {1.0f, 1.5f}) for (float width : {360.0f, 1080.0f}) {
            WindowViewModel vm; vm.settings_page = 8; vm.settings_store_browse = true;
            auto& v = vm.settings_store_view; v.state = S::Online;
            v.items.resize(3);
            v.items[0].id = "community"; v.items[0].name = L"Community";
            v.items[1].id = "pulse.desktop"; v.items[1].name = L"Desktop"; v.items[1].official = true;
            v.items[2].id = "pulse.7zip"; v.items[2].name = L"7-Zip"; v.items[2].official = true;
            v.items[2].category = L"archive"; v.items[2].settings_package = 0;
            { ExtensionQuickCardView q; q.item_index = 2; q.available = true; v.quick_cards.push_back(q); }
            for (auto& item : v.items) { item.button_label = L"Update"; item.action_enabled = true; }
            for (int filter : {0, 1, 2}) {
                v.category = filter == 1 ? 1 : 0; v.search = filter == 2 ? L"Desktop" : L"";
                SettingsLayout l; l.content = D2D1::RectF(0, 0, width*scale, 4000*scale);
                LayoutSettingsPacks(l, vm, scale, 60*scale, nullptr);
                if (filter != 1) Check(Has(l.store_items[1].button) && Hit(l, vm, l.store_items[1].button) == 10001,
                    "desktop update retains catalog index after grouping/search");
                if (filter != 2) {
                    Check(Has(l.store_items[2].button) && Hit(l, vm, l.store_items[2].button) == 10002,
                        "7zip update retains distinct catalog index after grouping/filter");
                    Check(!l.store_generic_quick.empty() && l.store_generic_quick[0].item_index == 2 &&
                        Hit(l, vm, l.store_generic_quick[0].card.button) == 10002,
                        "7zip quick-settings card update uses the same package as its own grid card");
                }
                if (filter == 1) Check(!Has(l.store_items[1].button), "archive filter removes desktop hit rectangle");
                if (filter == 2) Check(!Has(l.store_items[2].button), "desktop search removes 7zip hit rectangle");
            }
        }
        std::printf("Update actions: %d cases, %d failures\n", cases, failures);
        return failures ? 1 : 0;
    }
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
            Check(Hit(l,vm,l.store_items[0].icon)==static_cast<int>(PackAction::StoreItemDetailBase),"clicking store card body opens its detail page");
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
                featured.quick_cards.clear();
                { ExtensionQuickCardView q; q.item_index=2; q.available=true; featured.quick_cards.push_back(q); }
                featured.items[2].settings_package=0; featured.items[2].official=false;
                featured.items[2].button_label=L"Update"; featured.items[2].action_enabled=true;
                auto fvm=vm; fvm.settings_store_view=featured;
                SettingsLayout f; f.content=D2D1::RectF(0,0,width*scale,1000*scale);
                LayoutSettingsPacks(f,fvm,scale,60*scale,nullptr);
                Check(!f.store_generic_quick.empty(),"installed extension with quick settings gets its full-width card");
                static const SettingsLayout::GenericQuickCard none{};
                const auto& g0=f.store_generic_quick.empty()?none:f.store_generic_quick[0];
                const auto& c=g0.card;
                const float card_bottom=(std::max)(c.bounds.bottom,g0.section.bottom);
                Check(c.feature && Has(c.settings) && c.bounds.left<=20*scale+0.5f && c.bounds.right>=(width-20)*scale-0.5f,
                      "settings card is full width and offers its settings action");
                Check(Hit(f,fvm,c.settings)==static_cast<int>(PackAction::StoreItemSettingsBase)+2 &&
                      Hit(f,fvm,c.button)==static_cast<int>(PackAction::StoreItemBase)+2,"settings and update actions hit their own targets");
                const bool apart=Apart(c.settings,c.button);
                Check(apart && c.settings.left>=c.bounds.left && c.settings.right<=c.bounds.right && c.settings.bottom<=c.bounds.bottom &&
                      (c.version.right<=c.settings.left+0.5f || c.settings.top>=c.version.bottom),"settings button stays inside the card beside or below the version");
                Check(f.store_items[0].bounds.top>=card_bottom || f.store_items[0].bounds.bottom<=c.bounds.top,"grid cards do not overlap the featured card");
                Check(Has(f.store_items[2].bounds) && Apart(f.store_items[2].bounds,c.bounds) && !f.store_items[2].feature &&
                    Hit(f,fvm,f.store_items[2].settings)==Hit(f,fvm,c.settings),"featured extension also has a separate grid card sharing the same settings action");
                Check(f.store_trust.top>=card_bottom && f.store_manage.top>=f.store_trust.bottom,"trust note and official link follow the cards");
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
                // 7-Zip has no host-specific store surfaces: its quick-settings card and detail
                // page are the generic ones, fed by its manifest quickSettings menu preview.
                vm.settings_store_view.items[0].installed=true;
                {
                    ExtensionQuickCardView q; q.item_index=0; q.available=true; q.has_menu_preview=true;
                    q.preview_caption=L"Right-click an archive";
                    q.preview_rows={{L"Smart extract",L"Single item directly, several into a folder",0},{L"Extract to...",L"",0}};
                    vm.settings_store_view.quick_cards.push_back(q);
                }
                auto quick=layout();
                Check(!quick.store_generic_quick.empty(),"7zip uses the generic quick-settings card");
                if(!quick.store_generic_quick.empty()) {
                    const auto& g=quick.store_generic_quick[0];
                    const bool split=width>=1000.0f; // only the wide layout is guaranteed a side preview
                    Check(g.card.feature && Inside(g.preview_menu,g.preview) && Inside(g.preview_caption,g.preview) && (!split || Has(g.preview_menu)),
                        "generic quick card keeps its menu preview contained");
                    Check(Hit(quick,vm,g.card.settings)==static_cast<int>(PackAction::StoreItemSettingsBase),"quick card settings action targets 7zip");
                }
                vm.settings_store_view.quick_cards[0].available=false;
                vm.settings_store_view.items[0].settings_package=-1;
                vm.settings_store_view.items[0].enabled=false;
                vm.settings_store_view.detail=true; vm.settings_store_view.detail_item=0;
                const auto disabled_detail=layout();
                Check(disabled_detail.ext_detail.on && Hit(disabled_detail,vm,disabled_detail.ext_detail.enable)==static_cast<int>(PackAction::StoreDetailEnable),
                    "disabled 7zip detail is the generic page and exposes enable without starting the plugin");
                Check(!Has(disabled_detail.ext_detail.showcase_card),"no menu showcase while the plugin is not running");
                vm.settings_store_view.quick_cards[0].available=true;
                vm.settings_store_view.items[0].settings_package=0;
                vm.settings_store_view.items[0].enabled=true;
                for(int tab=0;tab<2;++tab) {
                    vm.settings_store_view.detail_tab=tab;
                    auto detail=layout(); const auto& d=detail.ext_detail;
                    Check(d.on && !Has(detail.store_tabs[0]) && Hit(detail,vm,d.crumb)==static_cast<int>(PackAction::StoreDetailBack),"detail owns its navigation and suppresses browse tabs");
                    const auto canvas=D2D1::RectF(0,0,width*scale,100000*scale);
                    bool detail_fits=true;
                    for(auto r:{d.icon,d.name,d.sub,d.enable,d.enable_label,d.more,d.body,d.side}) detail_fits &= Inside(r,canvas);
                    detail_fits &= Apart(d.enable,d.enable_label) && Apart(d.enable,d.more) && Apart(d.body,d.side);
                    Check(detail_fits,"generic detail header and body fit without overlap");
                    for(int i=0;i<d.tab_count && i<4;++i) Check(Hit(detail,vm,d.tabs[i])==d.tab_actions[i],"detail tab remains reachable");
                    if(tab==0) {
                        Check(Has(d.showcase_card) && Inside(d.showcase_menu,d.showcase_card) && Inside(d.showcase_caption,d.showcase_card) &&
                            Inside(d.showcase_card,d.desc_card) && Apart(d.showcase_card,d.desc_text) && Apart(d.showcase_card,d.desc_label) && Inside(d.showcase_card,d.body),
                            "plugin menu showcase sits on its stage in the introduction card without covering the text");
                    } else {
                        bool about_fits=true;
                        for(int i=0;i<6;++i)about_fits &= Apart(d.about_keys[i],d.about_values[i]) || d.about_values[i].top>=d.about_keys[i].bottom;
                        for(size_t i=0;i<d.history_rows.size();++i)about_fits &= Inside(d.history_versions[i],d.history_rows[i])&&Inside(d.history_notes[i],d.history_rows[i]);
                        Check(about_fits,"about key/value and history columns are separated in narrow and wide layouts");
                        Check(Hit(detail,vm,d.about_source)==static_cast<int>(PackAction::SzSource),"real HTTP source link has a hit target");
                    }
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

            // Generic third-party showcase/detail layout & hit targets (uninstalled & installed).
            for (bool ext_installed : {false, true}) for (bool has_menu : {false, true}) {
                if (!ext_installed && has_menu) continue;
                vm.settings_page = 8;
                vm.settings_store_browse = true;
                vm.settings_store_installed = false;
                vm.settings_store_view = {};
                vm.settings_store_view.state = S::Online;
                vm.settings_store_view.detail = true;
                vm.settings_store_view.detail_item = 0;
                ExtensionStoreItem ext;
                ext.id = "vendor.demo";
                ext.name = L"Vendor Demo Extension";
                ext.summary = L"Short summary shown in detail header.";
                ext.description = L"First paragraph of the showcase.\nSecond paragraph explaining workflow.";
                ext.publisher = L"Vendor Studio";
                ext.version = L"2.4.0";
                ext.size = L"1.8 MB";
                ext.engine = L"VendorCore 3.1";
                ext.license = L"MIT";
                ext.homepage = L"https://example.com/vendor";
                ext.source = L"https://github.com/example/vendor";
                ext.tags = {L"batch", L"archive", L"fast"};
                ext.highlights = {L"Zero-copy pipeline with isolated worker", L"Supports undo via Pulse staging"};
                ext.permissions = {L"Reads selected files", L"Owns plugin settings"};
                ext.releases = {{L"2.4.0", L"2026-11-20", L"Added showcase metadata", L"1.0.61"},
                                {L"2.3.0", L"2026-10-01", L"Initial public release", L"1.0.60"}};
                ext.installed = ext_installed;
                ext.installed_index = ext_installed ? 0 : -1;
                ext.enabled = ext_installed;
                ext.settings_package = ext_installed ? 0 : -1;
                ext.has_menu_tab = ext_installed && has_menu;
                ext.button_label = ext_installed ? L"Update" : L"Install";
                ext.action_enabled = true;
                vm.settings_store_view.items.push_back(std::move(ext));
                if (ext_installed) {
                    InstalledExtensionItem inst;
                    inst.id = L"vendor.demo"; inst.name = L"Vendor Demo Extension";
                    inst.version = L"2.4.0"; inst.enabled = true;
                    vm.settings_store_view.installed.push_back(std::move(inst));
                }
                const int expected_tabs = !ext_installed ? 2 : (has_menu ? 4 : 3);
                for (int tab : {0, 1}) {
                    vm.settings_store_view.detail_tab = tab;
                    auto dl = layout();
                    const auto& ed = dl.ext_detail;
                    const auto canvas = D2D1::RectF(0, 0, width*scale, 100000*scale);
                    bool ok = ed.on && ed.tab_count == expected_tabs;
                    ok &= Inside(ed.crumb, canvas) && Inside(ed.icon, canvas) && Inside(ed.name, canvas) &&
                          Inside(ed.sub, canvas) && Inside(ed.install_button, canvas);
                    ok &= Apart(ed.icon, ed.name) && Apart(ed.icon, ed.sub) && Apart(ed.name, ed.sub);
                    ok &= Hit(dl, vm, ed.crumb) == static_cast<int>(PackAction::StoreDetailBack);
                    ok &= Hit(dl, vm, ed.install_button) == static_cast<int>(PackAction::StoreDetailInstall);
                    if (ext_installed) {
                        ok &= Inside(ed.enable_label, canvas) && Inside(ed.enable, canvas) && Inside(ed.more, canvas);
                        ok &= Apart(ed.install_button, ed.enable_label) && Apart(ed.enable_label, ed.enable) && Apart(ed.enable, ed.more);
                        ok &= Hit(dl, vm, ed.enable) == static_cast<int>(PackAction::StoreDetailEnable);
                        ok &= Hit(dl, vm, ed.more) == static_cast<int>(PackAction::StoreDetailMore);
                    } else {
                        ok &= !Has(ed.enable) && !Has(ed.more);
                    }
                    for (int t = 0; t < ed.tab_count; ++t) {
                        ok &= Inside(ed.tabs[t], canvas) && Hit(dl, vm, ed.tabs[t]) == ed.tab_actions[t];
                        for (int u = 0; u < t; ++u) ok &= Apart(ed.tabs[t], ed.tabs[u]);
                        if (ext_installed) ok &= ed.tabs[t].top >= ed.more.bottom;
                    }
                    if (tab == 0) {
                        ok &= Inside(ed.desc_card, ed.body) && Inside(ed.desc_text, ed.desc_card);
                        ok &= Inside(ed.highlights_card, ed.body) && ed.highlight_rows.size() == 2;
                        ok &= Inside(ed.tags_card, ed.body) && ed.tag_chips.size() == 3;
                        for (const auto& chip : ed.tag_chips) ok &= Inside(chip, ed.tags_card);
                        ok &= Inside(ed.facts_card, ed.side) && Inside(ed.perms_card, ed.side) && Apart(ed.facts_card, ed.perms_card);
                        if (width >= 1080) ok &= Apart(ed.body, ed.side);
                        ok &= Hit(dl, vm, ed.fact_homepage) == static_cast<int>(PackAction::StoreDetailHomepage);
                        ok &= Hit(dl, vm, ed.fact_source) == static_cast<int>(PackAction::SzSource);
                    } else {
                        ok &= Inside(ed.groups[0], ed.body) && Inside(ed.history_card, ed.body) && ed.history_rows.size() == 2;
                        for (int r = 0; r < 6; ++r)
                            ok &= Inside(ed.about_keys[r], ed.groups[0]) && Inside(ed.about_values[r], ed.groups[0]) &&
                                  Apart(ed.about_keys[r], ed.about_values[r]);
                        ok &= Hit(dl, vm, ed.about_source) == static_cast<int>(PackAction::SzSource);
                    }
                    Check(ok, "generic third-party detail geometry and hits remain non-overlapping and complete");
                }
            }
        }
        {
            vm.settings_store_view = {};
            vm.settings_store_view.state = S::Online;
            ExtensionStoreItem custom;
            custom.name = L"Acme Archiver";
            custom.summary = L"Fast archival";
            custom.id = "acme.archiver";
            custom.category = L"archive";
            custom.settings_package = 1;
            custom.version = L"2.0";
            custom.enabled = true;
            custom.installed = true;
            vm.settings_store_view.items.push_back(custom);

            ExtensionQuickCardView qc;
            qc.item_index = 0;
            qc.package_index = 1;
            qc.package_id = "acme.archiver";
            qc.title = L"Quick Setup";
            qc.available = true;
            qc.has_menu_preview = true;
            qc.preview_caption = L"Menu Preview";
            qc.preview_rows = {{L"Extract Here", L"Extract archive here", 0, StoreIcon::Box}};

            ExtensionQuickRowView r0;
            r0.title = L"Auto Mode";
            r0.description = L"Behavior on open";
            ExtensionQuickControlView c_sel;
            c_sel.kind = 1;
            c_sel.action = static_cast<int>(PackAction::StoreQuickControlBase) + 0;
            c_sel.label = L"Smart";
            c_sel.width_dip = 140;
            r0.controls.push_back(c_sel);

            ExtensionQuickRowView r1;
            r1.title = L"Preset";
            ExtensionQuickControlView c_dis;
            c_dis.kind = 1;
            c_dis.action = static_cast<int>(PackAction::StoreQuickControlBase) + 1;
            c_dis.label = L"None";
            c_dis.disabled = true;
            c_dis.width_dip = 104;
            ExtensionQuickControlView c_sw;
            c_sw.kind = 0;
            c_sw.action = static_cast<int>(PackAction::StoreQuickControlBase) + 2;
            c_sw.checked = true;
            r1.controls.push_back(c_dis);
            r1.controls.push_back(c_sw);

            ExtensionQuickRowView r2;
            r2.title = L"Tools";
            ExtensionQuickControlView c_btn;
            c_btn.kind = 2;
            c_btn.action = static_cast<int>(PackAction::StoreQuickControlBase) + 3;
            c_btn.label = L"Open Folder";
            c_btn.width_dip = 112;
            r2.controls.push_back(c_btn);

            qc.rows = {r0, r1, r2};
            vm.settings_store_view.quick_cards.push_back(qc);

            for (const float scale : {1.0f, 1.5f}) {
                for (const float w : {1120.0f, 520.0f}) {
                    vm.settings_page = 8;
                    vm.settings_store_browse = true;
                    vm.settings_store_installed = false;
                    SettingsLayout gq_layout;
                    gq_layout.content = D2D1::RectF(0, 0, w * scale, 1000 * scale);
                    gq_layout.page_heading = D2D1::RectF(20 * scale, 16 * scale, (w - 20) * scale, 52 * scale);
                    LayoutSettingsPacks(gq_layout, vm, scale, 60 * scale, nullptr);
                    bool ok = gq_layout.store_generic_quick.size() == 1;
                    if (ok) {
                        const auto& gq = gq_layout.store_generic_quick[0];
                        ok &= gq.card.feature && Inside(gq.title, gq.card.bounds) && Inside(gq.saved, gq.card.bounds);
                        ok &= Apart(gq.title, gq.saved) && gq.rows.size() == 3 && gq.controls.size() == 4;
                        ok &= Inside(gq.preview_menu, gq.card.bounds) && Apart(gq.preview, gq.rows[2]);
                        for (size_t ci = 0; ci < gq.controls.size(); ++ci) {
                            ok &= Inside(gq.controls[ci].bounds, gq.card.bounds);
                            for (size_t cj = 0; cj < ci; ++cj) ok &= Apart(gq.controls[ci].bounds, gq.controls[cj].bounds);
                        }
                        // Hit testing: enabled controls return their exact action, disabled control is ignored (-1),
                        // card header opens detail, settings button opens settings.
                        const int hit_sel = Hit(gq_layout, vm, gq.controls[0].bounds);
                        const int hit_sw = Hit(gq_layout, vm, gq.controls[1].bounds);
                        const int hit_dis = Hit(gq_layout, vm, gq.controls[2].bounds);
                        const int hit_btn = Hit(gq_layout, vm, gq.controls[3].bounds);
                        ok &= hit_sel == static_cast<int>(PackAction::StoreQuickControlBase) + 0;
                        ok &= hit_sw == static_cast<int>(PackAction::StoreQuickControlBase) + 2;
                        ok &= hit_dis < 0;
                        ok &= hit_btn == static_cast<int>(PackAction::StoreQuickControlBase) + 3;
                        ok &= Hit(gq_layout, vm, gq.card.settings) == static_cast<int>(PackAction::StoreItemSettingsBase);
                        ok &= Hit(gq_layout, vm, gq.card.icon) == static_cast<int>(PackAction::StoreItemDetailBase);
                    }
                    Check(ok, "declarative StoreGenericQuick layout and hit testing succeed in wide and narrow frames");
                }
            }
        }
    }
    std::printf("%d cases, %d failures\n",cases,failures);
    return failures?1:0;
}
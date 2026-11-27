#include "host/render/desktop_renderer.h"
#include "host/overlays/desktop_overlays.h"
#include "../../../src/common/localization.h"
#include <filesystem>
#include <fstream>
#include <cstdio>
using namespace pulse::desktop;
namespace pulse::desktop::render {
struct DesktopRendererTestAccess {
    static void Widget(DesktopRenderer& r,const model::DesktopTile& tile,const model::DesktopModelState& state,
                       const LiveTileRuntimeData& live,float dpi,int dirty) {
        auto* dc=r.d2d_dc_.Get();float x=96,y=96;dc->GetDpi(&x,&y);const auto units=dc->GetUnitMode();D2D1_MATRIX_3X2_F old{};dc->GetTransform(&old);
        const float offset=dirty*96.0f/dpi;dc->SetDpi(dpi,dpi);dc->SetUnitMode(D2D1_UNIT_MODE_DIPS);dc->SetTransform(D2D1::Matrix3x2F::Translation(0,-offset));
        r.DrawLiveTile(tile,D2D1::RectF(16,16+offset,300,270+offset),state,live,{});
        dc->SetTransform(old);dc->SetDpi(x,y);dc->SetUnitMode(units);
    }
    static void Sample(DesktopRenderer& r,ID2D1DeviceContext* dc,float dpi,int dirty) {
        float x=96,y=96;dc->GetDpi(&x,&y);const auto units=dc->GetUnitMode();D2D1_MATRIX_3X2_F old{};dc->GetTransform(&old);
        const float offset=dirty*96.0f/dpi;dc->SetDpi(dpi,dpi);dc->SetUnitMode(D2D1_UNIT_MODE_DIPS);dc->SetTransform(D2D1::Matrix3x2F::Translation(0,-offset));
        for(int i=0;i<3;++i){
            r.fmt_caption_->SetTextAlignment(static_cast<DWRITE_TEXT_ALIGNMENT>(i));
            r.DrawWidgetText(L"中文文字 123.45 GB 🌤",18,r.fmt_caption_.Get(),D2D1::RectF(20,40+offset+i*44,300,75+offset+i*44),r.Brush(0xFF202020u),D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        r.fmt_caption_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        dc->SetTransform(old);dc->SetDpi(x,y);dc->SetUnitMode(units);
    }
    static void Bound(DesktopRenderer& r){
        r.widget_text_.Bind(r.dwrite_factory_.Get(),r.d2d_dc_.Get());
        for(int i=0;i<700;++i)r.widget_text_.Layout(r.dwrite_factory_.Get(),L"动态时钟 "+std::to_wstring(i),r.fmt_caption_.Get(),200,30);
    }
    static size_t Size(DesktopRenderer& r){return r.widget_text_.CacheSize();}
};
}
namespace pulse::desktop::overlays {
struct DesktopWidgetTextTestPeer {
    static bool Prepare(DesktopOverlayController& o,model::TileKind kind,const render::LiveTileRuntimeData& live,bool dark){
        o.active_kind_=OverlayKind::WidgetDetail;o.card_w_=368;o.card_h_=540;o.dark_mode_=dark;
        o.detail_={};o.detail_.kind=kind;o.detail_.weather=live.weather;o.detail_.system=live.system;o.detail_.drives=live.drives;
        o.detail_.battery=live.battery;o.detail_.countdown_title=L"2027 元旦";o.detail_.countdown_date=L"2027-01-01";
        return o.EnsureRenderDevices();
    }
    static bool OwnTarget(DesktopOverlayController& o){
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap;
        auto props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED));
        if(FAILED(o.d2d_dc_->CreateBitmap(D2D1::SizeU(400,600),nullptr,0,&props,&bitmap)))return false;
        o.d2d_dc_->SetTarget(bitmap.Get());o.d2d_dc_->BeginDraw();o.d2d_dc_->Clear(D2D1::ColorF(1,1,1));
        o.DrawWidgetDetail(o.d2d_dc_.Get(),0,0);const auto hr=o.d2d_dc_->EndDraw();o.d2d_dc_->SetTarget(nullptr);
        return SUCCEEDED(hr)&&o.detail_text_.Stats().draw_calls>0&&o.detail_text_.CacheStats().fallback_draws==0;
    }
    static void Draw(DesktopOverlayController& o,ID2D1DeviceContext* dc){o.DrawWidgetDetail(dc,0,0);}
    static auto Stats(DesktopOverlayController& o){return o.detail_text_.CacheStats();}
};
}
std::string Bytes(const std::filesystem::path& p){std::ifstream in(p,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
int wmain(int argc,wchar_t** argv){
    if(argc!=2)return 2;CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);SetEnvironmentVariableW(L"PULSE_STARTUP_TRACE",L"1");
    pulse::l10n::Initialize(GetModuleHandleW(nullptr),L"zh-CN");
    int pass=0,fail=0;auto check=[&](bool ok,const char* name){std::printf("[%s] %s\n",ok?"PASS":"FAIL",name);ok?++pass:++fail;};
    std::filesystem::path out(argv[1]);std::filesystem::create_directories(out);
    {
        render::DesktopRenderer renderer;if(!renderer.InitializeOffscreen(640,600))return 3;
        model::DesktopModelState empty;empty.settings.tiles_enabled=false;empty.settings.icons_hidden=true;
        render::DesktopInteractionState idle;idle.banner_suppressed=true;
        render::LiveTileRuntimeData live;
        live.download_items.push_back({L"C:\\fixture\\报告.pdf",L"报告 2026.pdf",L".pdf",false,234567,0});live.recent_items=live.download_items;
        live.pinned_folder_items[L"C:\\fixture"]=live.download_items;live.drives.push_back({L"C:\\",128ull<<30,512ull<<30});
        live.recycle_bin={12,23456789};live.weather.valid=true;live.weather.city=L"上海";live.weather.temp_c=23;live.weather.hi_c=26;live.weather.lo_c=18;
        live.weather.humidity_pct=65;live.weather.wind_kmh=12;live.weather.updated=L"12:34";live.weather.hours={{L"13:00",1,23},{L"14:00",2,24}};
        live.system.valid=true;live.system.cpu_pct=37;live.system.mem_used=8ull<<30;live.system.mem_total=16ull<<30;live.system.disk_used_pct=75;live.system.net_down_bps=987654;
        live.battery.valid=true;live.battery.has_battery=true;live.battery.battery_pct=84;live.battery.charging=true;live.battery.ac_online=true;live.battery.power_plan=L"平衡";live.battery.uptime_sec=7200;
        for(int kind=0;kind<=static_cast<int>(model::kLastTileKind);++kind){
            model::DesktopModelState state;state.settings.default_material=model::CardMaterial::Solid;state.todos={{L"核对中文事项 123",false}};
            model::DesktopItem item;item.name=L"应用 123.lnk";item.path=L"C:\\fixture\\app.lnk";item.in_apps_tile=true;state.items.push_back(item);
            model::DesktopTile tile;tile.id="widget";tile.kind=static_cast<model::TileKind>(kind);tile.size={2,2};tile.material=model::CardMaterial::Solid;
            tile.target_path=L"C:\\fixture";tile.note_body=L"便笺中文\n123.45 GB";tile.analog_clock=false;
            for(float dpi:{96.0f,144.0f}){
                const auto before=renderer.LabelTextStats().draw_calls;
                const auto file=out/(L"widget-"+std::to_wstring(kind)+L"-"+std::to_wstring(static_cast<int>(dpi))+L".png");
                check(renderer.RenderCompositePng(file.wstring(),empty,{}, {},{},idle,[&](auto*){render::DesktopRendererTestAccess::Widget(renderer,tile,state,live,dpi,0);}),"actual widget draws offscreen at target DPI");
                const auto builds=renderer.WidgetTextCacheStats().layout_builds;
                const auto calls=renderer.LabelTextStats().draw_calls-before;
                check(calls>0,"widget invokes actual LumaText backend (not DLL load)");
                const auto dirty=out/(L"widget-"+std::to_wstring(kind)+L"-"+std::to_wstring(static_cast<int>(dpi))+L"-dirty.png");
                check(renderer.RenderCompositePng(dirty.wstring(),empty,{}, {},{},idle,[&](auto*){render::DesktopRendererTestAccess::Widget(renderer,tile,state,live,dpi,17);}),"widget draws translated dirty origin");
                std::printf("widget=%d dpi=%.0f backend_calls=%llu new_layouts_second=%llu pixel_equal=%d\n",kind,dpi,static_cast<unsigned long long>(calls),static_cast<unsigned long long>(renderer.WidgetTextCacheStats().layout_builds-builds),Bytes(file)==Bytes(dirty));
            }
        }
        for(float dpi:{96.0f,120.0f,144.0f,192.0f}){
            const auto a=out/(L"text-phase-"+std::to_wstring(static_cast<int>(dpi))+L"-0.png"),b=out/(L"text-phase-"+std::to_wstring(static_cast<int>(dpi))+L"-1.png");
            for(int dirty:{0,17})check(renderer.RenderCompositePng((dirty?b:a).wstring(),empty,{}, {},{},idle,[&](auto* dc){render::DesktopRendererTestAccess::Sample(renderer,dc,dpi,dirty);}),"CJK numeric emoji alignment sample draws");
            check(Bytes(a)==Bytes(b),"text pixels identical across dirty origins at target DPI");
        }
        check(renderer.WidgetTextCacheStats().layout_hits>0,"repeated widget text reuses cached layouts");
        check(renderer.WidgetTextCacheStats().fallback_draws==0,"widget/text matrix has no uncounted or permanent D2D fallback");
        render::DesktopRendererTestAccess::Bound(renderer);
        check(render::DesktopRendererTestAccess::Size(renderer)<=render::DesktopTextRenderer::kMaxLayouts&&renderer.WidgetTextCacheStats().evictions>0,"dynamic clock/layout cache is bounded and evicts old entries");
        for(auto kind:{model::TileKind::Weather,model::TileKind::Calendar,model::TileKind::System,model::TileKind::Clock,model::TileKind::DiskRing,model::TileKind::Countdown,model::TileKind::Battery})for(bool dark:{false,true}){
            overlays::DesktopOverlayController overlay;
            check(overlays::DesktopWidgetTextTestPeer::Prepare(overlay,kind,live,dark),"actual widget detail owns initialized render devices");
            check(overlays::DesktopWidgetTextTestPeer::OwnTarget(overlay),"widget detail actual own D2D context invokes LumaText without fallback");
            check(renderer.RenderCompositePng((out/(L"detail-"+std::to_wstring(static_cast<int>(kind))+(dark?L"-dark.png":L"-light.png"))).wstring(),empty,{}, {},{},idle,[&](auto* dc){dc->Clear(dark?D2D1::ColorF(0x202020):D2D1::ColorF(0xF5F6F7));overlays::DesktopWidgetTextTestPeer::Draw(overlay,dc);}),"widget detail snapshot uses production drawing path");
        }
    }
    CoUninitialize();std::printf("RESULT passed=%d failed=%d\n",pass,fail);return fail?1:0;
}

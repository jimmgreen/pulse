#include "host/render/desktop_renderer.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
using namespace pulse::desktop;
namespace pulse::desktop::render {
struct DesktopRendererTestAccess {
    static void Label(DesktopRenderer& r, model::IconLabelStyle style, bool revealed, bool fractional, float dpi = 96, int dirty_offset = 0) {
        auto* dc = r.DeviceContext(); dc->Clear(D2D1::ColorF(.42f,.42f,.42f,1));
        text_contrast::Samples samples; samples.count=1; samples.colors[0]={.42f,.42f,.42f};
        const float shift = fractional ? .25f : 0;
        float old_x=96,old_y=96;dc->GetDpi(&old_x,&old_y);
        const auto units=dc->GetUnitMode();D2D1_MATRIX_3X2_F previous{};dc->GetTransform(&previous);
        dc->SetDpi(dpi,dpi);dc->SetUnitMode(D2D1_UNIT_MODE_DIPS);
        const float offset=dirty_offset*96.0f/dpi;
        dc->SetTransform(D2D1::Matrix3x2F::Translation(0,-offset));
        r.DrawIconLabel(L"应用 App baseline", D2D1::RectF(35,60+shift+offset,285,94+shift+offset), r.fmt_tile_small_title_.Get(),
            style, false, true, revealed, 1, &samples);
        dc->SetTransform(previous);dc->SetDpi(old_x,old_y);dc->SetUnitMode(units);
    }
};
}
template<class T> uint64_t Calls(T& r) {
    if constexpr (requires { r.LabelTextStats(); }) return r.LabelTextStats().draw_calls;
    else return 0;
}
std::string Bytes(const std::filesystem::path& p) {std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
int wmain(int argc,wchar_t** argv){
    if(argc!=2)return 2; CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED); int fails=0;
    auto check=[&](bool ok,const char* text){std::printf("[%s] %s\n",ok?"PASS":"FAIL",text);fails+=!ok;};
    {
        render::DesktopRenderer r; if(!r.InitializeOffscreen(320,180))return 3;
        std::filesystem::path out(argv[1]);std::filesystem::create_directories(out);
        model::DesktopModelState state;state.settings.tiles_enabled=false;state.settings.icons_hidden=true;
        render::DesktopInteractionState interaction;interaction.banner_suppressed=true;
        for(int style=0;style<4;++style){
            const auto before=Calls(r);
            for(int hover=0;hover<2;++hover){
                const auto file=out/(L"label-"+std::to_wstring(style)+L"-"+std::to_wstring(hover)+L".png");
                check(r.RenderCompositePng(file.wstring(),state,{}, {},{},interaction,[&](auto*){
                    render::DesktopRendererTestAccess::Label(r,static_cast<model::IconLabelStyle>(style),hover!=0,false);}),"offscreen label snapshot");
            }
            const auto idle=Bytes(out/(L"label-"+std::to_wstring(style)+L"-0.png"));
            const auto hover=Bytes(out/(L"label-"+std::to_wstring(style)+L"-1.png"));
            check(!idle.empty()&&(style==2 ? idle!=hover : idle==hover),"hover changes visibility only, not persistent label pixels");
            check(Calls(r)>before,"actual LumaText draw route exercised");
            const uint64_t expected=style==0?4:2;
            std::printf("style=%d backend_draws=%llu\n",style,static_cast<unsigned long long>(Calls(r)-before));
            check(Calls(r)-before==expected,"Normal/Pill draw one foreground; Shadow/visible Hidden draw shadow plus foreground");
        }
        check(r.Resize(640,360),"resize isolated DPI test surface");
        for(int dpi : {96,120,144,192}) for(int style=0;style<4;++style){
            const auto prefix=L"dpi-"+std::to_wstring(dpi)+L"-"+std::to_wstring(style);
            for(int pass=0;pass<2;++pass){
                check(r.RenderCompositePng((out/(prefix+L"-"+std::to_wstring(pass)+L".png")).wstring(),state,{}, {},{},interaction,[&](auto*){
                    render::DesktopRendererTestAccess::Label(r,static_cast<model::IconLabelStyle>(style),true,false,static_cast<float>(dpi),pass?17:0);}),"real fractional-DPI translated surface text draw");
            }
            check(Bytes(out/(prefix+L"-0.png"))==Bytes(out/(prefix+L"-1.png")),"dirty-surface origin preserves exact text pixels at target DPI");
        }
        check(r.Resize(320,180),"restore Apps test surface");
        state={};state.settings.tiles_enabled=true;state.settings.organize_enabled=true;
        state.settings.default_material=model::CardMaterial::Solid;
        model::DesktopItem item;item.name=L"应用 App baseline.lnk";item.path=L"C:\\pulse-label-baseline\\app.lnk";item.in_apps_tile=true;state.items.push_back(item);
        model::DesktopTile tile;tile.id="apps";tile.kind=model::TileKind::Apps;tile.free_placed=true;tile.x=10;tile.y=10;tile.w=300;tile.h=160;tile.size={2,2};state.tiles.push_back(tile);
        for(int frame=0;frame<4;++frame){
            interaction={};interaction.banner_suppressed=true;
            if(frame==1 || frame==3){interaction.hovered_tile_id="apps";interaction.hovered_path=item.path;}
            if(frame==2)interaction.pressed_app_index=0;
            check(r.RenderCompositePng((out/(L"apps-"+std::to_wstring(frame)+L".png")).wstring(),state,{}, {},{},interaction),"real Apps idle hover pressed snapshot");
        }
    }
    CoUninitialize();return fails?1:0;
}

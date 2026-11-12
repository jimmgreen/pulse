// Real FluentMenu snapshots and windowless interaction checks for the HTML
// archive-menu fixture. Reuses the production renderer and PNG exporter.
#include "../common/windows_compat.h"
#include "../common/localization.h"
#include "../ui/fluent_menu.h"
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace pulse::ui {
struct FluentMenuTestPeer {
    static void SetModel(FluentMenu& menu,const std::vector<FluentMenuItem>& items,IDWriteFactory2* factory,float scale) {
        menu.model_.SetItems(items); menu.model_.Layout(factory,scale);
    }
    static int HeadingGear(FluentMenu& menu,int row) {
        const float x=static_cast<float>(FluentMenu::kShadowMargin+menu.model_.WidthPx())-18*menu.scale_;
        return menu.InvokeAt(row,x);
    }
    static int HeadingLabel(FluentMenu& menu,int row) {
        return menu.InvokeAt(row,static_cast<float>(FluentMenu::kShadowMargin)+40*menu.scale_);
    }
    static int KeyboardInvoke(FluentMenu& menu,int row) {return menu.InvokeRow(row);}
};
}
namespace {
using namespace pulse::ui;
int failures=0,cases=0;
constexpr int kSettings=9100;
void Check(bool condition,const char* label) {
    ++cases; if(!condition) ++failures;
    std::printf("[%s] %s\n",condition?"PASS":"FAIL",label);
}
FluentMenuItem Row(int command,std::wstring text,StoreIcon icon,std::wstring detail={}) {
    FluentMenuItem item; item.command=command; item.text=std::move(text);
    item.vector_icon=icon; item.detail=std::move(detail); return item;
}
std::vector<FluentMenuItem> ArchiveRows() {
    using I=pulse::l10n::StringId;
    std::vector<FluentMenuItem> rows;
    auto open=Row(9000,L"打开",StoreIcon::Open); open.separator_after=true; rows.push_back(open);
    auto heading=Row(0,L"7-Zip",StoreIcon::None);
    heading.heading=true; heading.trailing_command=kSettings; heading.trailing_vector_icon=StoreIcon::Gear;
    heading.group_tint=true; heading.group_start=true; rows.push_back(heading);
    auto extract=Row(9001,pulse::l10n::Get(I::SzModeSmart),StoreIcon::Extract,L"解压到 Release\\，单文件直接放到此处");
    extract.group_tint=true; extract.primary=true; rows.push_back(extract);
    auto folder=Row(9002,pulse::l10n::Get(I::SzCmdTo),StoreIcon::ExtractTo);
    folder.group_tint=true; rows.push_back(folder);
    auto test=Row(9003,pulse::l10n::Get(I::SzCmdTest),StoreIcon::Test);
    test.group_tint=true; test.group_end=true; test.separator_after=true; rows.push_back(test);
    rows.push_back(Row(9004,L"复制路径",StoreIcon::Path));
    rows.push_back(Row(9005,L"属性",StoreIcon::Properties));
    return rows;
}
std::vector<FluentMenuItem> SubmenuRows() {
    auto rows=ArchiveRows();
    auto sub=Row(0,L"7-Zip",StoreIcon::Box); sub.group_tint=true;sub.group_start=true;sub.group_end=true;
    for(size_t i=2;i<=4;++i) {
        auto item=rows[i];item.group_tint=false;item.group_start=false;item.group_end=false;
        item.separator_after=i==4;sub.children.push_back(std::move(item));
    }
    sub.children.push_back(Row(kSettings,L"7-Zip 设置…",StoreIcon::Gear));
    return {rows[0],sub,rows[5],rows[6]};
}
void Geometry(FluentMenu& menu,IDWriteFactory2* factory,float scale) {
    const auto rows=ArchiveRows();
    FluentMenuModel model; model.SetItems(rows); model.Layout(factory,scale);
    Check(model.WidthPx()==static_cast<int>(std::ceil(264*scale)),"archive menu uses the HTML 264 DIP width");
    Check(model.RowHeightPx(1)==std::round(26*scale),"group heading uses the production 26 DIP heading height");
    Check(model.RowHeightPx(2)==std::round(46*scale) && model.RowHeightPx(3)==std::round(32*scale),"7zip double and single lines use HTML 46 and 32 DIP rows");
    bool contiguous=true;
    for(int i=1;i<4;++i) contiguous &= std::abs(model.RowTopPx(i)+model.RowHeightPx(i)-model.RowTopPx(i+1))<.1f;
    const float group_top=model.RowTopPx(1),group_bottom=model.RowTopPx(4)+model.RowHeightPx(4);
    Check(contiguous && group_top>=0 && group_bottom<model.RowTopPx(5) && group_bottom<model.HeightPx(),"one continuous group bounds contains heading and all rows and stops before the separator");
    Check(model.HitTestRow(model.RowTopPx(1)+model.RowHeightPx(1)/2)==1,"actionable heading participates in mouse row hit testing");
    Check(model.NextEnabled(0,1)==1 && model.NextEnabled(2,-1)==1,"keyboard navigation reaches the heading gear in both directions");
    FluentMenuTestPeer::SetModel(menu,rows,factory,scale);
    Check(FluentMenuTestPeer::HeadingGear(menu,1)==kSettings,"heading gear mouse target invokes exact settings command");
    Check(FluentMenuTestPeer::HeadingLabel(menu,1)==0,"heading label does not accidentally dispatch the gear command");
    Check(FluentMenuTestPeer::KeyboardInvoke(menu,1)==kSettings,"keyboard invocation of the heading reaches the same settings command");
    auto disabled=rows;disabled[1].enabled=false;
    FluentMenuTestPeer::SetModel(menu,disabled,factory,scale);
    Check(FluentMenuTestPeer::HeadingGear(menu,1)==0 && FluentMenuTestPeer::KeyboardInvoke(menu,1)==0,"disabled heading gear blocks both input routes");
    auto plain=rows;plain[1].trailing_command=0;plain[1].trailing_vector_icon=StoreIcon::None;
    model.SetItems(plain);model.Layout(factory,scale);
    Check(model.HitTestRow(model.RowTopPx(1)+model.RowHeightPx(1)/2)==-1 && model.NextEnabled(0,1)==2,"ordinary non-action headings preserve existing noninteractive behavior");
    const auto submenu=SubmenuRows();
    FluentMenuModel child;child.SetItems(submenu[1].children);child.Layout(factory,scale);
    Check(submenu[1].command==0 && child.FirstEnabled()==0 && child.At(child.Count()-1)->command==kSettings,"submenu preserves child commands and its final settings entry");
    FluentMenuTestPeer::SetModel(menu,submenu[1].children,factory,scale);
    Check(FluentMenuTestPeer::KeyboardInvoke(menu,child.Count()-1)==kSettings,"submenu settings entry uses the normal keyboard invocation route");
}
}
int wmain(int argc,wchar_t** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    if(FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)))return 2;
    const auto output=std::filesystem::absolute(argc>1?std::filesystem::path(argv[1]):std::filesystem::path(L"build/store-screenshots"));
    std::error_code error;std::filesystem::create_directories(output,error);
    if(error) {CoUninitialize();return 2;}
    pulse::l10n::Initialize(GetModuleHandleW(nullptr),L"zh-CN");
    Check(!pulse::l10n::Get(pulse::l10n::StringId::SzCmdTest).empty(),"menu fixture links actual localized resource strings");
    HWND owner=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,L"STATIC",L"Store menu render fixture",WS_POPUP,
        0,0,900,900,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!owner) {CoUninitialize();return 2;}
    {
        Compositor compositor;
        Check(compositor.Init(owner),"create shared production menu compositor");
        if(compositor.DwriteFactory()) for(float scale:{1.0f,1.5f}) {
            compositor.RecreateTextFormats(scale);
            FluentMenu menu;
            Check(menu.Create(owner,&compositor,scale),"create production FluentMenu without showing popup");
            Geometry(menu,compositor.DwriteFactory(),scale);
            for(bool dark:{false,true}) {
                menu.SetTheme(dark,D2D1::ColorF(0x527d70));
                const std::wstring suffix=(dark?L"-dark-":L"-light-")+std::to_wstring(static_cast<int>(scale*100))+L".png";
                const auto direct=output/(L"real-menu"+suffix);
                Check(menu.SaveDebugSnapshot(direct.c_str(),ArchiveRows(),2),"save production archive menu and continuous highlighted group");
                const auto sub=SubmenuRows();
                Check(menu.SaveDebugSnapshot((output/(L"real-menu-submenu-parent"+suffix)).c_str(),sub,1),"save real submenu parent with vector chevron");
                Check(menu.SaveDebugSnapshot((output/(L"real-menu-submenu-child"+suffix)).c_str(),sub[1].children,0),"save real submenu children with settings entry");
            }
        }
        compositor.Shutdown();
    }
    DestroyWindow(owner);CoUninitialize();
    std::printf("%d menu cases, %d failures\n",cases,failures);
    return failures?1:0;
}

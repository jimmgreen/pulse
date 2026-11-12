// Off-screen visual fixtures derived from archive_store_mockup.html, not catalog
// data or evidence of a running plugin. No preferences, network, or installation.
#include "../common/windows_compat.h"
#include "../common/localization.h"
#include "../ui/ui_renderer.h"
#include "../ui/store_icons.h"
#include "../../sdk/native/include/pulse/commands.h"
#include <cstdio>
#include <filesystem>
#include <string>

namespace {
using namespace pulse::ui;
int failures=0;
void Check(bool ok,const char* message) {
    std::printf("[%s] %s\n",ok?"PASS":"FAIL",message);
    if(!ok) ++failures;
}
WindowViewModel Fixture() {
    WindowViewModel vm;
    vm.settings_open=true; vm.settings_page=8; vm.settings_ui_font_scale=100;
    vm.settings_store_browse=true; vm.settings_store_installed=false;
    vm.backdrop_enabled=false;
    auto& v=vm.settings_store_view; v.state=ExtensionStoreView::State::Online;
    ExtensionStoreItem item;
    item.name=L"7-Zip 压缩与解压"; item.summary=L"右键解压、快速压缩、测试与校验；快速预览可列出 7z、RAR、ISO、CAB、WIM 等压缩包内容。";
    item.publisher=L"Pulse"; item.version=L"26.00"; item.size=L"2.1 MB";
    item.id="pulse.7zip"; item.category=L"archive"; item.official=true;
    item.settings_package=0; item.enabled=true; item.icon=StoreIcon::Box;
    item.license=L"LGPL-2.1"; item.source=L"https://www.7-zip.org/";
    item.releases={{L"26.00",L"2026-10-21",L"智能解压；快速压缩无需弹窗；校验值核对",L"1.0.59"},
        {L"25.01",L"2026-09-01",L"首个原生扩展版本",L"1.0.58"}};
    v.items.push_back(item);
    InstalledExtensionItem installed;
    installed.name=item.name; installed.id=L"pulse.7zip"; installed.version=item.version;
    installed.publisher=item.publisher; installed.enabled=true; installed.native_code=true;
    installed.can_disable=true; installed.can_uninstall=true; installed.can_rollback=true;
    v.installed.push_back(installed);
    auto& z=v.seven_zip; z.item=0; z.installed_index=0; z.available=true; z.installed=true;
    z.menu_available=true; z.formats_available=true;
    z.location=1; z.folder=LR"(C:\Archive fixtures\Extracted)";
    z.formats_rw={L"7z",L"zip",L"tar",L"gzip",L"xz",L"bzip2"};
    z.formats_ro={L"RAR",L"ISO",L"CAB",L"WIM",L"VHD",L"DMG",L"NTFS",L"FAT",L"ext4"};
    z.archive_rows={{L"解压到此处（智能）",L"单文件直接解压，多文件自动建文件夹",PULSE_COMMAND_ICON_EXTRACT},
        {L"解压到…",L"选择文件夹与其他选项",PULSE_COMMAND_ICON_EXTRACT},
        {L"测试压缩包",L"",PULSE_COMMAND_ICON_ARCHIVE}};
    z.file_rows={{L"快速压缩为 7z",L"保存为所选文件.7z",PULSE_COMMAND_ICON_ARCHIVE},
        {L"添加到压缩包…",L"",PULSE_COMMAND_ICON_ARCHIVE},
        {L"计算校验值",L"",PULSE_COMMAND_ICON_DEFAULT}};
    vm.settings_pack_images_available=true; vm.settings_pack_images_installed=true;
    vm.settings_pack_images_version=L"2026.11-long-installed-version"; vm.settings_pack_raw_available=true;
    vm.settings_pack_media_available=true; vm.settings_content_pack_state=2;
    return vm;
}
bool IconPixels(Compositor& comp) {
    auto* dc=comp.Dc();
    ComPtr<ID2D1SolidColorBrush> brush;
    if(FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(0x000000),&brush))) return false;
    constexpr int cols=12, cell=48;
    const int count=static_cast<int>(StoreIcon::Count);
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    dc->BeginDraw(); dc->Clear(D2D1::ColorF(0xffffff));
    for(int i=1;i<count;++i) {
        const float x=static_cast<float>((i-1)%cols*cell+8),y=static_cast<float>((i-1)/cols*cell+8);
        DrawStoreIcon(dc,brush.get(),D2D1::RectF(x,y,x+32,y+32),static_cast<StoreIcon>(i),1.5f);
    }
    const HRESULT hr=dc->EndDraw(); Check(SUCCEEDED(hr),"all vector icons EndDraw");
    if(FAILED(hr)) return false;
    ComPtr<ID2D1Image> target; dc->GetTarget(&target);
    ComPtr<ID2D1Bitmap1> bitmap,readback;
    if(!target.get() || FAILED(target->QueryInterface(IID_PPV_ARGS(&bitmap)))) return false;
    const auto props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ|D2D1_BITMAP_OPTIONS_CANNOT_DRAW,bitmap->GetPixelFormat());
    if(FAILED(dc->CreateBitmap(bitmap->GetPixelSize(),nullptr,0,&props,&readback)) ||
        FAILED(readback->CopyFromBitmap(nullptr,bitmap.get(),nullptr))) return false;
    D2D1_MAPPED_RECT mapped{}; if(FAILED(readback->Map(D2D1_MAP_OPTIONS_READ,&mapped))) return false;
    bool all=true;
    for(int i=1;i<count;++i) {
        int pixels=0;
        for(int y=(i-1)/cols*cell;y<((i-1)/cols+1)*cell;++y)
            for(int x=(i-1)%cols*cell;x<((i-1)%cols+1)*cell;++x) {
                const auto* px=mapped.bits+static_cast<size_t>(y)*mapped.pitch+static_cast<size_t>(x)*4;
                if(px[0]<240 || px[1]<240 || px[2]<240) ++pixels;
            }
        const bool ok=pixels>4; all &=ok;
        std::printf("[%s] StoreIcon %d visible_pixels=%d\n",ok?"PASS":"FAIL",i,pixels);
    }
    readback->Unmap(); return all;
}
bool FormatChipMetrics(Compositor& comp) {
    fluent::Painter painter(&comp);
    bool ok=true;
    for(float scale:{1.0f,1.5f}) {
        comp.RecreateTextFormats(scale); painter.SetScale(scale);
        for(const auto* name:{L"zip",L"bzip2",L"tar",L"compound",L"squashfs",L"lzma86"}) {
            const auto count=static_cast<UINT32>(std::wcslen(name));
            const float inner=painter.MeasureMonospaceBadgeWidth(name)-14*scale;
            ComPtr<IDWriteTextLayout> layout;
            DWRITE_TEXT_METRICS metrics{};
            const bool created=SUCCEEDED(comp.DwriteFactory()->CreateTextLayout(name,count,comp.SmallFormat(),inner,1000,&layout));
            if(created) layout->SetFontFamilyName(L"Consolas",{0,count});
            ok &= created && SUCCEEDED(layout->GetMetrics(&metrics)) && metrics.lineCount==1 && metrics.widthIncludingTrailingWhitespace<=inner;
        }
    }
    comp.RecreateTextFormats(1.0f);
    return ok;
}
}
int wmain(int argc,wchar_t** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    const HRESULT co=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(FAILED(co)) return 2;
    const auto out=std::filesystem::absolute(argc>1 ? std::filesystem::path(argv[1]) : std::filesystem::path(L"build/store-screenshots"));
    std::error_code ec; std::filesystem::create_directories(out,ec);
    if(ec) {CoUninitialize(); return 2;}
    pulse::l10n::Initialize(GetModuleHandleW(nullptr),L"zh-CN");
    HWND window=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,L"STATIC",L"Store render fixtures",WS_POPUP,
        0,0,1300,2400,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!window) {CoUninitialize(); return 2;}
    {
        Compositor comp;
        if(!comp.Init(window)) {DestroyWindow(window); CoUninitialize(); return 2;}
        Check(FormatChipMetrics(comp),"actual monospace format names fit without wrapping or losing their last character at 100/150 percent DPI");
        Check(IconPixels(comp),"every drawable StoreIcon has nonempty pixels (None is intentionally empty)");
        Check(comp.SaveSnapshot((out/L"vector-icons.png").c_str()),"save vector icon atlas");
        {
            MainRenderer renderer; renderer.SetCompositor(&comp); renderer.SetScale(1.0f);
            for(bool dark:{false,true}) for(bool narrow:{false,true}) {
                const int width=narrow?424:1300,height=2400;
                SetWindowPos(window,nullptr,0,0,width,height,SWP_NOACTIVATE|SWP_NOMOVE|SWP_NOZORDER);
                comp.Resize(width,height); comp.RecreateTextFormats(1.0f);
                renderer.InvalidateTypography();
                for(int page=0;page<12;++page) {
                    auto vm=Fixture(); vm.dark=dark;
                    std::wstring name;
                    if(page==0) name=L"store";
                    else if(page<=4) {vm.settings_store_view.detail=true; vm.settings_store_view.detail_tab=page-1; name=L"detail-"+std::to_wstring(page-1);}
                    else if(page==5) {vm.settings_store_browse=false;vm.settings_store_installed=true;name=L"installed";}
                    else if(page==6) {vm.settings_store_view.state=ExtensionStoreView::State::Loading;vm.settings_store_view.busy=true;name=L"loading";}
                    else if(page==7) {vm.settings_store_view.state=ExtensionStoreView::State::Failed;vm.settings_store_view.notice=L"Fixture network failure: HTTP 503";name=L"failed";}
                    else if(page==8) {vm.settings_store_view.state=ExtensionStoreView::State::Offline;name=L"offline";}
                    else if(page==9) {vm.settings_store_view.pending_restart=true;vm.settings_store_view.seven_zip.submenu=true;name=L"restart-submenu";}
                    else {vm.settings_store_view.items[0].enabled=false;vm.settings_store_view.items[0].settings_package=-1;
                        vm.settings_store_view.seven_zip.available=false;vm.settings_store_view.detail=page==11;
                        name=page==11?L"disabled-detail":L"disabled-store";}
                    const auto theme=MakeTheme(dark,D2D1::ColorF(0x527d70));
                    auto* dc=comp.Dc(); dc->BeginDraw(); dc->Clear(theme.bg);
                    renderer.Render(vm,D2D1::RectF(0,0,static_cast<float>(width),static_cast<float>(height)),theme);
                    const HRESULT hr=dc->EndDraw();
                    Check(SUCCEEDED(hr),"MainRenderer store fixture EndDraw");
                    const auto path=out/(name+(dark?L"-dark":L"-light")+(narrow?L"-narrow":L"-wide")+L".png");
                    Check(SUCCEEDED(hr)&&comp.SaveSnapshot(path.c_str()),"save store fixture PNG");
                    std::wprintf(L"PNG %ls\n",path.c_str());
                }
            }
        }
        comp.Shutdown();
    }
    DestroyWindow(window); CoUninitialize();
    std::printf("store render fixtures: %d failures\n",failures);
    return failures?1:0;
}

// Off-screen visual fixtures derived from archive_store_mockup.html, not catalog
// data or evidence of a running plugin. No preferences, network, or installation.
#include "../common/windows_compat.h"
#include "../common/localization.h"
#include "../app/extensions/icon_registry.h"
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
    static const PulseIconId custom_id = pulse::extensions::icons::GlobalRegistry().UpsertManifestIcon(
        "third.party.demo", "brand", 24000u,
        {{"M5 5h14v14H5z", PULSE_ICON_DRAW_FILL | PULSE_ICON_DRAW_CLOSED, PULSE_ICON_COLOR_INHERIT, 0u, 0u, 900u},
         {"M8 12l3 3 5-6", PULSE_ICON_DRAW_STROKE, PULSE_ICON_COLOR_ACCENT, 0u, 2000u, 1000u}});
    ExtensionStoreItem third_party;
    third_party.id="third.party.demo"; third_party.name=L"第三方批量工作流";
    third_party.summary=L"展示自定义 SVG 矢量图标、详细介绍、特性列表、标签与权限声明的完整详情页。";
    third_party.description=L"这是一个通过公开 manifest.showcase 元数据驱动的通用扩展详情页。\n支持多段落介绍、主要特性、标签过滤以及独立的许可与版本历史展示。";
    third_party.publisher=L"Community Studio"; third_party.version=L"2.4.0"; third_party.size=L"1.4 MB";
    third_party.category=L"files"; third_party.icon=StoreIcon::Puzzle; third_party.icon_color=0x3A7E9B;
    third_party.custom_icon=custom_id; third_party.engine=L"WorkflowEngine 3.1";
    third_party.license=L"MIT"; third_party.homepage=L"https://example.com/workflow";
    third_party.source=L"https://github.com/example/workflow";
    third_party.tags={L"工作流", L"批处理", L"自动化"};
    third_party.highlights={L"通过公开 contributes.icons 渲染品牌渐变磁贴", L"无需写入宿主 C++ 特判即可展现完整展示页"};
    third_party.permissions={L"读取所选文件", L"自有设置"};
    third_party.releases={{L"2.4.0",L"2026-11-20",L"支持商店展示页与自定义品牌图标",L"1.0.61"},
        {L"2.3.0",L"2026-10-15",L"首个公开发布版本",L"1.0.60"}};
    third_party.installed=true; third_party.installed_index=1; third_party.enabled=true;
    third_party.settings_package=1; third_party.has_menu_tab=true;
    third_party.button_label=L"更新"; third_party.action_enabled=true;
    v.items.push_back(third_party);
    InstalledExtensionItem installed;
    installed.name=item.name; installed.id=L"pulse.7zip"; installed.version=item.version;
    installed.publisher=item.publisher; installed.enabled=true; installed.native_code=true;
    installed.can_disable=true; installed.can_uninstall=true; installed.can_rollback=true;
    v.installed.push_back(installed);
    InstalledExtensionItem third_installed;
    third_installed.name=third_party.name; third_installed.id=L"third.party.demo"; third_installed.version=third_party.version;
    third_installed.publisher=third_party.publisher; third_installed.enabled=true; third_installed.native_code=true;
    third_installed.can_disable=true; third_installed.can_uninstall=true;
    v.installed.push_back(third_installed);
    // 7-Zip is shown through the generic manifest quick-settings card, like any plugin.
    ExtensionQuickCardView quick; quick.item_index=0; quick.package_index=0; quick.package_id="pulse.7zip";
    quick.title=L"常用设置"; quick.available=true; quick.has_menu_preview=true; quick.preview_caption=L"右键压缩包时";
    quick.preview_rows={{L"解压到此处（智能）",L"单文件直接解压，多文件自动建文件夹",PULSE_COMMAND_ICON_EXTRACT},
        {L"解压到…",L"选择文件夹与其他选项",PULSE_COMMAND_ICON_EXTRACT},
        {L"测试压缩包",L"",PULSE_COMMAND_ICON_ARCHIVE}};
    {
        auto control=[](int kind,int action,std::wstring label,bool checked,uint32_t w){
            ExtensionQuickControlView c;c.kind=kind;c.action=action;c.label=std::move(label);c.checked=checked;c.width_dip=w;return c;};
        quick.rows.push_back({L"智能解压",L"单文件直接解压到此处，多文件自动新建文件夹",{control(0,91001,L"",true,0)}});
        quick.rows.push_back({L"快速压缩格式",L"右键「添加到压缩包」时直接使用，不弹出对话框",{control(1,91002,L"7z（推荐）",false,150)}});
        quick.rows.push_back({L"右键菜单位置",L"",{control(1,91003,L"直接显示",false,150)}});
    }
    v.quick_cards.push_back(quick);
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
    const bool status_only = argc == 3 && std::wstring_view(argv[2]) == L"--status-notices";
    {
        Compositor comp;
        if(!comp.Init(window)) {DestroyWindow(window); CoUninitialize(); return 2;}
        if (!status_only) {
            Check(FormatChipMetrics(comp),"actual monospace format names fit without wrapping or losing their last character at 100/150 percent DPI");
            Check(IconPixels(comp),"every drawable StoreIcon has nonempty pixels (None is intentionally empty)");
            Check(comp.SaveSnapshot((out/L"vector-icons.png").c_str()),"save vector icon atlas");
        }
        {
            MainRenderer renderer; renderer.SetCompositor(&comp); renderer.SetScale(1.0f);
            for(bool dark:{false,true}) for(bool narrow:{false,true}) {
                const int width=narrow?424:1300,height=status_only?920:2400;
                SetWindowPos(window,nullptr,0,0,width,height,SWP_NOACTIVATE|SWP_NOMOVE|SWP_NOZORDER);
                comp.Resize(width,height); comp.RecreateTextFormats(1.0f);
                renderer.InvalidateTypography();
                for(int page=0;page<(status_only?4:16);++page) {
                    auto vm=Fixture(); vm.dark=dark;
                    std::wstring name;
                    if (status_only) {
                        using I = pulse::l10n::StringId;
                        using N = ExtensionStoreView::NoticeKind;
                        auto& view = vm.settings_store_view;
                        if (page == 0) {
                            view.notice = pulse::l10n::Get(I::StoreInstallFailed) + L"\n" +
                                pulse::l10n::Get(I::StoreErrorSignatureRequired);
                            view.notice_kind = N::Error;
                            view.state = ExtensionStoreView::State::Offline;
                            name = L"signature-failure";
                        } else if (page == 1) {
                            view.install_completed = true;
                            view.pending_restart = true;
                            name = L"install-success-restart";
                        } else if (page == 2) {
                            vm.settings_store_browse = false;
                            view.pending_restart = true;
                            name = L"manage-restart";
                        } else {
                            view.notice = pulse::l10n::Get(I::ExtensionStorePreparing);
                            view.pending_restart = true;
                            view.installed_busy = true;
                            view.busy = true;
                            name = L"busy-restart-disabled";
                        }
                    }
                    else if(page==0) name=L"store";
                    else if(page<=4) {vm.settings_store_view.detail=true; vm.settings_store_view.detail_item=0; vm.settings_store_view.detail_tab=(page-1)&1; name=L"detail-"+std::to_wstring(page-1);}
                    else if(page==5) {vm.settings_store_browse=false;vm.settings_store_installed=true;name=L"installed";}
                    else if(page==6) {vm.settings_store_view.state=ExtensionStoreView::State::Loading;vm.settings_store_view.busy=true;name=L"loading";}
                    else if(page==7) {vm.settings_store_view.state=ExtensionStoreView::State::Failed;vm.settings_store_view.notice=L"Fixture network failure: HTTP 503";name=L"failed";}
                    else if(page==8) {vm.settings_store_view.state=ExtensionStoreView::State::Offline;name=L"offline";}
                    else if(page==9) {vm.settings_store_view.pending_restart=true;vm.settings_store_view.quick_cards[0].preview_submenu=true;name=L"restart-submenu";}
                    else if(page<=11) {vm.settings_store_view.items[0].enabled=false;vm.settings_store_view.items[0].settings_package=-1;
                        vm.settings_store_view.quick_cards[0].available=false;vm.settings_store_view.detail=page==11;vm.settings_store_view.detail_item=0;
                        name=page==11?L"disabled-detail":L"disabled-store";}
                    else if(page==12||page==13) {vm.settings_store_view.detail=true;vm.settings_store_view.detail_item=1;
                        vm.settings_store_view.detail_tab=page-12;name=page==12?L"ext-detail-overview":L"ext-detail-about";}
                    else if(page==15) {vm.settings_store_browse=false;vm.settings_store_installed=true;
                        vm.settings_store_view.developer_mode=true;vm.settings_store_view.pending_restart=true;
                        InstalledExtensionItem dev;dev.name=L"File columns example";dev.id=L"com.example.file-columns";
                        dev.version=L"本地构建 · 4800c2a";dev.publisher=L"C:\\Users\\SS\\PulseExtTest\\dev-folders\\file-columns";
                        dev.status=L"已从文件夹加载";dev.enabled=true;dev.native_code=true;dev.can_uninstall=true;dev.development_index=0;
                        vm.settings_store_view.installed.push_back(dev);
                        dev.name=L"broken.dev";dev.id=L"broken.dev";dev.enabled=false;dev.status=L"重启 Pulse 后生效";
                        dev.notice=L"找不到 manifest 声明的文件：broken.dll";dev.development_index=1;
                        vm.settings_store_view.installed.push_back(dev);name=L"installed-dev";}
                    else {vm.settings_store_view.detail=true;vm.settings_store_view.detail_item=1;vm.settings_store_view.detail_tab=0;
                        vm.settings_store_view.items[1].installed=false;vm.settings_store_view.items[1].enabled=false;
                        vm.settings_store_view.items[1].settings_package=-1;vm.settings_store_view.items[1].button_label=L"下载并安装";
                        name=L"ext-detail-uninstalled";}
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
// Real Fluent upload and Painter render; no visible window or desktop switching.
#include "../ui/fluent_menu.h"
#include "../ui/FluentTokens.h"
#include "../shell_host/ctx_handlers.h"
#include "../ipc/shell_client.h"
#include <cstdio>
#include <cmath>
#include <fstream>
namespace pulse::app { std::wstring GetPulseDataDir() { return {}; } }
namespace pulse::ui {
struct FluentMenuTestPeer {
    static int Draw(FluentMenu& menu, Compositor& compositor, const common::MenuIconRef& pixels,
                    const char* name, const wchar_t* output) {
        auto* dc = compositor.Dc(); dc->SetDpi(96,96);
        std::vector<shell::CtxItemOut> sent(1); sent[0].icon=pixels;
        ipc::PayloadWriter writer; ipc::PutCtxItemIcons(writer,sent);
        ipc::PayloadReader reader(writer.data().data(),writer.data().size());
        std::vector<ipc::CtxMenuItem> received(1);
        const bool roundtrip=ipc::TryCtxItemIcons(reader,received) && reader.remaining()==0 &&
            received[0].icon && received[0].icon->width==pixels->width &&
            received[0].icon->height==pixels->height && received[0].icon->bgra==pixels->bgra;
        std::printf("[%s] host/client icon IPC roundtrip %s\n",roundtrip?"PASS":"FAIL",name);
        if(!roundtrip)return 1;
        const float scale=static_cast<float>(pixels->width)/16;
        fluent::Painter painter(&compositor); painter.SetScale(scale);
        const auto theme=MakeTheme(false,D2D1::ColorF(0.2f,0.4f,0.8f));
        ComPtr<ID2D1Bitmap1> target,cpu;
        const auto fmt=D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED);
        if(FAILED(dc->CreateBitmap(D2D1::SizeU(256,96),nullptr,0,D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,fmt,96,96),&target)))return 1;
        dc->SetTarget(target.get()); dc->BeginDraw(); dc->Clear(D2D1::ColorF(0,0.0f));
        if(!painter.BeginFrame(theme))return 1;
        fluent::MenuItemSpec spec; spec.bounds=D2D1::RectF(0,0,256,32*scale);
        spec.icon_bitmap=menu.IconBitmapFor(received[0].icon,dc); painter.DrawMenuItem(spec);
        if(FAILED(dc->EndDraw()))return 1;
        if(FAILED(dc->CreateBitmap(D2D1::SizeU(256,96),nullptr,0,D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ|D2D1_BITMAP_OPTIONS_CANNOT_DRAW,fmt,96,96),&cpu))||FAILED(cpu->CopyFromBitmap(nullptr,target.get(),nullptr)))return 1;
        D2D1_MAPPED_RECT map{}; if(FAILED(cpu->Map(D2D1_MAP_OPTIONS_READ,&map)))return 1;
        const unsigned left=static_cast<unsigned>(std::round(16*scale));
        const unsigned top=static_cast<unsigned>(std::round(8*scale));
        unsigned changes=0,max_delta=0;std::vector<uint8_t> rendered;
        for(unsigned y=0;y<pixels->height;++y)for(unsigned x=0;x<pixels->width*4;++x) {
            const auto got=map.bits[(top+y)*map.pitch+left*4+x]; const auto want=pixels->bgra[y*pixels->width*4+x];
            rendered.push_back(got);changes+=got!=want;
            max_delta=(std::max)(max_delta,static_cast<unsigned>(std::abs(int(got)-int(want))));
        }
        cpu->Unmap(); dc->SetTarget(nullptr);
        if(output) {std::ofstream f(output,std::ios::binary);const uint32_t size[]{pixels->width,pixels->height};f.write(reinterpret_cast<const char*>(size),sizeof(size));f.write(reinterpret_cast<const char*>(rendered.data()),rendered.size());}
        std::printf("[%s] %s source=%ux%u target=%ux%u changed_channels=%u max_delta=%u\n",changes?"FAIL":"PASS",name,pixels->width,pixels->height,pixels->width,pixels->height,changes,max_delta);
        return changes?1:0;
    }
};
}
int wmain(int argc,wchar_t** argv) {
    using namespace pulse::ui;
    const auto previous=GetThreadDesktop(GetCurrentThreadId());
    const auto name=L"PulseNativePixels-"+std::to_wstring(GetCurrentProcessId());
    HDESK desktop=CreateDesktopW(name.c_str(),nullptr,nullptr,0,GENERIC_ALL,nullptr);
    if(!desktop||!SetThreadDesktop(desktop))return 2;
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"PulseNativePixels";RegisterClassW(&wc);
    HWND owner=CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP|WS_EX_NOACTIVATE,wc.lpszClassName,L"",WS_POPUP,0,0,800,600,nullptr,nullptr,wc.hInstance,nullptr);
    int failures=0;
    {
        Compositor compositor;FluentMenu menu;
        if(!owner||!compositor.Init(owner)||!menu.Create(owner,&compositor,1))return 2;
        for(unsigned edge:{16u,20u,24u,28u,32u}) {
            auto p=std::make_shared<pulse::common::MenuIconPixels>();p->width=p->height=edge;p->bgra.resize(edge*edge*4);
            for(unsigned y=0;y<edge;++y)for(unsigned x=0;x<edge;++x) {
                auto i=(y*edge+x)*4; p->bgra[i]=p->bgra[i+1]=p->bgra[i+2]=((x+y)%2)?255:0;p->bgra[i+3]=255;
            }
            failures+=FluentMenuTestPeer::Draw(menu,compositor,p,"opaque one-pixel checker",nullptr);
        }
        for(int a=1;a<argc;++a) {
            auto p=std::make_shared<pulse::common::MenuIconPixels>();std::ifstream f(argv[a],std::ios::binary);
            f.read(reinterpret_cast<char*>(&p->width),4);f.read(reinterpret_cast<char*>(&p->height),4);
            if(!f||p->width>64||p->height>64)return 2;
            p->bgra.resize(p->width*p->height*4);f.read(reinterpret_cast<char*>(p->bgra.data()),p->bgra.size());if(!f||!p->Valid())return 2;
            failures+=FluentMenuTestPeer::Draw(menu,compositor,p,"native captured icon",(std::wstring(argv[a])+L".rendered.bgra").c_str());
        }
    }
    DestroyWindow(owner);CoUninitialize();SetThreadDesktop(previous);CloseDesktop(desktop);
    return failures?1:0;
}

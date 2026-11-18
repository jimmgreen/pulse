#include "../ui/plain_highlight_lines.h"
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <functional>
#include <string>
#include <cstdio>
#include <chrono>
using Microsoft::WRL::ComPtr;
struct HighlightTarget {
    ComPtr<ID2D1DeviceContext> dc;
    ComPtr<ID2D1Bitmap1> bitmap,readback;
    bool Init(ID2D1Device* device) {
        if(FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,&dc)))return false;
        const auto pixel=D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED);
        return SUCCEEDED(dc->CreateBitmap(D2D1::SizeU(900,600),nullptr,0,D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,pixel),&bitmap))&&
            SUCCEEDED(dc->CreateBitmap(D2D1::SizeU(900,600),nullptr,0,D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ|D2D1_BITMAP_OPTIONS_CANNOT_DRAW,pixel),&readback));
    }
    std::vector<uint8_t> Render(const std::function<void()>& draw,bool dark) {
        dc->SetTarget(bitmap.Get());dc->BeginDraw();dc->Clear(D2D1::ColorF(dark?0x17191c:0xffffff));draw();
        if(FAILED(dc->EndDraw())||FAILED(readback->CopyFromBitmap(nullptr,bitmap.Get(),nullptr)))return {};
        D2D1_MAPPED_RECT mapped{};if(FAILED(readback->Map(D2D1_MAP_OPTIONS_READ,&mapped)))return {};
        std::vector<uint8_t> out(900*600*4);
        for(size_t y=0;y<600;++y)std::copy_n(mapped.bits+y*mapped.pitch,3600,out.data()+y*3600);
        readback->Unmap();return out;
    }
};

void Draw(HighlightTarget& target, IDWriteTextLayout* layout, pulse::ui::PlainHighlightLines& index,
          const std::vector<uint32_t>& matches, uint32_t length, float top, bool optimized) {
    auto first=matches.begin(),last=matches.end();
    uint32_t begin=0,end=0;
    if(optimized&&matches.size()>128&&index.Range(layout,top,top+600,begin,end)) {
        first=std::lower_bound(first,last,begin>=length?begin-length+1:0);
        last=std::lower_bound(first,last,end);
    }
    ComPtr<ID2D1SolidColorBrush> brush,current;
    target.dc->CreateSolidColorBrush(D2D1::ColorF(0xC19C00,.28f),&brush);
    target.dc->CreateSolidColorBrush(D2D1::ColorF(0xCA5010,.4f),&current);
    for(auto it=first;it!=last;++it) {
        DWRITE_HIT_TEST_METRICS metrics[96];UINT32 actual=0;
        if(FAILED(layout->HitTestTextRange(*it,length,0,-top,metrics,96,&actual)))continue;
        for(UINT32 i=0;i<actual;++i)target.dc->FillRectangle(D2D1::RectF(metrics[i].left,metrics[i].top,metrics[i].left+metrics[i].width,metrics[i].top+metrics[i].height),it==matches.begin()+matches.size()/2?current.Get():brush.Get());
    }
}
int main(int argc,char**) {
    ComPtr<IDWriteFactory> factory;
    ComPtr<ID3D11Device> d3d;ComPtr<IDXGIDevice> dxgi;ComPtr<ID2D1Device> device;
    if(FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(factory.GetAddressOf())))||
       FAILED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&d3d,nullptr,nullptr))||FAILED(d3d.As(&dxgi))||FAILED(D2D1CreateDevice(dxgi.Get(),nullptr,&device)))return 2;
    HighlightTarget target;if(!target.Init(device.Get()))return 2;
    bool good=true;
    for(float scale:{1.f,1.5f}) {
        ComPtr<IDWriteTextFormat> format;
        factory->CreateTextFormat(L"Segoe UI",nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,14*scale,L"en-us",&format);
        std::wstring text;
        for(int i=0;i<1000;++i)text+=L"abc \x05d0\x05d1\x05d2 \xD83D\xDE00 abc\r\nabc long wrapped line abc abc abc abc\n";
        ComPtr<IDWriteTextLayout> layout;factory->CreateTextLayout(text.data(),static_cast<UINT32>(text.size()),format.Get(),170*scale,100000,&layout);
        layout->SetFontSize(32*scale,{0,25});
        for(uint32_t length:{1u,29u})for(bool dark:{false,true}) {
            std::vector<uint32_t> matches;for(uint32_t i=0;i+length<=text.size();i+=3)matches.push_back(i);
            pulse::ui::PlainHighlightLines index;
            for(float top:{0.f,.25f,48.5f,999.f,10000.f}) {
                auto expected=target.Render([&]{Draw(target,layout.Get(),index,matches,length,top,false);},dark);
                auto actual=target.Render([&]{Draw(target,layout.Get(),index,matches,length,top,true);},dark);
                if(expected.empty()||expected!=actual) {printf("[FAIL] pixels scale=%.1f length=%u dark=%d top=%.2f\n",scale,length,dark,top);good=false;}
            }
        }
        if(argc>1)for(size_t stride:{size_t{3},size_t{3000}}) {
            std::vector<uint32_t> matches;for(size_t i=0;i+29<=text.size();i+=stride)matches.push_back(static_cast<uint32_t>(i));
            for(int mode=0;mode<3;++mode) {
                pulse::ui::PlainHighlightLines index;
                const auto start=std::chrono::steady_clock::now();
                for(int repeat=0;repeat<30;++repeat) {
                    if(mode==1)index.Reset();
                    target.dc->SetTarget(target.bitmap.Get());target.dc->BeginDraw();target.dc->Clear();
                    Draw(target,layout.Get(),index,matches,29,10000,mode!=0);
                    if(FAILED(target.dc->EndDraw()))return 2;
                }
                printf("BENCH scale=%.1f matches=%zu mode=%s ms/frame=%.6f\n",scale,matches.size(),mode==0?"baseline":mode==1?"cold-index":"warm-index",std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()/30);
            }
        }
    }
    printf("[%s] WARP exact full bitmap comparisons at 100/150%%: bidi emoji CRLF mixed-size wrapped multiline boundary/current/normal highlights\n",good?"PASS":"FAIL");return good?0:1;
}
#include "../ui/markdown_view.h"
#include "../ui/table_view.h"
#include "../ui/tree_view.h"
#include <d3d11.h>
#include <cstdio>
#include <functional>
using Microsoft::WRL::ComPtr;
using namespace pulse::ui;
struct HighlightTarget {
    ComPtr<ID2D1DeviceContext> dc;
    ComPtr<ID2D1Bitmap1> bitmap,readback;
    bool Init(ID2D1Device* device) {
        if(FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,&dc)))return false;
        const auto pixel=D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED);
        return SUCCEEDED(dc->CreateBitmap(D2D1::SizeU(900,600),nullptr,0,D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,pixel),&bitmap))&&
            SUCCEEDED(dc->CreateBitmap(D2D1::SizeU(900,600),nullptr,0,D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ|D2D1_BITMAP_OPTIONS_CANNOT_DRAW,pixel),&readback));
    }
    std::vector<uint8_t> Render(const std::function<void(SearchHighlights)>& draw,SearchHighlights marks,bool dark) {
        dc->SetTarget(bitmap.Get());dc->BeginDraw();dc->Clear(D2D1::ColorF(dark?0x17191c:0xffffff));draw(marks);
        if(FAILED(dc->EndDraw())||FAILED(readback->CopyFromBitmap(nullptr,bitmap.Get(),nullptr)))return {};
        D2D1_MAPPED_RECT mapped{};if(FAILED(readback->Map(D2D1_MAP_OPTIONS_READ,&mapped)))return {};
        std::vector<uint8_t> out(900*600*4);
        for(size_t y=0;y<600;++y)std::copy_n(mapped.bits+y*mapped.pitch,3600,out.data()+y*3600);
        readback->Unmap();return out;
    }
};
bool Compare(HighlightTarget& target,const std::wstring& plain,const std::function<void(SearchHighlights)>& draw,bool dark) {
    bool good=true,visible=false;
    for(const auto& query:{std::wstring(L"a"),plain.substr(3,5),std::wstring(L"absent-query")}) {
        std::vector<uint32_t> offsets;
        for(size_t at=0;(at=plain.find(query,at))!=std::wstring::npos;++at)offsets.push_back(static_cast<uint32_t>(at));
        for(size_t current:{size_t{0},offsets.size()}) {
            std::vector<SearchHighlight> explicit_marks;
            for(size_t i=0;i<offsets.size();++i)explicit_marks.push_back({offsets[i],static_cast<uint32_t>(query.size()),i==current});
            const auto expected=target.Render(draw,explicit_marks,dark);
            const auto borrowed=target.Render(draw,SearchHighlights(offsets,static_cast<uint32_t>(query.size()),current),dark);
            good=good&&!expected.empty()&&expected==borrowed;
            if(query==L"a")visible=visible||borrowed!=target.Render(draw,{},dark);
        }
    }
    return good&&visible;
}
int main() {
    if(FAILED(CoInitializeEx(nullptr,COINIT_MULTITHREADED)))return 2;
    ComPtr<IDWriteFactory2> factory;
    ComPtr<ID3D11Device> d3d;ComPtr<IDXGIDevice> dxgi;ComPtr<ID2D1Device> device;
    if(FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory2),reinterpret_cast<IUnknown**>(factory.GetAddressOf())))||
        FAILED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&d3d,nullptr,nullptr))||
        FAILED(d3d.As(&dxgi))||FAILED(D2D1CreateDevice(dxgi.Get(),nullptr,&device)))return 2;
    HighlightTarget target;if(!target.Init(device.Get()))return 2;
    MarkdownView md;TableView table;TreeView tree;
    std::wstring markdown=L"PULSEMD\t1\n",csv=L"PULSETBL\t1\nS\tcsv\tCSV\t2\t200\t0\tcomma\t1\n",json=L"PULSETREE\t1\nH\tjson\t0\t0\t\t0\t0\nN\t0\ta\t\t\t200\t\n";
    for(int i=0;i<200;++i){markdown+=L"B\tp\t\t0\t0\t\taaaa\t\n";csv+=L"R\taaaa\taaaa\n";json+=L"N\t1\ts\t\taaaa\t0\t\n";}
    if(!md.SetPayload(markdown,L"C:\\isolated.md")||!table.SetPayload(csv)||!tree.SetPayload(json))return 2;
    bool good=true;
    for(bool dark:{false,true})for(float scale:{1.0f,1.5f}) {
        const auto theme=MakeTheme(dark,D2D1::ColorF(0.2f,0.4f,0.8f));const auto rect=D2D1::RectF(0,0,700,500);
        const bool a=Compare(target,md.PlainText(),[&](SearchHighlights h){md.Draw(target.dc.Get(),factory.Get(),rect,theme,dark,scale,nullptr,1,1,3,h);},dark);
        const bool b=Compare(target,table.PlainText(),[&](SearchHighlights h){table.Draw(target.dc.Get(),factory.Get(),rect,theme,dark,scale,D2D1::ColorF(dark?0x17191c:0xffffff),h);},dark);
        const bool c=Compare(target,tree.PlainText(),[&](SearchHighlights h){tree.Draw(target.dc.Get(),factory.Get(),rect,theme,dark,scale,h);},dark);
        printf("[%s] borrowed/explicit whole pixels md=%d table/header=%d tree=%d dark=%d scale=%.1f including current/empty/cross-boundary query and Markdown selection\n",a&&b&&c?"PASS":"FAIL",a?1:0,b?1:0,c?1:0,dark?1:0,scale);
        good=good&&a&&b&&c;
    }
    return good?0:1;
}

#include "../preview_host/table_document.h"
#include "../ui/table_view.h"
#include "../ui/ui_compositor.h"
#include <wincodec.h>
#include <filesystem>
#include <cstdio>
#include <sstream>
template<class T> using WrlPtr = Microsoft::WRL::ComPtr<T>;
bool SavePng(ID2D1DeviceContext* dc, ID2D1Bitmap1* bitmap, const std::filesystem::path& path) {
    WrlPtr<ID2D1Bitmap1> readable;
    auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, bitmap->GetPixelFormat());
    const auto size = bitmap->GetPixelSize();
    if (FAILED(dc->CreateBitmap(size, nullptr, 0, props, &readable)) || FAILED(readable->CopyFromBitmap(nullptr, bitmap, nullptr))) return false;
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
    WrlPtr<IWICImagingFactory> wic;
    WrlPtr<IWICStream> stream;
    WrlPtr<IWICBitmapEncoder> encoder;
    WrlPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    const bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
        SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) && SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
        SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(size.width, size.height)) &&
        SUCCEEDED(frame->SetPixelFormat(&format)) && SUCCEEDED(frame->WritePixels(size.height, mapped.pitch, mapped.pitch * size.height, mapped.bits)) &&
        SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    readable->Unmap();
    return ok;
}

int wmain(int argc, wchar_t** argv) {
 if(argc!=2) return 2;
 OleInitialize(nullptr);
 int failed=0;
 auto check=[&](bool ok,const char* text){printf("[%s] %s\n",ok?"PASS":"FAIL",text); failed+=!ok;};
 std::wstring first;
 size_t colors=0;
 for(unsigned sheet=0;sheet<12;++sheet){
  std::wstring payload; uint32_t bytes=0;
  check(pulse::preview::MakeXlsxTable(argv[1],payload,bytes,sheet),"read real workbook sheet");
  if(sheet==11) first=payload;
  std::wistringstream lines(payload); std::wstring line;
  while(std::getline(lines,line)) if(line.starts_with(L"F\t")) ++colors;
  pulse::ui::TableView view; check(view.SetPayload(payload),"parse styled payload");
  check(view.PlainText().find(L"FF92D050")==std::wstring::npos && view.PlainText().find(L'\x0002')==std::wstring::npos,"style metadata stays out of text");
 }
 printf("colored cells=%zu\n",colors);
 check(colors==3386,"all 3386 workbook fills retained including blank cells");
 HWND h=CreateWindowExW(0,L"STATIC",L"fill test",WS_POPUP,0,0,1100,800,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
 {
  pulse::ui::Compositor comp;
  check(comp.Init(h),"initialize renderer");
  auto dc=comp.Dc();
  WrlPtr<ID2D1Bitmap1> bitmap;
  auto props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED));
  check(SUCCEEDED(dc->CreateBitmap(D2D1::SizeU(1100,800),nullptr,0,props,&bitmap)),"create render target");
  dc->SetTarget(bitmap.Get()); dc->SetDpi(96,96);
  for(bool dark:{false,true}) {
   pulse::ui::TableView table; table.SetPayload(first);
   auto bg=pulse::ui::HexColor(dark?0x202020:0xffffff);
   dc->BeginDraw();dc->Clear(bg);
   table.Draw(dc,comp.DwriteFactory(),D2D1::RectF(0,0,1100,800),pulse::ui::MakeTheme(dark,pulse::ui::HexColor(0x0078d4)),dark,1,bg,{});
   check(SUCCEEDED(dc->EndDraw()),"render real styled worksheet");
   check(SavePng(dc,bitmap.Get(),dark?L"build/sheet-fill-dark.png":L"build/sheet-fill-light.png"),"save rendered sheet");
  }
 }
 DestroyWindow(h);OleUninitialize();return failed?1:0;
}



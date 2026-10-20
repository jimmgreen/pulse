#include "../ui/command_icons.h"
#include "../ui/FluentTokens.h"
#include <wincodec.h>
#include <dwrite.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>

namespace {
template<class T> struct Ref {
    T* value = nullptr;
    ~Ref() { if (value) value->Release(); }
    T** Put() { return &value; }
    T* operator->() const { return value; }
};
struct Apartment {
    HRESULT result = CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};
int failures = 0;
void Check(bool condition,const char* name) {
    if (!condition) { ++failures; std::printf("[FAIL] %s\n",name); }
}
float Luminance(D2D1_COLOR_F color) {
    const auto linear=[](float v) { return v<=0.04045f ? v/12.92f : std::pow((v+0.055f)/1.055f,2.4f); };
    return 0.2126f*linear(color.r)+0.7152f*linear(color.g)+0.0722f*linear(color.b);
}
float Contrast(D2D1_COLOR_F a,D2D1_COLOR_F b) {
    const float x=Luminance(a),y=Luminance(b);
    return ((std::max)(x,y)+0.05f)/((std::min)(x,y)+0.05f);
}
struct Pixels { int ink=0,outside=0; };
Pixels Inspect(IWICBitmap* bitmap,D2D1_COLOR_F background,D2D1_RECT_F bounds) {
    Ref<IWICBitmapLock> lock;
    WICRect rect{0,0,64,64};
    if (FAILED(bitmap->Lock(&rect,WICBitmapLockRead,lock.Put()))) return {-1,-1};
    UINT stride=0,size=0;BYTE* data=nullptr;
    lock->GetStride(&stride);lock->GetDataPointer(&size,&data);
    if (!data || size<stride*64) return {-1,-1};
    const int base[]={static_cast<int>(std::lround(background.b*255)),
                      static_cast<int>(std::lround(background.g*255)),
                      static_cast<int>(std::lround(background.r*255))};
    Pixels result;
    for(int y=0;y<64;++y) for(int x=0;x<64;++x) {
        const BYTE* pixel=data+static_cast<size_t>(y)*stride+static_cast<size_t>(x)*4;
        bool changed=false;
        for(int c=0;c<3;++c) changed|=std::abs(static_cast<int>(pixel[c])-base[c])>2;
        if (!changed) continue;
        ++result.ink;
        if (x<bounds.left-1 || x>bounds.right+1 || y<bounds.top-1 || y>bounds.bottom+1) ++result.outside;
    }
    return result;
}

void Gallery(IWICImagingFactory* wic, ID2D1Factory* factory, const wchar_t* filename) {
    using namespace pulse::ui;
    using namespace command_icons;
    constexpr const wchar_t* names[] = {
        L"Add",L"Close",L"Minimize",L"Maximize",L"Restore",L"Back",L"Forward",L"Up",
        L"Right",L"Left",L"Down",L"Up",L"Refresh",L"Search",L"Folder",L"Open folder",
        L"Desktop",L"Computer",L"Star",L"History",L"Download",L"File",L"Image",L"Drive",
        L"Network",L"Delete",L"Recycle",L"Tag",L"Settings",L"Sun",L"Cut",L"Copy",
        L"Paste",L"Rename",L"Sort",L"Filter",L"List",L"Grid",L"Split",L"Columns",
        L"Panel",L"Close panel",L"Check",L"Eye",L"Info",L"Home",L"Tray",L"Pin",
        L"Link",L"Palette",L"Sliders",L"Warning",L"Lock",L"External",L"Favourite",L"More",
        L"Split 1",L"Split 2",L"Split 3",L"Split 4",L"Contrast",L"Swap LR",L"Swap TB",
        L"Music",L"Video"
    };
    static_assert(std::size(names) == static_cast<size_t>(Icon::Count)-1);
    Ref<IWICBitmap> bitmap;
    Ref<ID2D1RenderTarget> target;
    Ref<ID2D1SolidColorBrush> brush;
    Ref<ID2D1StrokeStyle> stroke;
    Ref<IDWriteFactory> write;
    Ref<IDWriteTextFormat> format;
    HRESULT hr = wic->CreateBitmap(1100,1280,GUID_WICPixelFormat32bppPBGRA,WICBitmapCacheOnLoad,bitmap.Put());
    if (SUCCEEDED(hr)) hr = factory->CreateWicBitmapRenderTarget(bitmap.value,
        D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96),target.Put());
    if (SUCCEEDED(hr)) hr = target->CreateSolidColorBrush(HexColor(0xFFFFFF),brush.Put());
    if (SUCCEEDED(hr)) hr = CreateStrokeStyle(target.value,stroke.Put());
    if (SUCCEEDED(hr)) hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(write.Put()));
    if (SUCCEEDED(hr)) hr = write->CreateTextFormat(L"Segoe UI",nullptr,DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,11,L"en-US",format.Put());
    Check(SUCCEEDED(hr),"gallery resources");
    if (FAILED(hr)) return;
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    target->BeginDraw();
    for (int theme_index=0;theme_index<2;++theme_index) {
        const auto theme=MakeTheme(theme_index==0,HexColor(0x8265B2));
        const float top=static_cast<float>(theme_index)*640;
        brush->SetColor(theme.surface_card);
        target->FillRectangle({0,top,1100,top+640},brush.value);
        brush->SetColor(theme.text_secondary);
        const wchar_t* title=theme_index==0 ? L"PULSE  /  DARK  /  18, 24 & 32 px" : L"PULSE  /  LIGHT  /  18, 24 & 32 px";
        target->DrawText(title,static_cast<UINT32>(wcslen(title)),format.value,{0,top+12,1100,top+36},brush.value);
        for (int i=0;i<static_cast<int>(std::size(names));++i) {
            const float x=static_cast<float>(i%11)*100, y=top+42+static_cast<float>(i/11)*98;
            Draw(target.value,brush.value,stroke.value,static_cast<Icon>(i+1),{x+8,y+17,x+26,y+35});
            Draw(target.value,brush.value,stroke.value,static_cast<Icon>(i+1),{x+34,y+14,x+58,y+38});
            Draw(target.value,brush.value,stroke.value,static_cast<Icon>(i+1),{x+64,y+10,x+96,y+42});
            target->DrawText(names[i],static_cast<UINT32>(wcslen(names[i])),format.value,{x,y+53,x+100,y+76},brush.value);
        }
    }
    hr=target->EndDraw();
    Ref<IWICStream> stream;
    Ref<IWICBitmapEncoder> encoder;
    Ref<IWICBitmapFrameEncode> frame;
    if (SUCCEEDED(hr)) hr=wic->CreateStream(stream.Put());
    if (SUCCEEDED(hr)) hr=stream->InitializeFromFilename(filename,GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr=wic->CreateEncoder(GUID_ContainerFormatPng,nullptr,encoder.Put());
    if (SUCCEEDED(hr)) hr=encoder->Initialize(stream.value,WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr=encoder->CreateNewFrame(frame.Put(),nullptr);
    if (SUCCEEDED(hr)) hr=frame->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr=frame->WriteSource(bitmap.value,nullptr);
    if (SUCCEEDED(hr)) hr=frame->Commit();
    if (SUCCEEDED(hr)) hr=encoder->Commit();
    Check(SUCCEEDED(hr),"gallery PNG export");
}
}

int wmain(int argc, wchar_t** argv) {
    using namespace pulse::ui;
    using namespace command_icons;
    Apartment apartment;
    Check(SUCCEEDED(apartment.result),"COM initialization");
    if (failures) return 1;
    Ref<IWICImagingFactory> wic;
    Ref<IWICBitmap> bitmap;
    Ref<ID2D1Factory> factory;
    Ref<ID2D1RenderTarget> target;
    Ref<ID2D1SolidColorBrush> brush;
    Ref<ID2D1StrokeStyle> stroke;
    Check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(wic.Put()))),"WIC factory");
    Check(SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory.Put())),"D2D factory");
    if (failures) return 1;
    Check(SUCCEEDED(wic->CreateBitmap(64,64,GUID_WICPixelFormat32bppPBGRA,WICBitmapCacheOnLoad,bitmap.Put())),"WIC bitmap");
    if (failures) return 1;
    const auto properties=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
    Check(SUCCEEDED(factory->CreateWicBitmapRenderTarget(bitmap.value,properties,target.Put())),"software render target");
    if (failures) return 1;
    Check(SUCCEEDED(target->CreateSolidColorBrush(HexColor(0x303B50),brush.Put())),"icon brush");
    Check(SUCCEEDED(CreateStrokeStyle(target.value,stroke.Put())),"shared round stroke");
    if (failures) return 1;

    const auto light=MakeTheme(false,HexColor(0x5867C8));
    const auto dark=MakeTheme(true,HexColor(0x5867C8));
    Check(Contrast(light.text,light.surface_card)>=7.0f,"light primary text contrast");
    Check(Contrast(dark.text,dark.surface_card)>=7.0f,"dark primary text contrast");
    Check(Contrast(light.text_secondary,light.surface_card)>=4.5f,"light secondary text contrast");
    Check(Contrast(dark.text_secondary,dark.surface_card)>=4.5f,"dark secondary text contrast");
    Check(Contrast(dark.accent,dark.surface_card)>=4.5f,"resolved dark accent remains legible");
    Check(std::abs(light.accent.r-HexColor(0x5867C8).r)<0.0001f,"light custom accent is preserved");
    Check(light.fill_selected.a!=light.fill_selected_inactive.a,"focused and inactive selections remain distinct");
    const auto high=MakeHighContrastTheme();
    Check(high.surface_card.a==1.0f && high.text.a==1.0f && high.fill_selected.a==1.0f,
          "high-contrast surfaces and selection stay opaque");
    const int theme_failures=failures;
    Check(FromGlyph(L"\xE8A7")==Icon::OpenExternal,"open-in-new-window is not a document icon");
    Check(FromGlyph(L"\xE734")==Icon::Star && FromGlyph(L"\xE735")==Icon::StarFilled,
          "filled favourite state is not reduced to a color-only cue");
    Check(FromGlyph(L"ordinary text")==Icon::None && FromGlyph(L"A")==Icon::None,
          "normal text retains the original text renderer");

    int renderings=0;
    for(const auto& theme:{light,dark}) for(const float size:{16.0f,20.0f,24.0f,32.0f,48.0f}) {
        const auto bounds=CenteredBounds({0,0,64,64},size);
        for(int value=1;value<static_cast<int>(Icon::Count);++value) {
            target->BeginDraw();
            target->SetTransform(D2D1::Matrix3x2F::Identity());
            target->Clear(theme.surface_card);
            brush->SetColor(theme.text_secondary);
            Check(Draw(target.value,brush.value,stroke.value,static_cast<Icon>(value),bounds),"icon is handled");
            Check(SUCCEEDED(target->EndDraw()),"icon render result");
            const auto pixels=Inspect(bitmap.value,theme.surface_card,bounds);
            if (pixels.ink<=4 || pixels.outside!=0) {
                ++failures;
                std::printf("[FAIL] icon=%d size=%.0f ink=%d outside=%d\n",value,size,pixels.ink,pixels.outside);
            }
            ++renderings;
        }
    }
    int star_ink[2]{};
    for(int filled=0;filled<2;++filled) {
        target->BeginDraw();target->Clear(light.surface_card);
        brush->SetColor(light.text);
        Draw(target.value,brush.value,stroke.value,filled ? Icon::StarFilled : Icon::Star,{20,20,44,44});
        Check(SUCCEEDED(target->EndDraw()),"favourite state render");
        star_ink[filled]=Inspect(bitmap.value,light.surface_card,{20,20,44,44}).ink;
    }
    Check(star_ink[1]>star_ink[0],"filled favourite has more painted area than its outline");

    // Title-bar theme toggle: one solid half, so it never reads as the gear.
    Check(FromGlyph(L"\xE793")==Icon::Contrast,"theme toggle glyph maps to the contrast icon");
    target->BeginDraw();target->SetTransform(D2D1::Matrix3x2F::Identity());target->Clear(light.surface_card);
    brush->SetColor(light.text);
    Draw(target.value,brush.value,stroke.value,Icon::Contrast,{20,20,44,44});
    Check(SUCCEEDED(target->EndDraw()),"contrast render");
    // Inspect counts ink across the bitmap; with the right half as bounds,
    // "outside" is the ink of the left half.
    const auto contrast=Inspect(bitmap.value,light.surface_card,{32,20,44,44});
    const int contrast_left=contrast.outside, contrast_right=contrast.ink-contrast.outside;
    std::printf("contrast ink left=%d right=%d\n",contrast_left,contrast_right);
    Check(contrast_left>0 && contrast_right>contrast_left*2,"contrast icon paints its right half solid");

    target->BeginDraw();
    const auto transform=D2D1::Matrix3x2F::Translation(3,5)*D2D1::Matrix3x2F::Scale(1.25f,1.25f);
    target->SetTransform(transform);brush->SetOpacity(0.37f);
    Draw(target.value,brush.value,stroke.value,Icon::Settings,{12,12,36,36},0.5f);
    D2D1_MATRIX_3X2_F restored{};target->GetTransform(&restored);
    Check(std::memcmp(&restored,&transform,sizeof(restored))==0,"caller transform restored");
    Check(std::abs(brush->GetOpacity()-0.37f)<0.0001f,"caller brush opacity restored");
    Check(!Draw(target.value,brush.value,stroke.value,Icon::None,{0,0,24,24}),"unknown command falls back");
    Draw(target.value,brush.value,stroke.value,Icon::Copy,{24,24,0,0});
    Draw(target.value,brush.value,stroke.value,Icon::Copy,{0,0,24,24},std::numeric_limits<float>::quiet_NaN());
    Check(SUCCEEDED(target->EndDraw()),"invalid bounds and opacity do not poison the render target");
    target->SetTransform(D2D1::Matrix3x2F::Identity());brush->SetOpacity(1.0f);
    target->BeginDraw();target->Clear(light.surface_card);
    Draw(target.value,brush.value,stroke.value,Icon::Settings,{20,20,44,44},0.0f);
    Check(SUCCEEDED(target->EndDraw()),"zero-opacity draw");
    Check(Inspect(bitmap.value,light.surface_card,{20,20,44,44}).ink==0,"zero opacity does not paint");
    // Disabled outlines must not get dark beads where separate segments meet.
    for (const auto icon : {Icon::Back,Icon::Refresh,Icon::Cut,Icon::Paste,Icon::Delete}) {
        target->BeginDraw();target->Clear(D2D1::ColorF(0,0.0f));
        brush->SetColor(D2D1::ColorF(0xFFFFFF));
        Draw(target.value,brush.value,stroke.value,icon,{8,8,56,56},0.4f);
        Check(SUCCEEDED(target->EndDraw()),"translucent outline render");
        Ref<IWICBitmapLock> lock;
        WICRect rect{0,0,64,64};
        const HRESULT hr=bitmap->Lock(&rect,WICBitmapLockRead,lock.Put());
        Check(SUCCEEDED(hr),"translucent pixels available");
        if (FAILED(hr)) continue;
        UINT stride=0,size=0; BYTE* data=nullptr;
        lock->GetStride(&stride);lock->GetDataPointer(&size,&data);
        int max_alpha=0;
        if (data && size>=stride*64) {
            for (int y=0;y<64;++y) for (int x=0;x<64;++x)
                max_alpha=(std::max)(max_alpha,static_cast<int>(data[y*stride+x*4+3]));
        }
        Check(max_alpha>0 && max_alpha<=103,"joined outlines preserve disabled opacity");
    }
    if (argc==2) Gallery(wic.value,factory.value,argv[1]);
    if (failures==theme_failures) std::printf("[PASS] %d icon renderings, 2 themes, 5 sizes; bounds, glyph semantics, favourite state, brush/transform isolation, invalid-input fallback, and translucent joins.\n",renderings);
    if (!theme_failures) std::printf("[PASS] theme contrast and selection states.\n");
    return failures ? 1 : 0;
}

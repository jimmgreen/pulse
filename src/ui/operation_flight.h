#pragma once
#include "ui_compositor.h"
#include "../common/windows_compat.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <wincodec.h>

namespace pulse::ui {
inline float OperationFlightEase(float progress, bool opening) {
    if (progress <= 0) return 0;
    if (progress >= 1) return 1;
    const float x1 = opening ? .2f : .4f, x2 = .6f;
    const float y1 = opening ? .8f : 0.0f;
    const auto curve = [](float t, float a, float b) {
        const float u = 1-t;
        return 3*u*u*t*a + 3*u*t*t*b + t*t*t;
    };
    float lo=0, hi=1;
    for(int i=0;i<18;++i) {
        const float mid=(lo+hi)*.5f;
        if(curve(mid,x1,opening?.2f:x2)<progress)lo=mid;else hi=mid;
    }
    return curve((lo+hi)*.5f,y1,1.0f);
}

// A private layered popup owns the captured frame. Its HWND follows the full
// screen-space path, so the source dialog's client clip cannot cut off flight.
class OperationFlightLayer {
public:
    ~OperationFlightLayer() { Stop(); }
    OperationFlightLayer() = default;
    OperationFlightLayer(const OperationFlightLayer&) = delete;
    OperationFlightLayer& operator=(const OperationFlightLayer&) = delete;
    bool Active() const { return window_ != nullptr; }
    HWND Window() const { return window_; }
    RECT Bounds() const { return current_; }
    float Opacity() const { return alpha_; }
    void Stop() {
        if(window_)DestroyWindow(window_);
        window_=nullptr;
        if(source_)DeleteObject(source_);
        if(frame_)DeleteObject(frame_);
        source_=frame_=nullptr;
        source_bits_=nullptr;
    }
    bool Start(Compositor& compositor, HWND owner, RECT full, POINT anchor,
               bool opening, UINT duration, D2D1_COLOR_F background,
               const RECT* initial=nullptr, float initial_alpha=1) {
        Stop();
        auto* dc=compositor.Dc();
        if(!dc)return false;
        ComPtr<ID2D1Image> image;
        dc->GetTarget(&image);
        ComPtr<ID2D1Bitmap1> target;
        if(!image.get() || FAILED(image->QueryInterface(IID_PPV_ARGS(&target))))return false;
        const auto size=target->GetPixelSize();
        source_w_=static_cast<int>(size.width);source_h_=static_cast<int>(size.height);
        if(!source_w_ || !source_h_)return false;
        D2D1_BITMAP_PROPERTIES1 props{};
        props.pixelFormat=target->GetPixelFormat();props.dpiX=props.dpiY=96;
        props.bitmapOptions=D2D1_BITMAP_OPTIONS_CPU_READ|D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
        ComPtr<ID2D1Bitmap1> cpu;
        if(FAILED(dc->CreateBitmap(size,nullptr,0,&props,&cpu)) || FAILED(cpu->CopyFromBitmap(nullptr,target.get(),nullptr)))return false;
        D2D1_MAPPED_RECT map{};
        if(FAILED(cpu->Map(D2D1_MAP_OPTIONS_READ,&map)))return false;
        void* bits=nullptr;
        source_=Dib(source_w_,source_h_,&bits);
        if(bits)for(int y=0;y<source_h_;++y)
            std::memcpy(static_cast<BYTE*>(bits)+y*source_w_*4,map.bits+y*map.pitch,static_cast<size_t>(source_w_)*4);
        cpu->Unmap();
        if(!source_)return false;
        source_bits_=static_cast<BYTE*>(bits);
        const bool opaque=target->GetPixelFormat().alphaMode==D2D1_ALPHA_MODE_IGNORE;
        const float radius=8.0f*static_cast<float>(pulse::compat::WindowDpi(compositor.Hwnd()))/96.0f;
        for(int y=0;y<source_h_;++y)for(int x=0;x<source_w_;++x) {
            BYTE* pixel=source_bits_+(static_cast<size_t>(y)*source_w_+x)*4;
            // Acrylic is premultiplied over the live desktop in the real
            // window. Flatten its captured tint onto the card's theme surface
            // before applying the independent flight opacity.
            const float missing=opaque?0.0f:1.0f-pixel[3]/255.0f;
            const float bg[3]{background.b,background.g,background.r};
            for(int c=0;c<3;++c)
                pixel[c]=static_cast<BYTE>(std::clamp(std::lround(pixel[c]+255.0f*bg[c]*missing),0L,255L));
            pixel[3]=255;
            const float cx=std::clamp(x+.5f,radius,source_w_-radius);
            const float cy=std::clamp(y+.5f,radius,source_h_-radius);
            const float dx=x+.5f-cx,dy=y+.5f-cy;
            const float coverage=std::clamp(radius+.5f-std::sqrt(dx*dx+dy*dy),0.0f,1.0f);
            for(int c=0;c<4;++c)pixel[c]=static_cast<BYTE>(std::lround(pixel[c]*coverage));
        }
        RECT small_bounds{};
        const LONG w=std::max(1L,static_cast<LONG>(std::lround((full.right-full.left)*.08)));
        const LONG h=std::max(1L,static_cast<LONG>(std::lround((full.bottom-full.top)*.08)));
        small_bounds={anchor.x-w/2,anchor.y-h/2,anchor.x-w/2+w,anchor.y-h/2+h};
        from_=initial?*initial:(opening?small_bounds:full);to_=opening?full:small_bounds;
        from_alpha_=initial?initial_alpha:(opening?.2f:1.0f);to_alpha_=opening?1.0f:.2f;
        opening_=opening;duration_=duration;started_=GetTickCount64();
        static const bool registered=[] {
            WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"PulseOperationFlight";
            wc.lpfnWndProc=[](HWND hwnd,UINT msg,WPARAM wp,LPARAM lp)->LRESULT {
                if(msg==WM_NCHITTEST)return HTTRANSPARENT;
                if(msg==WM_MOUSEACTIVATE)return MA_NOACTIVATE;
                return DefWindowProcW(hwnd,msg,wp,lp);
            };
            return RegisterClassW(&wc)!=0 || GetLastError()==ERROR_CLASS_ALREADY_EXISTS;
        }();
        if(!registered){Stop();return false;}
        window_=CreateWindowExW(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW,
            L"PulseOperationFlight",L"",WS_POPUP,0,0,1,1,owner,nullptr,GetModuleHandleW(nullptr),nullptr);
        if(!window_){Stop();return false;}
        Sample(0);
        if(!frame_valid_){Stop();return false;}
        ShowWindow(window_,SW_SHOWNOACTIVATE);return true;
    }
    bool Tick() {
        const float progress=std::min(1.0f,static_cast<float>(GetTickCount64()-started_)/duration_);
        Sample(progress);return progress>=1;
    }
    void Sample(float progress) {
        if(!window_)return;
        frame_valid_=false;
        const float t=OperationFlightEase(std::clamp(progress,0.0f,1.0f),opening_);
        const auto lerp=[&](LONG a,LONG b){return static_cast<LONG>(std::lround(a+(b-a)*t));};
        current_={lerp(from_.left,to_.left),lerp(from_.top,to_.top),lerp(from_.right,to_.right),lerp(from_.bottom,to_.bottom)};
        alpha_=from_alpha_+(to_alpha_-from_alpha_)*t;
        const int w=std::max(1L,current_.right-current_.left),h=std::max(1L,current_.bottom-current_.top);
        if(frame_)DeleteObject(frame_);
        void* bits=nullptr;frame_=Dib(w,h,&bits);if(!frame_)return;
        // Resample premultiplied BGRA explicitly; GDI StretchBlt does not
        // preserve the alpha byte required by UpdateLayeredWindow.
        auto* destination=static_cast<BYTE*>(bits);
        for(int y=0;y<h;++y) {
            const float sy=std::clamp((y+.5f)*source_h_/h-.5f,0.0f,static_cast<float>(source_h_-1));
            const int y0=static_cast<int>(sy),y1=std::min(y0+1,source_h_-1);
            const float fy=sy-y0;
            for(int x=0;x<w;++x) {
                const float sx=std::clamp((x+.5f)*source_w_/w-.5f,0.0f,static_cast<float>(source_w_-1));
                const int x0=static_cast<int>(sx),x1=std::min(x0+1,source_w_-1);
                const float fx=sx-x0;
                for(int c=0;c<4;++c) {
                    const float top=source_bits_[(static_cast<size_t>(y0)*source_w_+x0)*4+c]*(1-fx)+
                        source_bits_[(static_cast<size_t>(y0)*source_w_+x1)*4+c]*fx;
                    const float bottom=source_bits_[(static_cast<size_t>(y1)*source_w_+x0)*4+c]*(1-fx)+
                        source_bits_[(static_cast<size_t>(y1)*source_w_+x1)*4+c]*fx;
                    destination[(static_cast<size_t>(y)*w+x)*4+c]=static_cast<BYTE>(std::lround(top*(1-fy)+bottom*fy));
                }
            }
        }
        HDC frame_dc=CreateCompatibleDC(nullptr);
        const auto old_frame=SelectObject(frame_dc,frame_);
        POINT pos{current_.left,current_.top},origin{};SIZE size{w,h};
        BLENDFUNCTION blend{AC_SRC_OVER,0,static_cast<BYTE>(std::lround(alpha_*255)),AC_SRC_ALPHA};
        frame_valid_=UpdateLayeredWindow(window_,nullptr,&pos,&size,frame_dc,&origin,0,&blend,ULW_ALPHA)!=FALSE;
        SelectObject(frame_dc,old_frame);DeleteDC(frame_dc);
    }
    bool SaveSnapshot(const wchar_t* path) const {
        if(!frame_)return false;
        GdiFlush();
        DIBSECTION dib{};if(!GetObjectW(frame_,sizeof(dib),&dib))return false;
        ComPtr<IWICImagingFactory> factory;
        if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory))))return false;
        ComPtr<IWICBitmap> bitmap;ComPtr<IWICStream> stream;ComPtr<IWICBitmapEncoder> encoder;ComPtr<IWICBitmapFrameEncode> frame;
        if(FAILED(factory->CreateBitmapFromMemory(static_cast<UINT>(dib.dsBm.bmWidth),static_cast<UINT>(dib.dsBm.bmHeight),
            GUID_WICPixelFormat32bppPBGRA,static_cast<UINT>(dib.dsBm.bmWidthBytes),
            static_cast<UINT>(dib.dsBm.bmWidthBytes*dib.dsBm.bmHeight),static_cast<BYTE*>(dib.dsBm.bmBits),&bitmap)) ||
           FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path,GENERIC_WRITE)) ||
           FAILED(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder)) ||
           FAILED(encoder->Initialize(stream.get(),WICBitmapEncoderNoCache)) || FAILED(encoder->CreateNewFrame(&frame,nullptr)) ||
           FAILED(frame->Initialize(nullptr)))return false;
        WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;
        if(FAILED(frame->SetSize(static_cast<UINT>(dib.dsBm.bmWidth),static_cast<UINT>(dib.dsBm.bmHeight))) ||
           FAILED(frame->SetPixelFormat(&format)) || FAILED(frame->WriteSource(bitmap.get(),nullptr)))return false;
        return SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    }
private:
    static HBITMAP Dib(int w,int h,void** bits) {
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=w;
        info.bmiHeader.biHeight=-h;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        return CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,bits,nullptr,0);
    }
    HWND window_=nullptr;HBITMAP source_=nullptr,frame_=nullptr;
    BYTE* source_bits_=nullptr;
    int source_w_=0,source_h_=0;RECT from_{},to_{},current_{};
    float from_alpha_=1,to_alpha_=1,alpha_=1;bool opening_=false,frame_valid_=false;
    ULONGLONG started_=0;UINT duration_=1;
};
}

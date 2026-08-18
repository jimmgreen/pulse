// ui_compositor.h — D3D11 + D2D + DWrite device, Mica, screenshot.
#pragma once
#include "FluentTokens.h"
#include <windows.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <dcomp.h>
#include <d2d1_3.h>
#include <dwrite_3.h>
#include <wincodec.h>
#include <memory>
#include <string>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace pulse::ui {

template <typename T>
struct ComPtr {
    T* p = nullptr;
    ~ComPtr() { if (p) p->Release(); }
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& o) noexcept { p = o.p; o.p = nullptr; }
    ComPtr& operator=(ComPtr&& o) noexcept { if (p) p->Release(); p = o.p; o.p = nullptr; return *this; }
    T* operator->() const { return p; }
    T** operator&() { return &p; }
    T* get() const { return p; }
    void reset() { if (p) p->Release(); p = nullptr; }
};

class Compositor {
public:
    Compositor();
    ~Compositor();

    bool Init(HWND hwnd);
    void Shutdown();
    void Resize(int width, int height);
    void Present();
    bool SaveSnapshot(const wchar_t* path);

    ID2D1DeviceContext2* Dc() const { return dc_.get(); }
    IDWriteFactory3* DwriteFactory() const { return dwriteFactory_.get(); }

    void RecreateTextFormats(float scale);
    IDWriteTextFormat* TextFormat() const { return textFormat_.get(); }
    IDWriteTextFormat* SmallFormat() const { return smallFormat_.get(); }
    IDWriteTextFormat* HeaderFormat() const { return headerFormat_.get(); }
    IDWriteTextFormat* TabFormat() const { return tabFormat_.get(); }
    IDWriteTextFormat* AddressFormat() const { return addressFormat_.get(); }
    IDWriteTextFormat* IconFormat() const { return iconFormat_.get(); }

    // Shared by Painter / FluentMenu so CJK always falls back to YaHei.
    static const wchar_t* UiLocaleName();
    static void ApplyCjkFallback(IDWriteFactory3* factory, IDWriteTextFormat* format);
    static void ClearCjkFallbackCache();

    int Width() const { return width_; }
    int Height() const { return height_; }
    bool UsesTransparentComposition() const { return transparentComposition_; }

private:
    bool InitD3D();
    bool CreateSwapChain();
    void ResizeSwapChain();

    HWND hwnd_ = nullptr;
    int width_ = 0;
    int height_ = 0;

    ComPtr<ID3D11Device> d3dDevice_;
    ComPtr<IDXGIDevice1> dxgiDevice_;
    ComPtr<IDXGISwapChain1> swapChain_;
    ComPtr<IDCompositionDevice> compositionDevice_;
    ComPtr<IDCompositionTarget> compositionTarget_;
    ComPtr<IDCompositionVisual> compositionVisual_;
    ComPtr<ID2D1Factory3> d2dFactory_;
    ComPtr<ID2D1Device2> d2dDevice_;
    ComPtr<ID2D1DeviceContext2> dc_;
    ComPtr<ID2D1Bitmap1> targetBitmap_;
    ComPtr<IDWriteFactory3> dwriteFactory_;
    ComPtr<IDWriteRenderingParams> textRenderingParams_;
    ComPtr<IDWriteTextFormat> textFormat_;
    ComPtr<IDWriteTextFormat> smallFormat_;
    ComPtr<IDWriteTextFormat> headerFormat_;
    ComPtr<IDWriteTextFormat> tabFormat_;
    ComPtr<IDWriteTextFormat> addressFormat_;
    ComPtr<IDWriteTextFormat> iconFormat_;
    bool transparentComposition_ = false;
};

} // namespace pulse::ui

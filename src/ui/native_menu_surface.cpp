#include "native_menu_surface.h"
#include "window_material.h"
#include <chrono>
#include <cstdio>

namespace pulse::ui {
namespace {
class MenuStageTimer {
public:
    explicit MenuStageTimer(const char* scope) : scope_(scope) {
        wchar_t value[4]{};
        enabled_ = GetEnvironmentVariableW(L"PULSE_MENU_LATENCY", value, 4) && value[0] == L'1';
    }
    void Mark(const char* phase) {
        if (!enabled_) return;
        const auto now = std::chrono::steady_clock::now();
        std::printf("[LATENCY] %s %s=%.3f ms\n", scope_, phase,
            std::chrono::duration<double, std::milli>(now - previous_).count());
        previous_ = now;
    }
private:
    const char* scope_;
    bool enabled_ = false;
    std::chrono::steady_clock::time_point previous_ = std::chrono::steady_clock::now();
};
}

NativeMenuSurface::~NativeMenuSurface() { Reset(); }

void NativeMenuSurface::Reset() noexcept {
    backdrop_.reset();
    buffer_.reset();
    if (target_.get()) target_->SetRoot(nullptr);
    if (composition_.get()) composition_->Commit();
    visual_.reset();
    target_.reset();
    swap_.reset();
    composition_.reset();
    if (hwnd_ && IsWindow(hwnd_)) {
        ApplyWindowEffect(hwnd_, WindowEffect::None, false);
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, original_ex_style_);
    }
    hwnd_ = nullptr;
    fail_present_for_testing_ = false;
    width_ = height_ = 1;
}

HRESULT NativeMenuSurface::Initialize(HWND hwnd, ID2D1DeviceContext* dc, bool dark, const BackdropFactory& factory) {
    MenuStageTimer timing("initialize");
    if (Ready() && hwnd_ == hwnd) return S_OK;
    Reset();
    if (!hwnd || !dc || !factory || !SystemAllowsPopupAcrylic()) return E_NOTIMPL;
    if (!(GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_NOREDIRECTIONBITMAP)) return E_INVALIDARG;
    wchar_t fault[4]{};
    GetEnvironmentVariableW(L"PULSE_MENU_NATIVE_FAIL_PRESENT", fault, ARRAYSIZE(fault));
    fail_present_for_testing_ = fault[0] == L'1';
    ComPtr<ID2D1Device> device;
    dc->GetDevice(&device);
    ComPtr<ID2D1Device2> device2;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<ID3D11Device> d3d;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> dxgi_factory;
    HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&device2));
    if (SUCCEEDED(hr)) hr = device2->GetDxgiDevice(&dxgi);
    if (SUCCEEDED(hr)) hr = dxgi->QueryInterface(IID_PPV_ARGS(&d3d));
    if (SUCCEEDED(hr)) hr = dxgi->GetAdapter(&adapter);
    if (SUCCEEDED(hr)) hr = adapter->GetParent(IID_PPV_ARGS(&dxgi_factory));
    if (FAILED(hr)) return hr;
    hwnd_ = hwnd;
    original_ex_style_ = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE,
        (original_ex_style_ & ~WS_EX_LAYERED) | WS_EX_NOREDIRECTIONBITMAP);
    ApplyWindowEffect(hwnd, WindowEffect::None, dark);
    try { backdrop_ = factory(hwnd); } catch (...) { backdrop_.reset(); }
    if (!backdrop_) {
        Reset();
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    }
    timing.Mark("backdrop");
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = desc.Height = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    hr = dxgi_factory->CreateSwapChainForComposition(d3d.get(), &desc, nullptr, &swap_);
    if (SUCCEEDED(hr)) {
        hr = DCompositionCreateDevice(dxgi.get(), __uuidof(IDCompositionDevice),
            reinterpret_cast<void**>(&composition_));
        if (FAILED(hr)) {
            hr = Compositor::CreateCompositionOnlyDevice(&composition_);
        }
    }
    if (SUCCEEDED(hr)) hr = composition_->CreateTargetForHwnd(hwnd, TRUE, &target_);
    if (SUCCEEDED(hr)) hr = composition_->CreateVisual(&visual_);
    if (SUCCEEDED(hr)) hr = visual_->SetContent(swap_.get());
    if (SUCCEEDED(hr)) hr = target_->SetRoot(visual_.get());
    if (SUCCEEDED(hr)) hr = composition_->Commit();
    if (FAILED(hr)) Reset();
    timing.Mark("swapchain_composition");
    return hr;
}

bool NativeMenuSurface::PaintForeground(ID2D1DeviceContext* dc, const D2D1_RECT_F& card,
    float radius, bool dark) noexcept {
    return backdrop_ && backdrop_->PaintForeground(dc, card, radius, dark);
}

HRESULT NativeMenuSurface::Present(ID2D1DeviceContext* dc, ID2D1Bitmap1* bitmap, POINT origin, float radius) {
    MenuStageTimer timing("present");
    if (!Ready() || !dc || !bitmap) return E_UNEXPECTED;
    if (fail_present_for_testing_) return E_FAIL;
    const auto size = bitmap->GetPixelSize();
    if (!backdrop_ || !backdrop_->Update(size.width, size.height, radius)) return E_FAIL;
    timing.Mark("backdrop_update");
    HRESULT hr = S_OK;
    if (size.width != width_ || size.height != height_) {
        buffer_.reset();
        hr = swap_->ResizeBuffers(2, size.width, size.height, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
        if (FAILED(hr)) return hr;
        width_ = size.width;
        height_ = size.height;
    }
    if (!buffer_.get()) {
        ComPtr<IDXGISurface> surface;
        hr = swap_->GetBuffer(0, IID_PPV_ARGS(&surface));
        const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        if (SUCCEEDED(hr)) hr = dc->CreateBitmapFromDxgiSurface(surface.get(), &properties, &buffer_);
        if (FAILED(hr)) return hr;
    }
    timing.Mark("resize_buffer");
    SetWindowPos(hwnd_, nullptr, origin.x, origin.y, static_cast<int>(width_), static_cast<int>(height_),
        SWP_NOZORDER | SWP_NOACTIVATE);
    ComPtr<ID2D1Image> previous;
    dc->GetTarget(&previous);
    float dpi_x = 96, dpi_y = 96;
    dc->GetDpi(&dpi_x, &dpi_y);
    D2D1_MATRIX_3X2_F transform{};
    dc->GetTransform(&transform);
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    dc->SetDpi(96, 96);
    dc->SetTarget(buffer_.get());
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    dc->DrawBitmap(bitmap);
    hr = dc->EndDraw();
    dc->SetTarget(previous.get());
    dc->SetDpi(dpi_x, dpi_y);
    dc->SetTransform(transform);
    timing.Mark("draw");
    // DWM owns composition pacing; a UI-thread vblank wait delays hover and submenu input.
    if (SUCCEEDED(hr)) hr = swap_->Present(0, 0);
    timing.Mark("swap_present");
    if (SUCCEEDED(hr)) hr = composition_->Commit();
    return hr;
}
}

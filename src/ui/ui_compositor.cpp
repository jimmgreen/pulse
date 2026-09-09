// ui_compositor.cpp
#include "ui_compositor.h"
#include "../common/windows_compat.h"
#include "../common/localization.h"
#include "lumatext_renderer.h"
#include "typography.h"
#include <commctrl.h>
#include <prsht.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <algorithm>
#include <cstdio>

namespace pulse::ui {

Compositor::Compositor() = default;
Compositor::~Compositor() { Shutdown(); }

bool Compositor::CheckGraphics(HRESULT hr, const wchar_t* stage) {
    if (SUCCEEDED(hr)) return true;
    wchar_t message[256]{};
    swprintf_s(message, L"%s failed (HRESULT 0x%08X)", stage, static_cast<unsigned>(hr));
    initialization_error_ = message;
    PWSTR local = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
        const std::wstring directory = std::wstring(local) + L"\\Pulse";
        CoTaskMemFree(local);
        CreateDirectoryW(directory.c_str(), nullptr);
        FILE* log = nullptr;
        if (_wfopen_s(&log, (directory + L"\\pulse_graphics.log").c_str(), L"a, ccs=UTF-8") == 0) {
            fwprintf(log, L"%s\n", message);
            fclose(log);
        }
    }
    return false;
}

bool Compositor::IsDeviceLost(HRESULT hr) {
    return hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED ||
           hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

bool Compositor::Init(HWND hwnd) {
    initialization_error_.clear();
    hwnd_ = hwnd;
    device_lost_ = false;
    if (!InitD3D()) return false;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    width_ = std::max(1L, rc.right - rc.left);
    height_ = std::max(1L, rc.bottom - rc.top);
    if (!CreateSwapChain()) return false;
    ResizeSwapChain();
    return targetBitmap_.get() != nullptr;
}

void Compositor::Shutdown() {
    if (!dc_.get() && !d3dDevice_.get()) return; // already shut down
    if (lumaText_) lumaText_->Shutdown();
    targetBitmap_.reset();
    if (dc_.get()) dc_->SetTarget(nullptr);
    if (compositionTarget_.get()) compositionTarget_->SetRoot(nullptr);
    if (compositionDevice_.get()) compositionDevice_->Commit();
    compositionVisual_.reset();
    compositionTarget_.reset();
    compositionDevice_.reset();
    swapChain_.reset();
    dc_.reset();
    d2dDevice_.reset();
    d2dFactory_.reset();
    dxgiDevice_.reset();
    d3dDevice_.reset();
    dwriteFactory_.reset();
    textRenderingParams_.reset();
    text_params_monitor_ = nullptr;
    typography::InvalidateCaches();
    textFormat_.reset();
    smallFormat_.reset();
    headerFormat_.reset();
    tabFormat_.reset();
    addressFormat_.reset();
    iconFormat_.reset();
    transparentComposition_ = false;
    hwnd_ = nullptr;
    device_lost_ = false;
}

bool Compositor::InitD3D() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1 };
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
        &d3dDevice_, nullptr, nullptr);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
            &d3dDevice_, nullptr, nullptr);
    }
    if (!CheckGraphics(hr, L"D3D11CreateDevice (hardware/WARP)")) return false;

    hr = d3dDevice_->QueryInterface(&dxgiDevice_);
    if (!CheckGraphics(hr, L"IDXGIDevice1")) return false;

    D2D1_FACTORY_OPTIONS opts{};
#ifdef _DEBUG
    opts.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
        &opts, reinterpret_cast<void**>(&d2dFactory_));
    if (!CheckGraphics(hr, L"D2D1CreateFactory")) return false;

    hr = d2dFactory_->CreateDevice(dxgiDevice_.get(), &d2dDevice_);
    if (!CheckGraphics(hr, L"D2D CreateDevice")) return false;

    hr = d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_);
    if (!CheckGraphics(hr, L"D2D CreateDeviceContext")) return false;

    dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    dc_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory2),
        reinterpret_cast<IUnknown**>(&dwriteFactory_));
    if (!CheckGraphics(hr, L"DWriteCreateFactory")) return false;

    UpdateTextRenderingParams(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST));
    if (!lumaText_) lumaText_ = std::make_unique<LumaTextRenderer>();
    lumaText_->Init(dwriteFactory_.get(), dc_.get());

    return true;
}

bool Compositor::DrawLumaText(std::wstring_view text, IDWriteTextFormat* format,
                              const D2D1_RECT_F& bounds,
                              const D2D1_COLOR_F& foreground,
                              const D2D1_COLOR_F& background,
                              DWRITE_TEXT_ALIGNMENT alignment) {
    if (!lumaText_ || !lumaText_->Enabled()) return false;
    if (lumaText_->Draw(text, format, bounds, foreground, background, alignment)) {
        return true;
    }
    lumaText_->RecordFallback();
    return false;
}

bool Compositor::MeasureLumaText(std::wstring_view text, IDWriteTextFormat* format,
                                 float& width, float* height) {
    if (!lumaText_ || !lumaText_->Enabled()) return false;
    return lumaText_->Measure(text, format, width, height);
}

bool Compositor::PaintLumaEdit(HWND hwnd, HDC hdc, IDWriteTextFormat* format,
                               const D2D1_COLOR_F& foreground,
                               const D2D1_COLOR_F& background) {
    if (!lumaText_ || !lumaText_->Enabled()) return false;
    return lumaText_->PaintEdit(hwnd, hdc, format, foreground, background);
}

bool Compositor::PresentLumaEdit(HWND hwnd, IDWriteTextFormat* format,
                                 const D2D1_COLOR_F& foreground,
                                 const D2D1_COLOR_F& background) {
    if (!hwnd) return false;
    const bool ok = PaintLumaEdit(hwnd, nullptr, format, foreground, background);
    ValidateRect(hwnd, nullptr);
    return ok;
}

LRESULT Compositor::CallLumaEditMouse(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                      IDWriteTextFormat* format) {
    if (lumaText_ && lumaText_->Enabled())
        return lumaText_->CallEditDefaultMouse(hwnd, msg, wParam, lParam, format);
    SendMessageW(hwnd, WM_SETREDRAW, FALSE, 0);
    const LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
    SendMessageW(hwnd, WM_SETREDRAW, TRUE, 0);
    HideCaret(hwnd);
    return result;
}

bool Compositor::LumaTextEnabled() const noexcept {
    return lumaText_ && lumaText_->Enabled();
}

const LumaTextStats* Compositor::GetLumaTextStats() const noexcept {
    return lumaText_ ? &lumaText_->Stats() : nullptr;
}

bool Compositor::CreateSwapChain() {
    ComPtr<IDXGIAdapter> adapter;
    if (!CheckGraphics(dxgiDevice_->GetAdapter(&adapter), L"DXGI GetAdapter")) return false;
    ComPtr<IDXGIFactory2> factory;
    if (!CheckGraphics(adapter->GetParent(IID_PPV_ARGS(&factory)), L"IDXGIFactory2")) return false;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = static_cast<UINT>(std::max(1, width_));
    desc.Height = static_cast<UINT>(std::max(1, height_));
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = compat::ModernWindows()
        ? DXGI_SWAP_EFFECT_FLIP_DISCARD : DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

    HRESULT hr = DCompositionCreateDevice(dxgiDevice_.get(), __uuidof(IDCompositionDevice),
        reinterpret_cast<void**>(&compositionDevice_));
    if (SUCCEEDED(hr)) {
        hr = factory->CreateSwapChainForComposition(
            d3dDevice_.get(), &desc, nullptr, &swapChain_);
        if (FAILED(hr) && desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD) {
            swapChain_.reset();
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            hr = factory->CreateSwapChainForComposition(d3dDevice_.get(), &desc, nullptr, &swapChain_);
        }
    }
    if (SUCCEEDED(hr)) hr = compositionDevice_->CreateTargetForHwnd(hwnd_, TRUE, &compositionTarget_);
    if (SUCCEEDED(hr)) hr = compositionDevice_->CreateVisual(&compositionVisual_);
    if (SUCCEEDED(hr)) hr = compositionVisual_->SetContent(swapChain_.get());
    if (SUCCEEDED(hr)) hr = compositionTarget_->SetRoot(compositionVisual_.get());
    if (SUCCEEDED(hr)) hr = compositionDevice_->Commit();

    if (SUCCEEDED(hr)) {
        transparentComposition_ = true;
    } else {
        compositionVisual_.reset();
        compositionTarget_.reset();
        compositionDevice_.reset();
        swapChain_.reset();
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        hr = factory->CreateSwapChainForHwnd(d3dDevice_.get(), hwnd_, &desc,
            nullptr, nullptr, &swapChain_);
        if (!CheckGraphics(hr, L"CreateSwapChainForHwnd")) return false;
    }

    dxgiDevice_->SetMaximumFrameLatency(1);
    return true;
}

void Compositor::ResizeSwapChain() {
    if (!swapChain_.get() || width_ <= 0 || height_ <= 0) return;
    targetBitmap_.reset();
    dc_->SetTarget(nullptr);
    HRESULT hr = swapChain_->ResizeBuffers(0, (UINT)width_, (UINT)height_,
        DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        CheckGraphics(hr, L"ResizeBuffers");
        if (IsDeviceLost(hr)) NotifyDeviceLost(hr);
        return;
    }

    ComPtr<IDXGISurface> surface;
    hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(&surface));
    if (FAILED(hr)) {
        CheckGraphics(hr, L"SwapChain GetBuffer");
        if (IsDeviceLost(hr)) NotifyDeviceLost(hr);
        return;
    }
    // Layout, hit-testing, and text formats already apply the window scale.
    // Keep the D2D target at 96 DPI so per-monitor scaling happens exactly once.
    // Using the monitor DPI here as well produced scale^2 sizing and clipped
    // off-screen flyout text on 125/150/200% displays.
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
            transparentComposition_ ? D2D1_ALPHA_MODE_PREMULTIPLIED : D2D1_ALPHA_MODE_IGNORE),
        96.0f, 96.0f);
    hr = dc_->CreateBitmapFromDxgiSurface(surface.get(), &props, &targetBitmap_);
    if (FAILED(hr)) {
        CheckGraphics(hr, L"CreateBitmapFromDxgiSurface");
        if (IsDeviceLost(hr)) NotifyDeviceLost(hr);
        return;
    }
    dc_->SetTarget(targetBitmap_.get());
}

void Compositor::Resize(int width, int height) {
    width_ = width;
    height_ = height;
    if (swapChain_.get()) ResizeSwapChain();
}

bool Compositor::UpdateTextRenderingParams(HMONITOR monitor) {
    if (!dwriteFactory_.get() || !dc_.get()) return false;
    if (!monitor && hwnd_) monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    if (textRenderingParams_.get() && monitor == text_params_monitor_) return true;

    ComPtr<IDWriteRenderingParams2> next;
    if (FAILED(typography::CreateRenderingParams(dwriteFactory_.get(), monitor, &next)) ||
        !next.get()) {
        return false;
    }
    const bool monitor_changed = text_params_monitor_ && text_params_monitor_ != monitor;
    textRenderingParams_ = std::move(next);
    text_params_monitor_ = monitor;
    dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    dc_->SetTextRenderingParams(textRenderingParams_.get());
    if (monitor_changed && hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

void Compositor::Present() {
    if (device_lost_) return;
    if (hwnd_) UpdateTextRenderingParams(
        MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST));
    HRESULT hr = S_OK;
    if (swapChain_.get()) hr = swapChain_->Present(1, 0);
    if (FAILED(hr)) {
        if (IsDeviceLost(hr)) NotifyDeviceLost(hr);
        return;
    }
    if (compositionDevice_.get()) {
        hr = compositionDevice_->Commit();
        if (FAILED(hr) && IsDeviceLost(hr)) NotifyDeviceLost(hr);
    }
}

void Compositor::NotifyDeviceLost(HRESULT) {
    if (device_lost_) return;
    device_lost_ = true;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

bool Compositor::Recover() {
    if (!device_lost_) return true;
    const HWND hwnd = hwnd_;
    if (!hwnd) return false;
    Shutdown();
    if (!Init(hwnd)) {
        device_lost_ = true;
        return false;
    }
    return true;
}

bool Compositor::SaveSnapshot(const wchar_t* path) {
    if (!swapChain_.get() || !d3dDevice_.get()) return false;

    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr)) return false;

    D3D11_TEXTURE2D_DESC desc{};
    backBuffer->GetDesc(&desc);

    ComPtr<ID3D11DeviceContext> ctx;
    d3dDevice_->GetImmediateContext(&ctx);

    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    hr = d3dDevice_->CreateTexture2D(&desc, nullptr, &staging);
    if (FAILED(hr)) return false;

    ctx->CopyResource(staging.get(), backBuffer.get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return false;

    ComPtr<IWICImagingFactory> wic;
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wic));
    if (FAILED(hr)) { ctx->Unmap(staging.get(), 0); return false; }

    ComPtr<IWICStream> stream;
    hr = wic->CreateStream(&stream);
    if (FAILED(hr)) { ctx->Unmap(staging.get(), 0); return false; }
    hr = stream->InitializeFromFilename(path, GENERIC_WRITE);
    if (FAILED(hr)) { ctx->Unmap(staging.get(), 0); return false; }

    ComPtr<IWICBitmapEncoder> encoder;
    hr = wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (FAILED(hr)) { ctx->Unmap(staging.get(), 0); return false; }
    hr = encoder->Initialize(stream.get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) { ctx->Unmap(staging.get(), 0); return false; }

    ComPtr<IWICBitmapFrameEncode> frame;
    hr = encoder->CreateNewFrame(&frame, nullptr);
    if (FAILED(hr)) { ctx->Unmap(staging.get(), 0); return false; }
    hr = frame->Initialize(nullptr);
    if (FAILED(hr)) { ctx->Unmap(staging.get(), 0); return false; }

    hr = frame->SetSize(desc.Width, desc.Height);
    if (FAILED(hr)) { ctx->Unmap(staging.get(), 0); return false; }

    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    hr = frame->SetPixelFormat(&format);
    if (FAILED(hr)) { ctx->Unmap(staging.get(), 0); return false; }

    hr = frame->WritePixels(desc.Height, mapped.RowPitch, mapped.RowPitch * desc.Height,
        static_cast<BYTE*>(mapped.pData));
    ctx->Unmap(staging.get(), 0);
    if (FAILED(hr)) return false;

    hr = frame->Commit();
    if (FAILED(hr)) return false;
    hr = encoder->Commit();
    return SUCCEEDED(hr);
}

static void CreateFormat(IDWriteFactory2* factory, float size, DWRITE_FONT_WEIGHT weight,
                         typography::FontRole role, ComPtr<IDWriteTextFormat>& fmt) {
    if (!factory) return;
    typography::CreateTextFormat(factory, {role, size, weight}, &fmt);
    if (fmt.get()) {
        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        DWRITE_TRIMMING trimming{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        fmt->SetTrimming(&trimming, nullptr);
    }
}

void Compositor::RecreateTextFormats(float scale) {
    UpdateTextRenderingParams(hwnd_
        ? MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST) : nullptr);
    textFormat_.reset();
    smallFormat_.reset();
    headerFormat_.reset();
    tabFormat_.reset();
    addressFormat_.reset();
    iconFormat_.reset();
    CreateFormat(dwriteFactory_.get(), 14.0f * scale, DWRITE_FONT_WEIGHT_NORMAL,
        typography::FontRole::Text, textFormat_);
    if (textFormat_.get()) textFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    CreateFormat(dwriteFactory_.get(), 12.0f * scale, DWRITE_FONT_WEIGHT_NORMAL,
        typography::FontRole::Text, smallFormat_);
    if (smallFormat_.get()) smallFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    CreateFormat(dwriteFactory_.get(), 14.0f * scale, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        typography::FontRole::Display, headerFormat_);
    CreateFormat(dwriteFactory_.get(), 13.0f * scale, DWRITE_FONT_WEIGHT_NORMAL,
        typography::FontRole::Text, tabFormat_);
    if (tabFormat_.get()) {
        tabFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        tabFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    CreateFormat(dwriteFactory_.get(), 14.0f * scale, DWRITE_FONT_WEIGHT_NORMAL,
        typography::FontRole::Text, addressFormat_);

    // Icon font for Fluent glyphs.
    typography::CreateTextFormat(dwriteFactory_.get(),
        {typography::FontRole::Icon, 16.0f * scale}, &iconFormat_);
    if (iconFormat_.get()) {
        iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        iconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
}

} // namespace pulse::ui

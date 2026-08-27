// window_material.cpp — DWM system backdrop and wallpaper sampling.
//
// Custom-image sampling uses the homemade-Mica D2D graph from
// wangwenx190/d2d-mica (MIT, Copyright (C) 2022 Yuhang Zhao):
//   GaussianBlur -> Blend(LUMINOSITY, flood x luminosityOpacity)
//                -> Blend(COLOR, flood x tintOpacity)
// Tint recipes match WinUI MicaController / DesktopAcrylicController.
#include "window_material.h"

#include <d2d1_1.h>
#include <d2d1effects.h>
#include <d2d1effects_2.h>
#include <d3d11.h>
#include <dxgi.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace pulse::ui {
namespace {

// d2d1.lib does not export effect CLSIDs. Values from d2d1effects.h / d2d1effects_2.h.
constexpr GUID kGaussianBlurClsid = { 0x1feb6d69, 0x2fe6, 0x4ac9,
    { 0x8c, 0x58, 0x1d, 0x7f, 0x93, 0xe7, 0xa6, 0xa5 } };
constexpr GUID kFloodClsid = { 0x61c23c20, 0xae69, 0x4d8e,
    { 0x94, 0xcf, 0x50, 0x07, 0x8d, 0xf6, 0x38, 0xf2 } };
constexpr GUID kBlendClsid = { 0x81c5b77b, 0x13f8, 0x4cdd,
    { 0xad, 0x20, 0xc8, 0x90, 0x54, 0x7a, 0xc6, 0x5d } };
constexpr GUID kOpacityClsid = { 0x811d79a4, 0xde28, 0x4454,
    { 0x80, 0x94, 0xc6, 0x46, 0x85, 0xf8, 0xbd, 0x4c } };

constexpr UINT kMaxSource = 2048;
constexpr float kSampleLongEdge = 1024.0f;

struct MaterialGraph {
    ComPtr<ID2D1Effect> blur;
    ComPtr<ID2D1Effect> lum_flood;
    ComPtr<ID2D1Effect> lum_opacity;
    ComPtr<ID2D1Effect> lum_blend;
    ComPtr<ID2D1Effect> tint_flood;
    ComPtr<ID2D1Effect> tint_opacity;
    ComPtr<ID2D1Effect> color_blend;
};

HRESULT BuildMaterialGraph(ID2D1DeviceContext* dc, ID2D1Image* input,
                           const D2D1_COLOR_F& tint, float blur_std,
                           float luminosity_opacity, float tint_opacity,
                           MaterialGraph& graph) {
    if (!dc || !input) return E_POINTER;

    HRESULT hr = dc->CreateEffect(kGaussianBlurClsid, &graph.blur);
    if (FAILED(hr)) return hr;
    graph.blur->SetInput(0, input);
    graph.blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, blur_std);
    graph.blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
    graph.blur->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION,
                         D2D1_GAUSSIANBLUR_OPTIMIZATION_SPEED);

    const D2D1_VECTOR_4F color{ tint.r, tint.g, tint.b, 1.0f };

    hr = dc->CreateEffect(kFloodClsid, &graph.lum_flood);
    if (FAILED(hr)) return hr;
    graph.lum_flood->SetValue(D2D1_FLOOD_PROP_COLOR, color);
    hr = dc->CreateEffect(kOpacityClsid, &graph.lum_opacity);
    if (FAILED(hr)) return hr;
    graph.lum_opacity->SetInputEffect(0, graph.lum_flood.get());
    graph.lum_opacity->SetValue(D2D1_OPACITY_PROP_OPACITY, luminosity_opacity);

    hr = dc->CreateEffect(kBlendClsid, &graph.lum_blend);
    if (FAILED(hr)) return hr;
    graph.lum_blend->SetValue(D2D1_BLEND_PROP_MODE, D2D1_BLEND_MODE_LUMINOSITY);
    graph.lum_blend->SetInputEffect(0, graph.blur.get());
    graph.lum_blend->SetInputEffect(1, graph.lum_opacity.get());

    hr = dc->CreateEffect(kFloodClsid, &graph.tint_flood);
    if (FAILED(hr)) return hr;
    graph.tint_flood->SetValue(D2D1_FLOOD_PROP_COLOR, color);
    hr = dc->CreateEffect(kOpacityClsid, &graph.tint_opacity);
    if (FAILED(hr)) return hr;
    graph.tint_opacity->SetInputEffect(0, graph.tint_flood.get());
    graph.tint_opacity->SetValue(D2D1_OPACITY_PROP_OPACITY, tint_opacity);

    hr = dc->CreateEffect(kBlendClsid, &graph.color_blend);
    if (FAILED(hr)) return hr;
    graph.color_blend->SetValue(D2D1_BLEND_PROP_MODE, D2D1_BLEND_MODE_COLOR);
    graph.color_blend->SetInputEffect(0, graph.lum_blend.get());
    graph.color_blend->SetInputEffect(1, graph.tint_opacity.get());
    return S_OK;
}

IWICBitmapSource* ToDisplayPixels(IWICImagingFactory* wic, IWICBitmapFrameDecode* frame,
                                  ComPtr<IWICColorTransform>& transform,
                                  ComPtr<IWICFormatConverter>& converter) {
    if (!wic || !frame) return nullptr;

    UINT count = 0;
    frame->GetColorContexts(0, nullptr, &count);
    if (count > 0) {
        IWICColorContext* raw = nullptr;
        UINT actual = 0;
        ComPtr<IWICColorContext> src;
        ComPtr<IWICColorContext> srgb;
        if (SUCCEEDED(frame->GetColorContexts(1, &raw, &actual)) && actual > 0 && raw) {
            src.p = raw;
            if (SUCCEEDED(wic->CreateColorContext(&srgb)) &&
                SUCCEEDED(srgb->InitializeFromExifColorSpace(1)) &&
                SUCCEEDED(wic->CreateColorTransformer(&transform)) &&
                SUCCEEDED(transform->Initialize(frame, src.get(), srgb.get(),
                                                GUID_WICPixelFormat32bppPBGRA))) {
                return transform.get();
            }
            transform.reset();
        }
    }

    if (FAILED(wic->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        converter.reset();
        return nullptr;
    }
    return converter.get();
}

bool IsWindows11OrLater() noexcept {
    using RtlGetVersionFn = LONG (WINAPI*)(OSVERSIONINFOW*);
    static const bool value = []() noexcept {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) return false;
        auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
        if (!fn) return false;
        OSVERSIONINFOW info{};
        info.dwOSVersionInfoSize = sizeof(info);
        if (fn(&info) != 0) return false;
        return info.dwMajorVersion > 10 ||
               (info.dwMajorVersion == 10 && info.dwBuildNumber >= 22000);
    }();
    return value;
}

DWORD BackdropTypeForEffect(WindowEffect effect) noexcept {
    switch (effect) {
    case WindowEffect::Acrylic: return DWMSBT_TRANSIENTWINDOW;
    case WindowEffect::Mica:    return DWMSBT_MAINWINDOW;
    case WindowEffect::MicaAlt: return DWMSBT_TABBEDWINDOW;
    default:                    return DWMSBT_NONE;
    }
}

D2D1_RECT_F CoverRect(float img_w, float img_h, const D2D1_RECT_F& dest) {
    const float dw = dest.right - dest.left;
    const float dh = dest.bottom - dest.top;
    if (img_w <= 0.0f || img_h <= 0.0f || dw <= 0.0f || dh <= 0.0f) return dest;
    const float scale = (std::max)(dw / img_w, dh / img_h);
    const float w = img_w * scale;
    const float h = img_h * scale;
    const float x = dest.left + (dw - w) * 0.5f;
    const float y = dest.top + (dh - h) * 0.5f;
    return D2D1::RectF(x, y, x + w, y + h);
}

HRESULT CreateTargetBitmap(ID2D1DeviceContext* dc, UINT w, UINT h, ID2D1Bitmap1** out) {
    D2D1_BITMAP_PROPERTIES1 props{};
    props.pixelFormat = { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED };
    props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
    props.dpiX = 96.0f;
    props.dpiY = 96.0f;
    return dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, props, out);
}

struct TargetGuard {
    ID2D1DeviceContext* dc = nullptr;
    ComPtr<ID2D1Image> previous;
    D2D1_MATRIX_3X2_F transform{};
    bool active = false;

    explicit TargetGuard(ID2D1DeviceContext* context) : dc(context) {
        if (!dc) return;
        dc->GetTarget(&previous);
        dc->GetTransform(&transform);
        active = true;
    }
    ~TargetGuard() { Restore(); }
    TargetGuard(const TargetGuard&) = delete;
    TargetGuard& operator=(const TargetGuard&) = delete;

    void Restore() {
        if (!active || !dc) return;
        dc->SetTarget(previous.get());
        dc->SetTransform(transform);
        active = false;
    }
};

FILE* MaterialLogFile() {
    static FILE* file = []() -> FILE* {
        wchar_t dir[MAX_PATH]{};
        wchar_t path[MAX_PATH]{};
        if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH))
            return nullptr;
        swprintf_s(path, L"%s\\Pulse", dir);
        CreateDirectoryW(path, nullptr);
        swprintf_s(path, L"%s\\Pulse\\material.log", dir);
        FILE* f = nullptr;
        _wfopen_s(&f, path, L"a");
        return f;
    }();
    return file;
}

struct DecodedPixels {
    UINT width = 0;
    UINT height = 0;
    UINT stride = 0;
    std::vector<uint8_t> pixels;
    HRESULT result = E_FAIL;
};

DecodedPixels DecodeImageFile(const std::wstring& path) {
    DecodedPixels decoded;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICColorTransform> color;
    ComPtr<IWICFormatConverter> converter;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&wic));
    if (SUCCEEDED(hr)) {
        hr = wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                            WICDecodeMetadataCacheOnDemand, &decoder);
    }
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) {
        decoded.result = hr;
        return decoded;
    }

    IWICBitmapSource* pixels = ToDisplayPixels(wic.get(), frame.get(), color, converter);
    if (!pixels) {
        decoded.result = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
        return decoded;
    }

    UINT width = 0;
    UINT height = 0;
    hr = pixels->GetSize(&width, &height);
    if (FAILED(hr) || width == 0 || height == 0) {
        decoded.result = FAILED(hr) ? hr : WINCODEC_ERR_BADIMAGE;
        return decoded;
    }

    IWICBitmapSource* source = pixels;
    if (width > kMaxSource || height > kMaxSource) {
        const float scale = (std::min)(
            static_cast<float>(kMaxSource) / static_cast<float>((std::max)(1u, width)),
            static_cast<float>(kMaxSource) / static_cast<float>((std::max)(1u, height)));
        width = (std::max)(1u, static_cast<UINT>(width * scale));
        height = (std::max)(1u, static_cast<UINT>(height * scale));
        hr = wic->CreateBitmapScaler(&scaler);
        if (SUCCEEDED(hr)) {
            hr = scaler->Initialize(pixels, width, height, WICBitmapInterpolationModeFant);
        }
        if (FAILED(hr)) {
            decoded.result = hr;
            return decoded;
        }
        source = scaler.get();
    }

    const uint64_t stride = static_cast<uint64_t>(width) * 4u;
    const uint64_t bytes = stride * static_cast<uint64_t>(height);
    if (stride > (std::numeric_limits<UINT>::max)() ||
        bytes > (std::numeric_limits<UINT>::max)() ||
        bytes > (std::numeric_limits<size_t>::max)()) {
        decoded.result = E_OUTOFMEMORY;
        return decoded;
    }
    try {
        decoded.pixels.resize(static_cast<size_t>(bytes));
    } catch (...) {
        decoded.result = E_OUTOFMEMORY;
        return decoded;
    }
    hr = source->CopyPixels(nullptr, static_cast<UINT>(stride), static_cast<UINT>(bytes),
                            decoded.pixels.data());
    if (FAILED(hr)) {
        decoded.pixels.clear();
        decoded.result = hr;
        return decoded;
    }
    decoded.width = width;
    decoded.height = height;
    decoded.stride = static_cast<UINT>(stride);
    decoded.result = S_OK;
    return decoded;
}

} // namespace

struct WindowMaterial::DecodeWorker {
    struct Request {
        std::wstring path;
        uint64_t generation = 0;
        HWND notify = nullptr;
    };
    struct Result {
        std::wstring path;
        uint64_t generation = 0;
        DecodedPixels decoded;
    };

    ~DecodeWorker() {
        if (thread) CloseHandle(thread);
        if (wake) CloseHandle(wake);
    }

    std::mutex mutex;
    Request request;
    std::optional<Result> completed;
    uint64_t next_generation = 0;
    bool stop = false;
    HANDLE wake = nullptr;
    HANDLE thread = nullptr;
    std::atomic<uint64_t> attempts{0};
    std::atomic<DWORD> test_delay_ms{0};
};

void LogWindowMaterial(const char* fmt, ...) {
    FILE* f = MaterialLogFile();
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char line[1024];
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (n < 0) return;
    char out[1200];
    sprintf_s(out, "%02u:%02u:%02u.%03u %s\n", st.wHour, st.wMinute, st.wSecond,
              st.wMilliseconds, line);
    if (f) {
        fputs(out, f);
        fflush(f);
    }
    OutputDebugStringA(out);
    fputs(out, stderr);
    fflush(stderr);
}

const wchar_t* WindowEffectId(WindowEffect effect) noexcept {
    switch (effect) {
    case WindowEffect::Acrylic: return L"acrylic-material";
    case WindowEffect::Mica:    return L"mica";
    case WindowEffect::MicaAlt: return L"mica-alt";
    default:                    return L"none";
    }
}

const wchar_t* WindowEffectLabel(WindowEffect effect) noexcept {
    switch (effect) {
    case WindowEffect::Acrylic: return L"\u4e9a\u514b\u529b";
    case WindowEffect::Mica:    return L"Mica";
    case WindowEffect::MicaAlt: return L"Mica Alt";
    default:                    return L"\u65e0";
    }
}

WindowEffect WindowEffectFromId(std::wstring_view id) noexcept {
    if (id == L"acrylic-material" || id == L"acrylic" || id == L"dwm-blur")
        return WindowEffect::Acrylic;
    if (id == L"mica") return WindowEffect::Mica;
    if (id == L"mica-alt" || id == L"miac-alt") return WindowEffect::MicaAlt;
    if (id == L"none") return WindowEffect::None;
    return WindowEffect::MicaAlt;
}

bool WindowEffectUsesBackdrop(WindowEffect effect) noexcept {
    return effect != WindowEffect::None;
}

bool ApplyWindowEffect(HWND hwnd, WindowEffect effect, bool dark) noexcept {
    if (!hwnd) return false;
    UpdateWindowTheme(hwnd, dark);
    const DWORD corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

    if (IsHighContrast() || !IsWindows11OrLater()) {
        effect = WindowEffect::None;
    }

    DWORD backdrop = BackdropTypeForEffect(effect);
    HRESULT hr = DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE,
                                       &backdrop, sizeof(backdrop));
    if (FAILED(hr) && backdrop == DWMSBT_TABBEDWINDOW) {
        backdrop = DWMSBT_MAINWINDOW;
        hr = DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE,
                                   &backdrop, sizeof(backdrop));
        if (SUCCEEDED(hr)) effect = WindowEffect::Mica;
    }
    if (FAILED(hr)) {
        effect = WindowEffect::None;
        backdrop = DWMSBT_NONE;
        DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
    }

    if (effect == WindowEffect::None) {
        const COLORREF caption = dark ? RGB(0x1A, 0x1A, 0x1A) : RGB(0xF3, 0xF3, 0xF3);
        DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
    } else {
        const COLORREF caption = 0xFFFFFFFFu;
        DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
    }

    const MARGINS margins{ -1 };
    DwmExtendFrameIntoClientArea(hwnd, &margins);
    return WindowEffectUsesBackdrop(effect) && SUCCEEDED(hr);
}

int RunMaterialSelfTest() {
    LogWindowMaterial("selftest begin");

    ComPtr<ID3D11Device> d3d;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                   D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
    if (FAILED(hr)) {
        LogWindowMaterial("D3D11CreateDevice hr=0x%08X", static_cast<unsigned>(hr));
        return 1;
    }
    ComPtr<IDXGIDevice> dxgi;
    d3d->QueryInterface(&dxgi);
    ComPtr<ID2D1Factory1> factory;
    D2D1_FACTORY_OPTIONS opts{};
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                           &opts, reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || !dxgi.get()) {
        LogWindowMaterial("D2D1CreateFactory hr=0x%08X", static_cast<unsigned>(hr));
        return 1;
    }
    ComPtr<ID2D1Device> device;
    hr = factory->CreateDevice(dxgi.get(), &device);
    ComPtr<ID2D1DeviceContext> dc;
    if (SUCCEEDED(hr)) hr = device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc);
    if (FAILED(hr) || !dc.get()) {
        LogWindowMaterial("CreateDeviceContext hr=0x%08X", static_cast<unsigned>(hr));
        return 1;
    }

    const UINT n = 64;
    std::vector<UINT32> pixels(n * n);
    for (UINT y = 0; y < n; ++y) {
        for (UINT x = 0; x < n; ++x) {
            const bool on = ((x / 8) + (y / 8)) & 1;
            pixels[y * n + x] = on ? 0xFFFFFFFFu : 0xFF000000u;
        }
    }
    D2D1_BITMAP_PROPERTIES1 src_props{};
    src_props.pixelFormat = { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED };
    src_props.dpiX = 96.0f;
    src_props.dpiY = 96.0f;
    ComPtr<ID2D1Bitmap1> checker;
    hr = dc->CreateBitmap(D2D1::SizeU(n, n), pixels.data(), n * 4, src_props, &checker);
    if (FAILED(hr)) {
        LogWindowMaterial("CreateBitmap checker hr=0x%08X", static_cast<unsigned>(hr));
        return 1;
    }

    auto variance_of = [&](ID2D1Bitmap* image, const char* tag) -> double {
        if (!image) {
            LogWindowMaterial("%s image=null", tag);
            return -1.0;
        }
        D2D1_BITMAP_PROPERTIES1 cpu_props{};
        cpu_props.pixelFormat = { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED };
        cpu_props.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
        cpu_props.dpiX = 96.0f;
        cpu_props.dpiY = 96.0f;
        ComPtr<ID2D1Bitmap1> cpu;
        if (FAILED(dc->CreateBitmap(D2D1::SizeU(n, n), nullptr, 0, cpu_props, &cpu)) ||
            FAILED(cpu->CopyFromBitmap(nullptr, image, nullptr))) {
            LogWindowMaterial("%s CopyFromBitmap failed", tag);
            return -1.0;
        }
        D2D1_MAPPED_RECT mapped{};
        if (FAILED(cpu->Map(D2D1_MAP_OPTIONS_READ, &mapped))) {
            LogWindowMaterial("%s Map failed", tag);
            return -1.0;
        }
        double sum = 0.0;
        double sum2 = 0.0;
        UINT count = 0;
        UINT32 first = 0;
        UINT32 last = 0;
        for (UINT y = 0; y < n; ++y) {
            const auto* row = reinterpret_cast<const UINT32*>(
                mapped.bits + static_cast<size_t>(y) * mapped.pitch);
            for (UINT x = 0; x < n; ++x) {
                const UINT32 p = row[x];
                if (count == 0) first = p;
                last = p;
                const double luma = ((p >> 16) & 255) * 0.2126 +
                                    ((p >> 8) & 255) * 0.7152 +
                                    (p & 255) * 0.0722;
                sum += luma;
                sum2 += luma * luma;
                ++count;
            }
        }
        cpu->Unmap();
        const double mean = sum / count;
        const double var = sum2 / count - mean * mean;
        LogWindowMaterial("%s first=%08X last=%08X mean=%.2f var=%.2f",
                          tag, first, last, mean, var);
        return var;
    };

    auto blur_bitmap = [&](ID2D1Image* input, float std_dev, bool end_draw, const char* tag) -> double {
        ComPtr<ID2D1Effect> blur;
        HRESULT ehr = dc->CreateEffect(kGaussianBlurClsid, &blur);
        LogWindowMaterial("%s CreateEffect blur hr=0x%08X", tag, static_cast<unsigned>(ehr));
        if (FAILED(ehr)) return -1.0;
        blur->SetInput(0, input);
        HRESULT vhr = blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, std_dev);
        blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
        LogWindowMaterial("%s SetValue std=%.1f hr=0x%08X", tag, std_dev, static_cast<unsigned>(vhr));

        ComPtr<ID2D1Bitmap1> baked;
        if (FAILED(CreateTargetBitmap(dc.get(), n, n, &baked))) {
            LogWindowMaterial("%s CreateTargetBitmap failed", tag);
            return -1.0;
        }
        dc->BeginDraw();
        dc->SetTarget(baked.get());
        dc->Clear(D2D1::ColorF(1.0f, 0.0f, 1.0f, 1.0f));
        dc->DrawImage(blur.get(), D2D1::Point2F(0.0f, 0.0f), D2D1_INTERPOLATION_MODE_LINEAR);
        dc->SetTarget(nullptr);
        HRESULT end_hr = S_OK;
        if (end_draw) end_hr = dc->EndDraw();
        else end_hr = dc->Flush(nullptr, nullptr);
        LogWindowMaterial("%s End/Flush hr=0x%08X end_draw=%d", tag,
                          static_cast<unsigned>(end_hr), end_draw ? 1 : 0);
        return variance_of(baked.get(), tag);
    };

    const double src_var = variance_of(checker.get(), "source");
    const double blur1 = blur_bitmap(checker.get(), 1.0f, true, "bitmap-std1");
    const double blur40 = blur_bitmap(checker.get(), 40.0f, true, "bitmap-std40");

    ComPtr<ID2D1CommandList> list;
    dc->CreateCommandList(&list);
    dc->BeginDraw();
    dc->SetTarget(list.get());
    dc->DrawBitmap(checker.get());
    dc->SetTarget(nullptr);
    list->Close();
    dc->EndDraw();
    const double cl40 = blur_bitmap(list.get(), 40.0f, true, "cmdlist-std40");

    const double nested = [&]() -> double {
        ComPtr<ID2D1Effect> blur;
        if (FAILED(dc->CreateEffect(kGaussianBlurClsid, &blur))) return -1.0;
        blur->SetInput(0, checker.get());
        blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, 40.0f);
        blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
        ComPtr<ID2D1Bitmap1> baked;
        if (FAILED(CreateTargetBitmap(dc.get(), n, n, &baked))) return -1.0;
        dc->BeginDraw();
        dc->SetTarget(baked.get());
        dc->Clear(D2D1::ColorF(1.0f, 0.0f, 1.0f, 1.0f));
        dc->DrawImage(blur.get(), D2D1::Point2F(0.0f, 0.0f), D2D1_INTERPOLATION_MODE_LINEAR);
        dc->SetTarget(nullptr);
        const double before_end = variance_of(baked.get(), "nested-before-EndDraw");
        dc->EndDraw();
        const double after_end = variance_of(baked.get(), "nested-after-EndDraw");
        LogWindowMaterial("nested before=%.2f after=%.2f", before_end, after_end);
        return before_end;
    }();

    const double mica_var = [&]() -> double {
        MaterialGraph graph;
        const D2D1_COLOR_F tint = D2D1::ColorF(0.125f, 0.125f, 0.125f, 1.0f);
        HRESULT ghr = BuildMaterialGraph(dc.get(), checker.get(), tint, 8.0f, 1.0f, 0.8f, graph);
        LogWindowMaterial("mica-graph Create hr=0x%08X", static_cast<unsigned>(ghr));
        if (FAILED(ghr) || !graph.color_blend.get()) return -1.0;
        ComPtr<ID2D1Bitmap1> baked;
        if (FAILED(CreateTargetBitmap(dc.get(), n, n, &baked))) return -1.0;
        dc->BeginDraw();
        dc->SetTarget(baked.get());
        dc->Clear(D2D1::ColorF(1.0f, 0.0f, 1.0f, 1.0f));
        dc->DrawImage(graph.color_blend.get(), D2D1::Point2F(0.0f, 0.0f),
                      D2D1_INTERPOLATION_MODE_LINEAR);
        dc->SetTarget(nullptr);
        dc->EndDraw();
        return variance_of(baked.get(), "mica-graph");
    }();

    LogWindowMaterial("summary src_var=%.2f blur1=%.2f blur40=%.2f cmdlist40=%.2f nested=%.2f mica=%.2f",
                      src_var, blur1, blur40, cl40, nested, mica_var);
    const bool blur_works = blur40 >= 0.0 && blur40 < src_var * 0.5;
    const bool cmd_works = cl40 >= 0.0 && cl40 < src_var * 0.5;
    const bool graph_works = mica_var >= 0.0;
    LogWindowMaterial("verdict bitmap_blur=%s cmdlist_blur=%s mica_graph=%s",
                      blur_works ? "PASS" : "FAIL",
                      cmd_works ? "PASS" : "FAIL",
                      graph_works ? "PASS" : "FAIL");
    return (blur_works && cmd_works && graph_works) ? 0 : 2;
}

WindowMaterial::~WindowMaterial() {
    auto worker = decode_worker_;
    if (!worker) return;
    {
        std::lock_guard<std::mutex> lock(worker->mutex);
        worker->stop = true;
        worker->request.notify = nullptr;
        ++worker->request.generation;
    }
    SetEvent(worker->wake);
    if (worker->thread) WaitForSingleObject(worker->thread, 100);
    decode_worker_.reset();
}

void WindowMaterial::SetCompositor(Compositor* compositor) {
    compositor_ = compositor;
    ResetGpuResources();
    if (decode_worker_) {
        std::lock_guard<std::mutex> lock(decode_worker_->mutex);
        decode_worker_->request.notify = compositor ? compositor->Hwnd() : nullptr;
    }
}

void WindowMaterial::ResetGpuResources() {
    source_.reset();
    source_path_.clear();
    source_dc_ = nullptr;
    sampled_.reset();
    sampled_path_.clear();
    sampled_dc_ = nullptr;
    sampled_w_ = 0;
    sampled_h_ = 0;
}

void WindowMaterial::Invalidate() {
    ResetGpuResources();
    source_pixels_.clear();
    source_pixels_path_.clear();
    source_width_ = 0;
    source_height_ = 0;
    source_stride_ = 0;
    source_failed_.clear();
    requested_path_.clear();
    if (decode_worker_) {
        std::lock_guard<std::mutex> lock(decode_worker_->mutex);
        requested_generation_ = ++decode_worker_->next_generation;
        decode_worker_->request = {{}, requested_generation_,
            compositor_ ? compositor_->Hwnd() : nullptr};
        decode_worker_->completed.reset();
        SetEvent(decode_worker_->wake);
    }
}

void WindowMaterial::EnsureDecodeWorker() {
    if (decode_worker_) return;
    auto worker = std::make_shared<DecodeWorker>();
    worker->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!worker->wake) return;
    auto* argument = new std::shared_ptr<DecodeWorker>(worker);
    worker->thread = CreateThread(nullptr, 0, DecodeWorkerMain, argument, 0, nullptr);
    if (!worker->thread) {
        delete argument;
        return;
    }
    decode_worker_ = std::move(worker);
}

void WindowMaterial::QueueDecode(const std::wstring& path) {
    EnsureDecodeWorker();
    requested_path_ = path;
    if (!decode_worker_) {
        source_failed_ = path;
        return;
    }
    {
        std::lock_guard<std::mutex> lock(decode_worker_->mutex);
        requested_generation_ = ++decode_worker_->next_generation;
        decode_worker_->request = {path, requested_generation_,
            compositor_ ? compositor_->Hwnd() : nullptr};
        decode_worker_->completed.reset();
    }
    SetEvent(decode_worker_->wake);
}

void WindowMaterial::TakeDecodeResult() {
    if (!decode_worker_) return;
    std::optional<DecodeWorker::Result> completed;
    {
        std::lock_guard<std::mutex> lock(decode_worker_->mutex);
        if (decode_worker_->completed) {
            completed = std::move(decode_worker_->completed);
            decode_worker_->completed.reset();
        }
    }
    if (!completed || completed->generation != requested_generation_ ||
        completed->path != requested_path_) {
        return;
    }

    ResetGpuResources();
    if (FAILED(completed->decoded.result)) {
        source_pixels_.clear();
        source_pixels_path_.clear();
        source_width_ = 0;
        source_height_ = 0;
        source_stride_ = 0;
        source_failed_ = completed->path;
        return;
    }
    source_pixels_ = std::move(completed->decoded.pixels);
    source_pixels_path_ = completed->path;
    source_width_ = completed->decoded.width;
    source_height_ = completed->decoded.height;
    source_stride_ = completed->decoded.stride;
    source_failed_.clear();
}

DWORD WINAPI WindowMaterial::DecodeWorkerMain(void* parameter) {
    std::unique_ptr<std::shared_ptr<DecodeWorker>> argument(
        static_cast<std::shared_ptr<DecodeWorker>*>(parameter));
    std::shared_ptr<DecodeWorker> self = *argument;
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    uint64_t handled_generation = 0;
    for (;;) {
        DecodeWorker::Request request;
        bool stop = false;
        {
            std::lock_guard<std::mutex> lock(self->mutex);
            stop = self->stop;
            request = self->request;
        }
        if (stop) break;
        if (request.generation == handled_generation) {
            WaitForSingleObject(self->wake, INFINITE);
            continue;
        }
        handled_generation = request.generation;
        if (request.path.empty()) continue;

        self->attempts.fetch_add(1, std::memory_order_relaxed);
        const DWORD delay = self->test_delay_ms.load(std::memory_order_relaxed);
        if (delay > 0) Sleep(delay);
        DecodedPixels decoded;
        if (SUCCEEDED(com_result)) decoded = DecodeImageFile(request.path);
        else decoded.result = com_result;

        HWND notify = nullptr;
        {
            std::lock_guard<std::mutex> lock(self->mutex);
            if (!self->stop && self->request.generation == request.generation) {
                self->completed = DecodeWorker::Result{
                    request.path, request.generation, std::move(decoded)};
                notify = self->request.notify;
            }
        }
        if (notify && IsWindow(notify)) InvalidateRect(notify, nullptr, FALSE);
    }
    if (SUCCEEDED(com_result)) CoUninitialize();
    return 0;
}

WindowMaterial::Recipe WindowMaterial::RecipeFor(WindowEffect effect, bool dark) noexcept {
    Recipe recipe;
    switch (effect) {
    case WindowEffect::Acrylic:
        recipe.blur_std = 30.0f;
        recipe.tint_opacity = 0.0f;
        recipe.luminosity_opacity = 0.64f;
        recipe.tint = dark ? HexColor(0x545454) : HexColor(0xD3D3D3);
        break;
    case WindowEffect::MicaAlt:
        recipe.blur_std = 80.0f;
        recipe.tint_opacity = dark ? 0.0f : 0.5f;
        recipe.luminosity_opacity = 1.0f;
        recipe.tint = dark ? HexColor(0x0A0A0A) : HexColor(0xDADADA);
        break;
    case WindowEffect::Mica:
    default:
        recipe.blur_std = 80.0f;
        recipe.tint_opacity = dark ? 0.8f : 0.5f;
        recipe.luminosity_opacity = 1.0f;
        recipe.tint = dark ? HexColor(0x202020) : HexColor(0xF3F3F3);
        break;
    }
    return recipe;
}

ID2D1Bitmap* WindowMaterial::SourceBitmap(const std::wstring& path) {
    ID2D1DeviceContext* dc = compositor_ ? compositor_->Dc() : nullptr;
    TakeDecodeResult();
    if (!dc || path.empty()) {
        ResetGpuResources();
        return nullptr;
    }
    if (source_.get() && source_dc_ == dc && source_path_ == path)
        return source_.get();
    if (source_failed_ == path)
        return nullptr;

    if (source_pixels_path_ == path && !source_pixels_.empty()) {
        D2D1_BITMAP_PROPERTIES1 props{};
        props.pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED};
        props.dpiX = 96.0f;
        props.dpiY = 96.0f;
        ComPtr<ID2D1Bitmap1> uploaded;
        const HRESULT hr = dc->CreateBitmap(
            D2D1::SizeU(source_width_, source_height_), source_pixels_.data(),
            source_stride_, props, &uploaded);
        if (FAILED(hr)) {
            if (compositor_ &&
                (hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED ||
                 hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR)) {
                compositor_->NotifyDeviceLost(hr);
            }
            return nullptr;
        }
        source_ = std::move(uploaded);
        source_path_ = path;
        source_dc_ = dc;
        return source_.get();
    }

    if (requested_path_ != path) {
        ResetGpuResources();
        source_pixels_.clear();
        source_pixels_path_.clear();
        source_width_ = 0;
        source_height_ = 0;
        source_stride_ = 0;
        source_failed_.clear();
        QueueDecode(path);
    }
    return nullptr;
}

#ifdef PULSE_WINDOW_MATERIAL_TESTING
void WindowMaterial::SetDecodeDelayForTesting(DWORD delay_ms) {
    EnsureDecodeWorker();
    if (decode_worker_) {
        decode_worker_->test_delay_ms.store(delay_ms, std::memory_order_relaxed);
    }
}

uint64_t WindowMaterial::DecodeAttemptsForTesting() const {
    return decode_worker_
        ? decode_worker_->attempts.load(std::memory_order_relaxed)
        : 0;
}

bool WindowMaterial::DecodeFailedForTesting(const std::wstring& path) {
    TakeDecodeResult();
    return source_failed_ == path;
}
#endif

bool WindowMaterial::DrawSourceCover(ID2D1DeviceContext* dc, const D2D1_RECT_F& dest,
                                     const std::wstring& path) {
    ID2D1Bitmap* source = SourceBitmap(path);
    if (!dc || !source) return false;
    const D2D1_SIZE_F size = source->GetSize();
    const D2D1_RECT_F cover = CoverRect(size.width, size.height, dest);
    dc->PushAxisAlignedClip(dest, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    dc->DrawBitmap(source, cover, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                   nullptr, nullptr);
    dc->PopAxisAlignedClip();
    return true;
}

ID2D1Bitmap* WindowMaterial::EnsureSampled(ID2D1DeviceContext* dc, const D2D1_RECT_F& dest,
                                           WindowEffect effect, bool dark,
                                           const std::wstring& path) {
    ID2D1Bitmap* source = SourceBitmap(path);
    if (!dc || !source) return nullptr;

    const float dw = dest.right - dest.left;
    const float dh = dest.bottom - dest.top;
    if (dw < 2.0f || dh < 2.0f) return nullptr;
    const float long_edge = (std::max)(dw, dh);
    const float scale = (std::min)(1.0f, kSampleLongEdge / long_edge);
    const int sw = (std::max)(2, static_cast<int>(dw * scale + 0.5f));
    const int sh = (std::max)(2, static_cast<int>(dh * scale + 0.5f));

    if (sampled_.get() && sampled_dc_ == dc && sampled_path_ == path &&
        sampled_effect_ == effect && sampled_dark_ == dark) {
        // Interactive resize can deliver a WM_SIZE for every pixel. Reusing
        // a nearby sample avoids rebuilding the full blur graph on each tick;
        // a larger change still refreshes the cache for final quality.
        const int tolerance_w = (std::max)(16, sw / 8);
        const int tolerance_h = (std::max)(16, sh / 8);
        if (std::abs(sampled_w_ - sw) <= tolerance_w &&
            std::abs(sampled_h_ - sh) <= tolerance_h) {
            return sampled_.get();
        }
    }

    const Recipe recipe = RecipeFor(effect, dark);
    const float blur_std = recipe.blur_std * (static_cast<float>(sw) / dw);
    const D2D1_SIZE_F size = source->GetSize();
    const D2D1_RECT_F cover = CoverRect(size.width, size.height,
                                        D2D1::RectF(-1.0f, -1.0f,
                                                    static_cast<float>(sw) + 1.0f,
                                                    static_cast<float>(sh) + 1.0f));

    ComPtr<ID2D1CommandList> cover_list;
    if (FAILED(dc->CreateCommandList(&cover_list)))
        return nullptr;

    {
        TargetGuard guard(dc);
        dc->SetTarget(cover_list.get());
        dc->SetTransform(D2D1::Matrix3x2F::Identity());
        dc->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f));
        dc->DrawBitmap(source, cover, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                       nullptr, nullptr);
    }
    if (FAILED(cover_list->Close()))
        return nullptr;

    MaterialGraph graph;
    const HRESULT graph_hr = BuildMaterialGraph(dc, cover_list.get(), recipe.tint,
                                                blur_std, recipe.luminosity_opacity,
                                                recipe.tint_opacity, graph);
    if (FAILED(graph_hr) || !graph.color_blend.get()) {
        return nullptr;
    }

    ComPtr<ID2D1Bitmap1> baked;
    if (FAILED(CreateTargetBitmap(dc, static_cast<UINT>(sw), static_cast<UINT>(sh), &baked)))
        return nullptr;

    {
        TargetGuard guard(dc);
        dc->SetTarget(baked.get());
        dc->SetTransform(D2D1::Matrix3x2F::Identity());
        dc->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f));
        dc->DrawImage(graph.color_blend.get(), D2D1::Point2F(0.0f, 0.0f),
                      D2D1_INTERPOLATION_MODE_LINEAR);
    }

    sampled_.reset();
    sampled_.p = baked.get();
    if (sampled_.p) sampled_.p->AddRef();
    sampled_path_ = path;
    sampled_effect_ = effect;
    sampled_dark_ = dark;
    sampled_w_ = sw;
    sampled_h_ = sh;
    sampled_dc_ = dc;
    return sampled_.get();
}

bool WindowMaterial::DrawBackdrop(ID2D1DeviceContext* dc, const D2D1_RECT_F& dest,
                                  WindowEffect effect, bool dark, const std::wstring& path) {
    if (!dc || path.empty() || effect == WindowEffect::None || IsHighContrast()) {
        return false;
    }
    ID2D1Bitmap* sampled = EnsureSampled(dc, dest, effect, dark, path);
    if (!sampled) return false;
    dc->DrawBitmap(sampled, dest, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                   nullptr, nullptr);
    return true;
}

} // namespace pulse::ui

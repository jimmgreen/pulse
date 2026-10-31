#include "../ui/visible_text_renderer.h"
#include <d3d11.h>
#include <dwrite_2.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <vector>
#include <string>
using Microsoft::WRL::ComPtr;
namespace {
int failures = 0;
void Check(bool ok, const char* name) {
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
class Inline final : public IDWriteInlineObject {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id != __uuidof(IUnknown) && id != __uuidof(IDWriteInlineObject)) return E_NOINTERFACE;
        *out = static_cast<IDWriteInlineObject*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto n = --refs_; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE Draw(void*, IDWriteTextRenderer*, FLOAT, FLOAT, BOOL, BOOL, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetMetrics(DWRITE_INLINE_OBJECT_METRICS* out) override {
        if (!out) return E_POINTER; *out = {16, 16, 12, FALSE}; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetOverhangMetrics(DWRITE_OVERHANG_METRICS* out) override {
        if (!out) return E_POINTER; *out = {}; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetBreakConditions(DWRITE_BREAK_CONDITION* before, DWRITE_BREAK_CONDITION* after) override {
        if (!before || !after) return E_POINTER;
        *before = *after = DWRITE_BREAK_CONDITION_NEUTRAL; return S_OK;
    }
private:
    std::atomic<ULONG> refs_{1};
};
class Unknown final : public IUnknown {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id != __uuidof(IUnknown)) return E_NOINTERFACE;
        *out = static_cast<IUnknown*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto n = --refs_; if (!n) delete this; return n; }
private:
    std::atomic<ULONG> refs_{1};
};
struct Canvas {
    ComPtr<ID3D11Device> d3d;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<ID2D1Device> device;
    ComPtr<ID2D1DeviceContext> dc;
    ComPtr<ID2D1Bitmap1> bitmap;
    ComPtr<IDWriteFactory2> factory;
    bool Init() {
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory2),
                reinterpret_cast<IUnknown**>(factory.GetAddressOf()))) ||
            FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr)) || FAILED(d3d.As(&dxgi)) ||
            FAILED(D2D1CreateDevice(dxgi.Get(), nullptr, &device)) ||
            FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc))) return false;
        const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        if (FAILED(dc->CreateBitmap(D2D1::SizeU(600, 300), nullptr, 0, props, &bitmap))) return false;
        dc->SetTarget(bitmap.Get()); dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE); return true;
    }
    void Begin() { dc->BeginDraw(); dc->Clear(D2D1::ColorF(D2D1::ColorF::White)); }
    void End() { Check(SUCCEEDED(dc->EndDraw()), "finish offscreen frame"); }
    std::vector<unsigned char> Pixels() {
        ComPtr<ID2D1Bitmap1> copy;
        const auto size = bitmap->GetPixelSize();
        const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, bitmap->GetPixelFormat());
        if (FAILED(dc->CreateBitmap(size, nullptr, 0, props, &copy)) ||
            FAILED(copy->CopyFromBitmap(nullptr, bitmap.Get(), nullptr))) return {};
        D2D1_MAPPED_RECT map{};
        if (FAILED(copy->Map(D2D1_MAP_OPTIONS_READ, &map))) return {};
        std::vector<unsigned char> bytes(size.width * size.height * 4);
        for (UINT32 y = 0; y < size.height; ++y)
            std::copy_n(map.bits + y * map.pitch, size.width * 4, bytes.data() + y * size.width * 4);
        copy->Unmap(); return bytes;
    }
};
}
int wmain() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    {
        Canvas c; if (!c.Init()) return 2;
        ComPtr<IDWriteTextFormat> format;
        if (FAILED(c.factory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 18, L"en-us", &format))) return 2;
        ComPtr<ID2D1SolidColorBrush> brush;
        if (FAILED(c.dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &brush))) return 2;
        const auto clip = D2D1::RectF(0, 0, 600, 140);
        const auto origin = D2D1::Point2F(20.0f, 17.5f);
        std::wstring body;
        for (int i = 0; i < 35; ++i) body += L"row \x6587\x4ef6 \xD83D\xDE00\n";
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(c.factory->CreateTextLayout(body.data(), static_cast<UINT32>(body.size()), format.Get(), 560, 2000, &layout))) return 2;
        c.Begin(); c.dc->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        c.dc->DrawTextLayout(origin, layout.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP | D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
        c.dc->PopAxisAlignedClip(); c.End(); const auto native = c.Pixels();
        c.Begin(); c.dc->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        pulse::ui::VisibleTextDrawStats stats;
        const bool drawn = pulse::ui::DrawVisibleTextLayout(c.dc.Get(), layout.Get(), brush.Get(), origin, clip, &stats);
        c.dc->PopAxisAlignedClip(); c.End();
        Check(drawn && stats.culled_runs > 0 && stats.drawn_runs > 0, "visible runs draw and offscreen runs are culled");
        Check(!native.empty() && native == c.Pixels(), "fractional origin and color glyphs match native RGBA");
        c.Begin(); c.End(); const auto empty = c.Pixels();
        const std::wstring complex = L"prefix \xD83D\xDC68\x200D\xD83D\xDC69\x200D\xD83D\xDC67\x200D\xD83D\xDC66";
        ComPtr<IDWriteTextLayout> complex_layout;
        if (FAILED(c.factory->CreateTextLayout(complex.data(), static_cast<UINT32>(complex.size()), format.Get(), 560, 2000, &complex_layout))) return 2;
        c.Begin(); const bool complex_drawn = pulse::ui::DrawVisibleTextLayout(c.dc.Get(), complex_layout.Get(), brush.Get(), origin, clip); c.End();
        Check(!complex_drawn && c.Pixels() == empty, "complex paint run preserves atomic native/cache fallback");
        ComPtr<IDWriteTextLayout> fallback;
        if (FAILED(c.factory->CreateTextLayout(L"prefix x", 8, format.Get(), 560, 2000, &fallback))) return 2;
        ComPtr<Inline> object; object.Attach(new Inline);
        Check(SUCCEEDED(fallback->SetInlineObject(object.Get(), {7, 1})), "prepare inline after a drawable prefix");
        c.Begin(); const bool inline_drawn = pulse::ui::DrawVisibleTextLayout(c.dc.Get(), fallback.Get(), brush.Get(), origin, clip); c.End();
        Check(!inline_drawn && c.Pixels() == empty, "inline callback failure publishes no partial prefix");
        Check(SUCCEEDED(fallback->SetInlineObject(nullptr, {7, 1})), "remove inline before effect test");
        ComPtr<Unknown> unknown; unknown.Attach(new Unknown);
        Check(SUCCEEDED(fallback->SetDrawingEffect(unknown.Get(), {7, 1})), "prepare unknown effect after a drawable prefix");
        c.Begin(); const bool effect_drawn = pulse::ui::DrawVisibleTextLayout(c.dc.Get(), fallback.Get(), brush.Get(), origin, clip); c.End();
        Check(!effect_drawn && c.Pixels() == empty, "unknown effect publishes no partial prefix");
        c.Begin(); c.dc->SetTransform(D2D1::Matrix3x2F::Translation(0.5f, 0));
        const bool transformed = pulse::ui::DrawVisibleTextLayout(c.dc.Get(), layout.Get(), brush.Get(), origin, clip);
        c.dc->SetTransform(D2D1::Matrix3x2F::Identity()); c.End();
        Check(!transformed && c.Pixels() == empty, "animation transform leaves native fallback untouched");
    }
    CoUninitialize(); return failures ? 1 : 0;
}

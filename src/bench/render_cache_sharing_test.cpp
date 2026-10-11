// render_cache_sharing_test.cpp - Multi-window M3d: render caches shared by
// several windows. A cache binds to the Direct2D device instead of one
// window's context, so another window on the same device keeps its bitmaps
// and a closed window's context is never used; background results repaint
// every registered window.
#include "../ui/d2d_device_binding.h"
#include "../ui/repaint_targets.h"

#include <cstdio>
#include <thread>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d2d1.lib")

using pulse::ui::ComPtr;
using pulse::ui::D2DDeviceBinding;
using pulse::ui::RepaintTargets;

namespace {
int g_failures = 0;
void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

struct Device {
    ComPtr<ID3D11Device> d3d;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<ID2D1Factory1> factory;
    ComPtr<ID2D1Device> d2d;
    bool Create() {
        return SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                           nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr)) &&
               SUCCEEDED(d3d->QueryInterface(IID_PPV_ARGS(&dxgi))) &&
               SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&factory))) &&
               SUCCEEDED(factory->CreateDevice(dxgi.get(), &d2d));
    }
    // A window-like context: its own target bitmap, as a Compositor has.
    bool Context(ComPtr<ID2D1DeviceContext>& dc, ComPtr<ID2D1Bitmap1>& target) const {
        if (FAILED(d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc))) return false;
        const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(dc->CreateBitmap(D2D1::SizeU(16, 16), nullptr, 0, &props, &target))) return false;
        dc->SetTarget(target.get());
        return true;
    }
};

bool MakeBitmap(ID2D1DeviceContext* dc, ComPtr<ID2D1Bitmap>& bitmap) {
    if (!dc) return false;
    const uint32_t pixels[4] = {0xFF2060A0u, 0xFF2060A0u, 0xFF2060A0u, 0xFF2060A0u};
    const auto props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    return SUCCEEDED(dc->CreateBitmap(D2D1::SizeU(2, 2), pixels, 8, &props, &bitmap));
}

HRESULT DrawInto(ID2D1DeviceContext* dc, ID2D1Bitmap* bitmap) {
    dc->BeginDraw();
    dc->DrawBitmap(bitmap, D2D1::RectF(0, 0, 8, 8));
    return dc->EndDraw();
}

bool SameDevice(ID2D1DeviceContext* dc, ID2D1Device* device) {
    if (!dc) return false;
    ComPtr<ID2D1Device> owner;
    dc->GetDevice(&owner);
    return owner.get() == device;
}

HWND TestWindow() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PulseRenderCacheSharingTest";
    RegisterClassW(&wc);
    // Visible but off-screen: hidden windows never accumulate an update region.
    return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_POPUP | WS_VISIBLE,
                           -32000, -32000, 32, 32, nullptr, nullptr, wc.hInstance, nullptr);
}

bool HasUpdate(HWND hwnd) { return GetUpdateRect(hwnd, nullptr, FALSE) != FALSE; }
}  // namespace

int main() {
    // --- RepaintTargets
    RepaintTargets targets;
    const HWND a = TestWindow(), b = TestWindow(), c = TestWindow();
    Check(a && b && c, "test windows created");
    ValidateRect(a, nullptr); ValidateRect(b, nullptr); ValidateRect(c, nullptr);
    targets.Add(a);
    targets.Add(b);
    targets.Add(a);
    targets.Add(nullptr);
    Check(targets.Count() == 2, "windows register once; null is ignored");
    std::thread([&] { targets.InvalidateAll(); }).join();
    Check(HasUpdate(a) && HasUpdate(b) && !HasUpdate(c), "a worker result repaints every registered window, only those");
    ValidateRect(a, nullptr); ValidateRect(b, nullptr);
    targets.Remove(a);
    std::thread([&] { targets.InvalidateAll(); }).join();
    Check(!HasUpdate(a) && HasUpdate(b), "a window that left no longer repaints");
    targets.Set(c);
    Check(targets.Count() == 1, "Set replaces every window (caches owned by one window)");
    targets.Set(nullptr);
    Check(targets.Count() == 0, "Set(nullptr) clears");
    for (const HWND hwnd : {a, b, c}) if (hwnd) DestroyWindow(hwnd);

    // --- D2DDeviceBinding
    Device one, two;
    ComPtr<ID2D1DeviceContext> window_a, window_b, other;
    ComPtr<ID2D1Bitmap1> target_a, target_b, target_other;
    const bool graphics = one.Create() && two.Create() && one.Context(window_a, target_a) &&
                          one.Context(window_b, target_b) && two.Context(other, target_other);
    Check(graphics, "Direct2D software devices and window contexts");
    if (graphics) {
        D2DDeviceBinding binding;
        Check(binding.Bind(window_a.get()) && binding.Dc() && binding.Dc() != window_a.get() &&
                  SameDevice(binding.Dc(), one.d2d.get()),
              "binding creates a private context on the window's device");
        ID2D1DeviceContext* const private_dc = binding.Dc();
        Check(!binding.Bind(window_b.get()) && binding.Dc() == private_dc,
              "another window on the same device keeps the binding (no cache flush)");
        ComPtr<ID2D1Bitmap> shared;
        Check(MakeBitmap(binding.Dc(), shared) && SUCCEEDED(DrawInto(window_a.get(), shared.get())) &&
                  SUCCEEDED(DrawInto(window_b.get(), shared.get())),
              "a cached bitmap draws in every window of the device");
        window_a.reset();
        target_a.reset();
        ComPtr<ID2D1Bitmap> after_close;
        Check(MakeBitmap(binding.Dc(), after_close) && SUCCEEDED(DrawInto(window_b.get(), after_close.get())),
              "bitmaps are still created after the first window's context is gone");
        Check(DrawInto(other.get(), shared.get()) == D2DERR_WRONG_FACTORY,
              "control: a bitmap from another factory's device cannot be drawn");
        // Same factory, different device: the device itself is the boundary.
        Device sibling;
        ComPtr<ID2D1DeviceContext> sibling_dc;
        ComPtr<ID2D1Bitmap1> sibling_target;
        const bool sibling_ok =
            SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                        nullptr, 0, D3D11_SDK_VERSION, &sibling.d3d, nullptr, nullptr)) &&
            SUCCEEDED(sibling.d3d->QueryInterface(IID_PPV_ARGS(&sibling.dxgi))) &&
            SUCCEEDED(one.factory->CreateDevice(sibling.dxgi.get(), &sibling.d2d)) &&
            sibling.Context(sibling_dc, sibling_target);
        Check(sibling_ok && DrawInto(sibling_dc.get(), shared.get()) == D2DERR_WRONG_RESOURCE_DOMAIN,
              "control: a bitmap from another device of the same factory cannot be drawn");
        Check(binding.Bind(other.get()) && SameDevice(binding.Dc(), two.d2d.get()),
              "a different device (device loss) rebinds and reports the change");
        Check(binding.Bind(nullptr) && !binding.Dc() && !binding.Device(), "binding to nothing releases the device");
        Check(!binding.Bind(nullptr), "unbinding twice reports no change");
    }

    std::printf("render_cache_sharing_test: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}

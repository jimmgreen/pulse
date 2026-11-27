// Persistent GPU icon cache contracts. All HWND work stays on a private desktop.
#include "../ui/fluent_menu.h"
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace pulse::app { std::wstring GetPulseDataDir() { return {}; } }
namespace pulse::ui {
struct FluentMenuTestPeer {
    static common::MenuIconRef Pixels(unsigned color, unsigned edge = 16) {
        auto pixels = std::make_shared<common::MenuIconPixels>();
        pixels->width = pixels->height = edge;
        pixels->bgra.resize(static_cast<size_t>(edge) * edge * 4);
        for (size_t i = 0; i < pixels->bgra.size(); i += 4) {
            pixels->bgra[i] = static_cast<unsigned char>(color);
            pixels->bgra[i + 1] = static_cast<unsigned char>(color >> 8);
            pixels->bgra[i + 2] = static_cast<unsigned char>(color >> 16);
            pixels->bgra[i + 3] = 255;
        }
        return pixels;
    }
    static ComPtr<ID2D1Bitmap> Hold(ID2D1Bitmap* bitmap) {
        ComPtr<ID2D1Bitmap> held;
        if (bitmap) { bitmap->AddRef(); held.p = bitmap; }
        return held;
    }
    static bool Matches(ID2D1DeviceContext* dc, ID2D1Bitmap* bitmap, const common::MenuIconRef& pixels) {
        if (!bitmap) return false;
        const auto size = bitmap->GetPixelSize();
        if (size.width != pixels->width || size.height != pixels->height) return false;
        const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            bitmap->GetPixelFormat(), 96, 96);
        ComPtr<ID2D1Bitmap1> cpu;
        if (FAILED(dc->CreateBitmap(size, nullptr, 0, props, &cpu)) ||
            FAILED(cpu->CopyFromBitmap(nullptr, bitmap, nullptr))) return false;
        D2D1_MAPPED_RECT mapped{};
        if (FAILED(cpu->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
        bool same = true;
        for (unsigned y = 0; y < size.height; ++y) {
            same = same && std::memcmp(mapped.bits + static_cast<size_t>(y) * mapped.pitch,
                pixels->bgra.data() + static_cast<size_t>(y) * size.width * 4, size.width * 4) == 0;
        }
        cpu->Unmap();
        return same;
    }
    static inline FluentMenu* active = nullptr;
    static inline int popup_stage = 0;
    static inline bool submenu_shown = false;
    static void CALLBACK SelectFixture(HWND, UINT, UINT_PTR timer, DWORD) {
        if (!active || !active->IsOpen()) return;
        if (popup_stage == 0) {
            active->OpenSubmenu(0);
            submenu_shown = active->first_sub_.parent_row == 0 && IsWindowVisible(active->first_sub_.hwnd);
            popup_stage = 1;
            return;
        }
        KillTimer(nullptr, timer);
        const auto& model = active->first_sub_.model;
        const POINT child{active->ShadowMargin() + 32,
            active->ShadowMargin() + static_cast<LONG>(model.RowTopPx(0) + model.RowHeightPx(0) / 2)};
        active->OnSubMouse(child, true); // Internal hit-test/dispatch; no global input or cursor operation.
        popup_stage = 2;
        if (active->IsOpen()) active->Dismiss(); // A failed hit test must still close the fixture.
    }
    static bool Popup(FluentMenu& menu, const common::MenuIconRef& pixels) {
        FluentMenuItem header;
        header.text = L"Fixture";
        header.icon = pixels;
        FluentMenuItem child;
        child.command = 1001;
        child.text = L"Select fixture";
        child.icon = pixels;
        header.children.push_back(std::move(child));
        active = &menu;
        popup_stage = 0;
        submenu_shown = false;
        const UINT_PTR timer = SetTimer(nullptr, 0, 30, SelectFixture);
        if (!timer) { active = nullptr; return false; }
        const int command = menu.TrackPopup({240, 160}, {header});
        KillTimer(nullptr, timer);
        active = nullptr;
        return command == 1001 && popup_stage == 2 && submenu_shown;
    }
    static int Run(FluentMenu& menu, HWND owner, Compositor& compositor) {
        int failures = 0;
        const auto check = [&](bool ok, const char* label) {
            std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
            failures += !ok;
        };
        auto* dc = compositor.Dc();
        const auto initial_count = menu.icon_bitmaps_.size();
        check(!menu.IconBitmapFor({}, dc) && !menu.IconBitmapFor(Pixels(0, 65), dc) &&
            menu.icon_bitmaps_.size() == initial_count, "invalid pixels never enter cache");
        auto pixels = Pixels(0x13579B);
        auto first = Hold(menu.IconBitmapFor(pixels, dc));
        const auto first_count = menu.icon_bitmaps_.size();
        check(first.get() && first.get() == menu.IconBitmapFor(pixels, dc) &&
            menu.icon_bitmaps_.size() == first_count, "same ref reuses existing GPU bitmap");
        std::weak_ptr<const common::MenuIconPixels> weak = pixels;
        pixels.reset();
        check(!weak.expired(), "cache owns pixel identity and prevents dangling-address matches");
        auto changed = Pixels(0x97531B);
        auto* bitmap = menu.IconBitmapFor(changed, dc);
        check(bitmap != first.get() && Matches(dc, bitmap, changed), "new color ref uploads exact new pixels");
        auto resized = Pixels(0x97531B, 32);
        check(Matches(dc, menu.IconBitmapFor(resized, dc), resized), "dimension change keeps exact pixels and size");
        ComPtr<ID2D1Device> device;
        dc->GetDevice(&device);
        ComPtr<ID2D1DeviceContext> second;
        if (FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &second))) return failures + 1;
        check(Matches(second.get(), menu.IconBitmapFor(changed, second.get()), changed) && weak.expired(),
            "context replacement drops prior CPU identities");
        ComPtr<ID3D11Device> d3d;
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<ID2D1Factory1> factory;
        ComPtr<ID2D1Device> other_device;
        ComPtr<ID2D1DeviceContext> other_dc;
        const bool other = SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr)) &&
            SUCCEEDED(d3d->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi))) &&
            SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                nullptr, reinterpret_cast<void**>(&factory))) &&
            SUCCEEDED(factory->CreateDevice(dxgi.get(), &other_device)) &&
            SUCCEEDED(other_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &other_dc));
        check(other && Matches(other_dc.get(), menu.IconBitmapFor(changed, other_dc.get()), changed),
            "different D3D/D2D device receives correct resource-domain bitmap");
        auto before_theme = Hold(menu.IconBitmapFor(changed, dc));
        menu.SetTheme(!menu.dark_, menu.accent_);
        auto after_theme = Hold(menu.IconBitmapFor(changed, dc));
        check(after_theme.get() && after_theme.get() != before_theme.get() && Matches(dc, after_theme.get(), changed),
            "theme change invalidates retained uploads without changing pixels");
        menu.SetTheme(menu.dark_, menu.accent_);
        check(menu.IconBitmapFor(changed, dc) == after_theme.get(), "identical theme preserves warm upload");
        const float original_scale = menu.scale_;
        menu.Create(owner, &compositor, original_scale * 1.5f);
        auto* dpi_bitmap = menu.IconBitmapFor(changed, dc);
        check(dpi_bitmap && dpi_bitmap != after_theme.get() && Matches(dc, dpi_bitmap, changed),
            "DPI change invalidates retained uploads without changing source pixels");
        menu.Create(owner, &compositor, original_scale);
        menu.SetTheme(!menu.dark_, menu.accent_);
        std::vector<std::weak_ptr<const common::MenuIconPixels>> weak_rows;
        common::MenuIconRef hot;
        for (unsigned i = 0; i < 256; ++i) {
            auto icon = Pixels(i);
            if (i == 0) hot = icon;
            weak_rows.push_back(icon);
            menu.IconBitmapFor(icon, dc);
        }
        menu.IconBitmapFor(hot, dc);
        menu.IconBitmapFor(Pixels(512), dc);
        check(menu.icon_bitmaps_.size() == 256 && !weak_rows[0].expired() && weak_rows[1].expired(),
            "bounded cache evicts least recently used pixel owner");
        for (unsigned i = 0; i < 80; ++i) menu.IconBitmapFor(Pixels(1024 + i, 64), dc);
        check(menu.icon_bitmaps_.size() == 256, "64-pixel icon churn remains bounded");
        menu.SetTheme(!menu.dark_, menu.accent_);
        auto retry = Pixels(0xFFCC88);
        // Inject the memoized failure state, not a fake GPU success or a weakened assertion.
        FluentMenu::IconBitmap failed;
        failed.pixels = retry;
        dc->AddRef();
        failed.context.p = dc;
        failed.last_used = (std::numeric_limits<uint64_t>::max)();
        menu.icon_bitmaps_.push_back(std::move(failed));
        check(!menu.IconBitmapFor(retry, dc) && !menu.IconBitmapFor(retry, dc) && menu.icon_bitmaps_.size() == 1,
            "memoized failure is not retried within the same popup");
        check(Popup(menu, retry) && Matches(dc, menu.IconBitmapFor(retry, dc), retry),
            "next actual popup retries memoized failure with correct pixels");
        auto warm = Hold(menu.IconBitmapFor(retry, dc));
        check(Popup(menu, retry) && menu.IconBitmapFor(retry, dc) == warm.get(),
            "second actual popup and submenu reuse retained bitmap and dispatch correct command");
        std::printf("RESULT failures=%d\n", failures);
        return failures;
    }
};
} // namespace pulse::ui

int main() {
    const HDESK previous = GetThreadDesktop(GetCurrentThreadId());
    const std::wstring name = L"PulseMenuIconCacheTest-" + std::to_wstring(GetCurrentProcessId());
    const HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desktop || !SetThreadDesktop(desktop)) return 2;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PulseMenuIconCacheTest";
    RegisterClassW(&wc);
    HWND owner = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_NOACTIVATE, wc.lpszClassName, L"",
        WS_POPUP, 0, 0, 800, 600, nullptr, nullptr, wc.hInstance, nullptr);
    int failures = 1;
    {
        pulse::ui::Compositor compositor;
        pulse::ui::FluentMenu menu;
        if (owner && compositor.Init(owner)) {
            compositor.RecreateTextFormats(1);
            if (menu.Create(owner, &compositor, 1)) {
                menu.SetActivateOnOpen(false);
                menu.SetTheme(false, D2D1::ColorF(0.2f, 0.4f, 0.8f));
                failures = pulse::ui::FluentMenuTestPeer::Run(menu, owner, compositor);
            }
        }
    }
    if (owner) DestroyWindow(owner);
    if (SUCCEEDED(com)) CoUninitialize();
    SetThreadDesktop(previous);
    CloseDesktop(desktop);
    return failures ? 1 : 0;
}

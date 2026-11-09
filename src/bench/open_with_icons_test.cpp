#include "../ui/open_with_icons.h"
#include <cstdio>

namespace pulse::ui {
struct OpenWithIconCacheTest {
    static void HoldWorker(OpenWithIconCache& cache) { cache.running_ = true; }
    static size_t Queued(OpenWithIconCache& cache) { return cache.queue_.size(); }
    static void Complete(OpenWithIconCache& cache, const wchar_t* extension, bool found) {
        const std::wstring key = std::wstring(extension) + L"|24";
        OpenWithIconCache::Pixels pixels;
        if (found) {
            pixels.width = pixels.height = 1;
            pixels.data = {0, 0, 255, 255};
        }
        cache.ready_[key] = std::move(pixels);
        cache.pending_.erase(key);
    }
};
}

int main() {
    using namespace pulse::ui;
    int failed = 0;
    const auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) ++failed;
    };
    ComPtr<ID3D11Device> d3d;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
    ComPtr<IDXGIDevice> dxgi;
    if (SUCCEEDED(hr)) hr = d3d->QueryInterface(IID_PPV_ARGS(&dxgi));
    ComPtr<ID2D1Device> device;
    if (SUCCEEDED(hr)) hr = D2D1CreateDevice(dxgi.get(), nullptr, &device);
    ComPtr<ID2D1DeviceContext> dc;
    if (SUCCEEDED(hr)) hr = device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc);
    check(SUCCEEDED(hr), "create real software D2D device");
    if (FAILED(hr)) return 1;
    OpenWithIconCache cache;
    cache.SetDeviceContext(dc.get());
    OpenWithIconCacheTest::HoldWorker(cache);
    for (const auto* name : {L"photo.JPG", L"photo.png", L"photo.HEIC", L"image.svg",
            L"image.GIF", L"photo.CR3", L"image.apng"})
        check(!cache.BadgeFor(name, 24).bitmap && OpenWithIconCacheTest::Queued(cache) == 0,
              "image does not display or request an application badge");
    check(!cache.BadgeFor(L"document.pdf", 24).bitmap && OpenWithIconCacheTest::Queued(cache) == 1,
          "document requests application icon asynchronously");
    for (int i = 0; i < 1000; ++i) cache.BadgeFor(L"another.pdf", 24);
    check(OpenWithIconCacheTest::Queued(cache) == 1,
          "1000 same-type draws share one pending lookup");
    OpenWithIconCacheTest::Complete(cache, L".pdf", true);
    const auto first = cache.BadgeFor(L"document.pdf", 24);
    check(first.bitmap != nullptr, "completed icon appears");
    const auto queued = OpenWithIconCacheTest::Queued(cache);
    for (int i = 0; i < 1000; ++i) cache.BadgeFor(L"another.pdf", 24);
    check(OpenWithIconCacheTest::Queued(cache) == queued,
          "cached draws do not schedule more association work");
    for (int i = 0; i < 50; ++i) {
        cache.InvalidateAssociations();
        if (cache.BadgeFor(L"document.pdf", 24).bitmap != first.bitmap) ++failed;
    }
    check(cache.BadgeFor(L"document.pdf", 24).bitmap == first.bitmap,
          "repeated association notifications retain displayed bitmap while refreshing");
    OpenWithIconCacheTest::Complete(cache, L".pdf", true);
    check(cache.BadgeFor(L"document.pdf", 24).bitmap != nullptr,
          "replacement icon swaps without a blank frame");
    cache.InvalidateAssociations();
    OpenWithIconCacheTest::Complete(cache, L".pdf", false);
    check(!cache.BadgeFor(L"document.pdf", 24).bitmap,
          "removed association clears old icon after resolution");
    cache.BadgeFor(L"movie.mp4", 24);
    OpenWithIconCacheTest::Complete(cache, L".mp4", true);
    check(cache.BadgeFor(L"movie.mp4", 24).bitmap != nullptr, "video retains application badge");
    const auto before_family = OpenWithIconCacheTest::Queued(cache);
    cache.BadgeFor(L"document.psd", 24);
    cache.BadgeFor(L"document.psb", 24);
    check(OpenWithIconCacheTest::Queued(cache) == before_family + 1,
          "PSD/PSB share one pending Photoshop lookup");
    OpenWithIconCacheTest::Complete(cache, L".psd", true);
    const auto shared = cache.BadgeFor(L"document.psd", 24);
    check(shared.bitmap && cache.BadgeFor(L"document.psb", 24).bitmap == shared.bitmap,
          "PSD/PSB share the identical editor bitmap");
    for (const auto* extension : {L".psd", L".psb"}) {
        const std::wstring name = std::wstring(L"document") + extension;
        cache.InvalidateAssociations();
        const auto queued_pending = OpenWithIconCacheTest::Queued(cache);
        const auto pending = cache.BadgeFor(name, 24);
        check(pending.bitmap && !pending.format_label && OpenWithIconCacheTest::Queued(cache) == queued_pending + 1,
              "PSD/PSB refreshes the shared editor without premature fallback");
        OpenWithIconCacheTest::Complete(cache, L".psd", true);
        const auto found = cache.BadgeFor(name, 24);
        check(found.bitmap && !found.format_label, "PSD/PSB displays resolved Photoshop icon");
        cache.InvalidateAssociations();
        check(cache.BadgeFor(name, 24).bitmap == found.bitmap, "PSD/PSB retains icon during refresh");
        OpenWithIconCacheTest::Complete(cache, L".psd", false);
        const auto missing = cache.BadgeFor(name, 24);
        check(!missing.bitmap && missing.format_label &&
              _wcsicmp(missing.format_label, extension + 1) == 0, "PSD/PSB missing app uses correct format label");
        const auto queued_before = OpenWithIconCacheTest::Queued(cache);
        check(cache.BadgeFor(name, 24).format_label && OpenWithIconCacheTest::Queued(cache) == queued_before,
              "PSD/PSB missing result is cached");
        cache.InvalidateAssociations();
        cache.BadgeFor(name, 24);
        OpenWithIconCacheTest::Complete(cache, L".psd", true);
        const auto restored = cache.BadgeFor(name, 24);
        check(restored.bitmap && !restored.format_label, "PSD/PSB new association replaces fallback");
    }
    return failed ? 1 : 0;
}

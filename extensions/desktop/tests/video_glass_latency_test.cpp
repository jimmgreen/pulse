#include "host/render/live_wallpaper_source.h"
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

using Microsoft::WRL::ComPtr;
using pulse::desktop::render::LiveWallpaperSource;

namespace {
int failures = 0;
void Check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}

struct Device {
    ComPtr<ID3D11Device> d3d;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID2D1Factory1> factory;
    ComPtr<ID2D1Device> d2d;
    ComPtr<ID2D1DeviceContext> dc;
    bool Init() {
        D3D_FEATURE_LEVEL level{};
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                &d3d, &level, &context))) return false;
        ComPtr<IDXGIDevice> dxgi;
        if (FAILED(d3d.As(&dxgi))) return false;
        D2D1_FACTORY_OPTIONS options{};
        return SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                   __uuidof(ID2D1Factory1), &options,
                   reinterpret_cast<void**>(factory.GetAddressOf()))) &&
               SUCCEEDED(factory->CreateDevice(dxgi.Get(), &d2d)) &&
               SUCCEEDED(d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc));
    }
};

bool ReadPixel(Device& device, ID2D1Bitmap1* source, uint32_t expected) {
    if (!source) return false;
    ComPtr<ID2D1Bitmap1> read;
    const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        source->GetPixelFormat());
    if (FAILED(device.dc->CreateBitmap(source->GetPixelSize(), nullptr, 0, &props, &read)) ||
        FAILED(read->CopyFromBitmap(nullptr, source, nullptr))) return false;
    D2D1_MAPPED_RECT map{};
    if (FAILED(read->Map(D2D1_MAP_OPTIONS_READ, &map))) return false;
    const bool ok = *reinterpret_cast<const uint32_t*>(map.bits) == expected;
    read->Unmap();
    return ok;
}

void TestContention() {
    Device producer, consumer;
    if (!producer.Init() || !consumer.Init()) {
        Check(false, "two hardware D3D11/D2D devices initialized (no software substitute)");
        return;
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = 64; td.Height = 36; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<IDXGIKeyedMutex> mutex;
    ComPtr<IDXGIResource1> resource;
    HANDLE handle = nullptr;
    if (FAILED(producer.d3d->CreateTexture2D(&td, nullptr, &texture)) ||
        FAILED(texture.As(&mutex)) || FAILED(texture.As(&resource)) ||
        FAILED(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                            nullptr, &handle))) {
        Check(false, "shared NT texture created");
        return;
    }
    const uint32_t first = 0xff2478b2u, second = 0xffb23751u;
    std::vector<uint32_t> pixels(64 * 36, first);
    Check(mutex->AcquireSync(0, 1000) == S_OK, "producer acquires initial texture");
    producer.context->UpdateSubresource(texture.Get(), 0, nullptr, pixels.data(), 64 * 4, 0);
    mutex->ReleaseSync(0);
    LiveWallpaperSource source;
    const auto import = [&]() {
        return source.ImportSharedVideoTexture(consumer.d3d.Get(), consumer.dc.Get(), handle,
                                              64, 36, 256, 144, true);
    };
    Check(import() && source.HasFrame() && source.FrostBitmap(), "initial frame and frost imported");
    Check(ReadPixel(consumer, source.SharpBitmap(), first), "initial GPU pixel matches producer");
    const auto count = source.Stats().frames_imported;
    const bool locked = mutex->AcquireSync(0, 1000) == S_OK;
    Check(locked, "producer holds texture while UI imports");
    std::vector<double> times;
    bool all_busy = true;
    if (locked) {
        for (int i = 0; i < 24; ++i) {
            const auto start = std::chrono::steady_clock::now();
            all_busy = !import() && all_busy;
            times.push_back(std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count());
        }
        std::fill(pixels.begin(), pixels.end(), second);
        producer.context->UpdateSubresource(texture.Get(), 0, nullptr, pixels.data(), 64 * 4, 0);
        mutex->ReleaseSync(0);
    }
    std::sort(times.begin(), times.end());
    const double p95 = times.empty() ? 1e9 : times[times.size() * 95 / 100];
    std::printf("CONTENDED_IMPORT samples=%zu p95_ms=%.3f max_ms=%.3f\n",
                times.size(), p95, times.empty() ? 1e9 : times.back());
    Check(all_busy && source.Stats().frames_imported == count, "busy frames do not mutate imported generation");
    Check(ReadPixel(consumer, source.SharpBitmap(), first), "busy import retains last complete GPU frame");
    Check(p95 < 6.0, "UI import contention p95 below 6 ms (no timed GPU lock wait)");
    Check(import() && ReadPixel(consumer, source.SharpBitmap(), second), "next available frame recovers with correct pixels");
    source.ReleaseDeviceResources();
    Check(!source.HasFrame(), "device release discards imported frame");
    Check(import() && ReadPixel(consumer, source.SharpBitmap(), second), "shared frame reopens after device resource release");
    source.ReleaseDeviceResources();
    CloseHandle(handle);
}
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    TestContention();
    CoUninitialize();
    std::printf("VIDEO_GLASS_LATENCY failed=%d\n", failures);
    return failures ? 1 : 0;
}

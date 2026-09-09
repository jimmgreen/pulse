#include "../ui/shell_icons.h"
#include <shellapi.h>
#include <cstdio>
#include <chrono>

namespace pulse::ui {
struct ShellIconCacheTestAccess {
    static bool Wait(ShellIconCache& cache) {
        const auto end = GetTickCount64() + 10000;
        while (GetTickCount64() < end) {
            {
                std::lock_guard lock(cache.mutex_);
                if (cache.queued_.empty()) return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }
    static bool Run() {
        ShellIconCache cache;
        bool ok = true;
        auto check = [&](bool result, const char* name) {
            std::printf("[%s] %s\n", result ? "PASS" : "FAIL", name);
            ok &= result;
        };
        cache.GenericIndex(L"", true, FILE_ATTRIBUTE_DIRECTORY);
        cache.GenericIndex(L"test.txt", false, FILE_ATTRIBUTE_NORMAL);
        check(Wait(cache), "native type queries complete");
        {
            std::lock_guard lock(cache.mutex_);
            check(cache.generic_index_.contains(L"<dir>") && cache.generic_index_.contains(L".txt"),
                  "Windows folder and associated file icons resolve");
        }
        wchar_t windows[MAX_PATH]{};
        GetWindowsDirectoryW(windows, MAX_PATH);
        check(cache.NeedsExactIcon(L"Windows", true, windows), "folders use actual Shell paths");
        // Seed a full cache to exercise eviction without thousands of Shell calls.
        {
            std::lock_guard lock(cache.mutex_);
            for (int i = 0; i < 4096; ++i) {
                const auto key = L"seed-" + std::to_wstring(i);
                cache.exact_index_[key] = 0;
                cache.last_used_[key] = ++cache.access_clock_;
            }
        }
        cache.RequestExact(windows);
        check(Wait(cache), "exact folder query completes");
        SHFILEINFOW info{};
        const bool resolved = SHGetFileInfoW(windows, 0, &info, sizeof(info),
                                            SHGFI_SYSICONINDEX | SHGFI_SMALLICON) != 0;
        {
            std::lock_guard lock(cache.mutex_);
            check(resolved && cache.exact_index_.contains(windows) &&
                  cache.exact_index_.at(windows) == info.iIcon, "index matches native Shell query");
            check(cache.exact_index_.size() == 4096 && !cache.exact_index_.contains(L"seed-0") &&
                  cache.exact_index_.contains(L"seed-1"), "evicts only oldest entry");
        }
        const std::wstring missing = std::wstring(windows) + L"\\pulse-nonexistent-icon-test\\missing.exe";
        cache.RequestExact(missing);
        check(Wait(cache), "missing path query completes");
        {
            std::lock_guard lock(cache.mutex_);
            check(!cache.exact_index_.contains(missing) && cache.retry_after_.contains(missing),
                  "failure is retryable rather than permanently cached");
            cache.retry_after_[windows] = 0;
            cache.exact_index_.erase(windows);
            cache.last_used_.erase(windows);
        }
        cache.RequestExact(windows);
        check(Wait(cache), "expired failure retries");
        {
            std::lock_guard lock(cache.mutex_);
            check(cache.exact_index_.contains(windows) && !cache.retry_after_.contains(windows),
                  "successful retry clears failure state");
        }
        cache.Reset();
        cache.GenericIndex(L"", true, FILE_ATTRIBUTE_DIRECTORY);
        check(Wait(cache), "worker restarts after reset");
        ComPtr<ID3D11Device> d3d;
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<ID2D1Factory1> factory;
        ComPtr<ID2D1Device> device;
        ComPtr<ID2D1DeviceContext> context;
        const bool graphics = SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP,
            nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &d3d, nullptr, nullptr)) &&
            SUCCEEDED(d3d->QueryInterface(IID_PPV_ARGS(&dxgi))) &&
            SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateDevice(dxgi.get(), &device)) &&
            SUCCEEDED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context));
        check(graphics, "create Direct2D software device");
        if (graphics) {
            cache.SetDeviceContext(context.get());
            for (const float size : {16.0f, 24.0f, 32.0f, 48.0f, 96.0f, 256.0f}) {
                auto* bitmap = cache.BitmapFor(L"", L"", true, FILE_ATTRIBUTE_DIRECTORY, size);
                check(bitmap && bitmap->GetPixelSize().width >= 16,
                      "native folder converts to Direct2D bitmap at requested scale");
            }
            cache.SetDeviceContext(nullptr);
            cache.SetDeviceContext(context.get());
            check(cache.BitmapFor(L"", L"", true, FILE_ATTRIBUTE_DIRECTORY, 32) != nullptr,
                  "device recreation preserves native indices and rebuilds bitmap");
            cache.Reset();
        }
        return ok;
    }
};
}

int main() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool ok = pulse::ui::ShellIconCacheTestAccess::Run();
    if (SUCCEEDED(hr)) CoUninitialize();
    return ok ? 0 : 1;
}

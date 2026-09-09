#include "../ui/thumbnail_cache.h"
#include <cstdio>

namespace pulse::ui {
struct ThumbnailCacheTestAccess {
    static bool Run() {
        ThumbnailCache cache;
        bool ok = true;
        auto check = [&](bool result, const char* name) {
            std::printf("[%s] %s\n", result ? "PASS" : "FAIL", name);
            ok &= result;
        };
        auto request = [&](const std::wstring& key) {
            ThumbnailCache::Request req;
            req.key = key;
            req.epoch = cache.epoch_.load();
            return req;
        };
        auto insert = [&](const std::wstring& key, size_t cost = 1) {
            ThumbnailCache::Item item;
            item.cost = cost;
            cache.StoreResult(request(key), std::move(item));
        };
        for (int i = 0; i < 128; ++i) insert(std::to_wstring(i));
        cache.Touch(cache.items_.at(L"0"));
        insert(L"128");
        check(cache.items_.contains(L"0") && !cache.items_.contains(L"1"),
              "recently used preview survives capacity eviction");
        insert(L"0", 3);
        check(cache.items_.size() == 128 && cache.lru_.size() == 128 &&
              cache.cache_bytes_ == 130, "replacement has one LRU entry and exact accounting");

        auto old = request(L"old");
        cache.Evict();
        cache.pending_.insert(old.key);
        check(!cache.StoreResult(old, {}) && cache.pending_.contains(old.key) &&
              cache.items_.empty(), "old generation cannot erase replacement pending request");

        insert(L"current", 7);
        auto stale = request(L"current");
        stale.details = true;
        stale.identity = L"previous selection";
        cache.latest_details_identity_ = L"new selection";
        check(!cache.StoreResult(stale, {}) && cache.cache_bytes_ == 7 &&
              cache.items_.at(L"current").cost == 7,
              "rapid selection change preserves existing cache accounting");

        cache.Evict();
        insert(L"cold", 8 * 1024 * 1024);
        insert(L"hot", 8 * 1024 * 1024);
        cache.Touch(cache.items_.at(L"cold"));
        insert(L"new", 1);
        check(cache.items_.contains(L"cold") && !cache.items_.contains(L"hot") &&
              cache.cache_bytes_ == 8 * 1024 * 1024 + 1,
              "byte budget evicts least recently used preview");
        cache.Evict();
        for (int i = 0; i < 5; ++i) {
            auto req = request(std::to_wstring(i));
            req.identity = L"animation";
            ThumbnailCache::Item item;
            item.frame_count = 10;
            item.frame_index = static_cast<uint32_t>(i);
            item.animation_identity = req.identity;
            item.cost = 1;
            if (i == 4) cache.Touch(cache.items_.at(L"0"));
            cache.StoreResult(req, std::move(item));
        }
        check(cache.items_.size() == 4 && cache.items_.contains(L"0") &&
              !cache.items_.contains(L"1") && cache.cache_bytes_ == 4,
              "animation keeps four most recently used frames");
        cache.Reset();
        check(cache.items_.empty() && cache.lru_.empty() && cache.cache_bytes_ == 0,
              "reset clears all cache state");

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
        check(graphics, "create preview software rendering device");
        if (graphics) {
            cache.SetDeviceContext(context.get());
            ComPtr<ID2D1Bitmap1> target, readback;
            auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            bool ready = SUCCEEDED(context->CreateBitmap(D2D1::SizeU(100, 100),
                nullptr, 0, &props, &target));
            props.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
            ready = ready && SUCCEEDED(context->CreateBitmap(D2D1::SizeU(100, 100),
                nullptr, 0, &props, &readback));
            check(ready, "create preview render and readback targets");
            if (ready) {
                context->SetTarget(target.get());
                for (bool portrait : {false, true}) {
                    cache.Evict();
                    ThumbnailCache::Item item;
                    item.w = portrait ? 100u : 200u;
                    item.h = portrait ? 200u : 100u;
                    item.stride = item.w * 4;
                    item.pixels.assign(static_cast<size_t>(item.stride) * item.h, 255);
                    item.cost = item.pixels.size();
                    item.kind = ipc::PreviewContentKind::Bitmap;
                    cache.StoreResult(request(cache.Key(L"image", 200, 0, 0)), std::move(item));
                    for (float offset : {0.0f, 50.0f, 100.0f, 200.0f}) {
                        float x = offset, y = offset;
                        context->BeginDraw();
                        context->Clear(D2D1::ColorF(0, 0.0f));
                        const auto result = cache.Draw(context.get(), D2D1::RectF(0, 0, 100, 100),
                            L"image", 0, 200, 0, 0, 0, 1.0f, nullptr, nullptr, nullptr,
                            false, nullptr, &x, &y);
                        bool filled = result == PreviewDrawResult::Bitmap && SUCCEEDED(context->EndDraw());
                        filled = filled && SUCCEEDED(readback->CopyFromBitmap(nullptr, target.get(), nullptr));
                        D2D1_MAPPED_RECT mapped{};
                        if (filled && SUCCEEDED(readback->Map(D2D1_MAP_OPTIONS_READ, &mapped))) {
                            for (UINT row = 0; row < 100; ++row)
                                for (UINT col = 0; col < 100; ++col)
                                    filled &= mapped.bits[row * mapped.pitch + col * 4 + 3] == 255;
                            readback->Unmap();
                        } else {
                            filled = false;
                        }
                        check(filled, "cover preview remains filled at pan boundaries");
                    }
                }
                context->SetTarget(nullptr);
            }
            cache.Reset();
        }
        return ok;
    }
};
}

bool RunThumbnailCacheTests() {
    return pulse::ui::ThumbnailCacheTestAccess::Run();
}

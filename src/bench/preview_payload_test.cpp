#include "../ui/thumbnail_cache.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <array>

namespace pulse::ui {
struct ThumbnailCacheTestAccess {
    static int Run(bool bench) {
        ComPtr<ID3D11Device> d3d;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
        ComPtr<IDXGIDevice> dxgi;
        if (SUCCEEDED(hr)) hr = d3d->QueryInterface(IID_PPV_ARGS(&dxgi));
        ComPtr<ID2D1Device> device;
        if (SUCCEEDED(hr)) hr = D2D1CreateDevice(dxgi.get(), nullptr, &device);
        ComPtr<ID2D1DeviceContext> dc;
        if (SUCCEEDED(hr)) hr = device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc);
        if (FAILED(hr)) return 2;
        ThumbnailCache cache;
        cache.SetDeviceContext(dc.get());
        cache.running_ = true; // Queue deterministic requests without starting workers.
        bool ok = true;
        const auto check = [&](bool value, const char* name) {
            std::printf("[%s] %s\n", value ? "PASS" : "FAIL", name); ok &= value;
        };
        const auto draw = [&](const wchar_t* path, std::wstring* copy,
                              std::shared_ptr<const std::wstring>* shared, uint32_t frame = 0,
                              const std::shared_ptr<DocumentImageSession>& document = {}) {
            return cache.Draw(dc.get(), D2D1::RectF(0, 0, 100, 100), path,
                0, 512, 1, 2, 3, 1, copy, nullptr, nullptr, true, nullptr,
                nullptr, nullptr, nullptr, nullptr, frame, nullptr, nullptr, nullptr,
                nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false, nullptr, nullptr,
                nullptr, document, shared);
        };
        const auto load = [&](const wchar_t* path, ipc::PreviewContentKind kind, std::wstring text) {
            draw(path, nullptr, nullptr);
            const auto req = cache.queue_.front(); cache.queue_.clear();
            ThumbnailCache::Item item; item.kind = kind; item.text = std::move(text);
            item.cost = item.text.size() * sizeof(wchar_t);
            return cache.StoreResult(req, std::move(item));
        };
        std::shared_ptr<const std::wstring> holder;
        const std::wstring payload(4 * 1024 * 1024, L'X');
        check(load(L"large.csv", ipc::PreviewContentKind::Table, payload), "store real cache payload");
        const auto bytes = cache.cache_bytes_;
        draw(L"large.csv", nullptr, &holder);
        const auto first = holder;
        check(holder && *holder == payload && bytes == cache.cache_bytes_, "promotion preserves full content and cache byte accounting");
        draw(L"large.csv", nullptr, &holder);
        check(holder == first, "warm hit reuses immutable allocation");
        std::wstring copy;
        draw(L"large.csv", &copy, &holder);
        check(copy == payload && holder == first, "legacy and shared outputs work together after promotion");
        draw(L"other.csv", nullptr, &holder);
        const auto stale_request = cache.queue_.front();
        check(!holder, "pending different path clears shared output");
        draw(L"large.csv", nullptr, &holder, 1);
        check(!holder, "pending different sheet clears shared output");
        cache.Evict();
        check(*first == payload && cache.cache_bytes_ == 0, "eviction releases cache ownership but live view remains valid");
        ThumbnailCache::Item stale; stale.kind = ipc::PreviewContentKind::Text; stale.text = L"stale";
        check(!cache.StoreResult(stale_request, std::move(stale)), "obsolete epoch cannot restore old payload");
        check(load(L"large.csv", ipc::PreviewContentKind::Table, L"replacement"), "replacement request loads");
        draw(L"large.csv", nullptr, &holder);
        check(holder && *holder == L"replacement" && *first == payload, "replacement does not mutate active old view");
        auto document = std::make_shared<DocumentImageSession>(); document->active = false;
        draw(L"large.csv", nullptr, &holder, 0, document);
        check(!holder, "cancelled document clears output");
        draw(L"", nullptr, &holder);
        check(!holder, "invalid request clears output");
        cache.Evict();
        check(load(L"failed", ipc::PreviewContentKind::None, L""), "cache failed content");
        holder = first; draw(L"failed", nullptr, &holder);
        check(!holder, "failure cannot expose previous payload");
        for (const auto kind : {ipc::PreviewContentKind::Text, ipc::PreviewContentKind::Hex,
                ipc::PreviewContentKind::Archive, ipc::PreviewContentKind::Markdown,
                ipc::PreviewContentKind::Table, ipc::PreviewContentKind::Tree}) {
            cache.Evict(); load(L"kind", kind, L"body"); draw(L"kind", &copy, &holder);
            check(holder && *holder == L"body" && copy == L"body", "structured/text kind preserves legacy content");
        }
        for (const auto kind : {ipc::PreviewContentKind::Text, ipc::PreviewContentKind::Bitmap}) {
            cache.Evict(); load(L"empty", kind, L""); holder = first; copy = L"old";
            draw(L"empty", &copy, &holder);
            const auto& entry = cache.items_.at(cache.Key(L"empty", 512, 2, 3));
            check(!holder && !entry.shared_text && copy.empty(), "empty text/bitmap metadata does not allocate a shared string");
        }
        if (bench && ok) {
            for (const size_t chars : {size_t(32768), size_t(4 * 1024 * 1024)}) {
                cache.Evict(); load(L"bench", ipc::PreviewContentKind::Table, std::wstring(chars, L'B'));
                draw(L"bench", nullptr, &holder); holder.reset();
                std::array<double, 10> legacy{}, shared{};
                size_t checksum = 0;
                const auto measure = [&](bool share) {
                    const auto begin = std::chrono::steady_clock::now();
                    for (int frame = 0; frame < 100; ++frame) {
                        std::wstring local; std::shared_ptr<const std::wstring> owned;
                        draw(L"bench", share ? nullptr : &local, share ? &owned : nullptr);
                        checksum += share ? owned->size() : local.size();
                    }
                    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
                };
                for (size_t round = 0; round < legacy.size(); ++round) {
                    if (round % 2) { shared[round] = measure(true); legacy[round] = measure(false); }
                    else { legacy[round] = measure(false); shared[round] = measure(true); }
                    std::printf("sample chars=%zu round=%zu frames=100 legacy_ms=%.6f shared_ms=%.6f\n", chars, round, legacy[round], shared[round]);
                }
                std::sort(legacy.begin(), legacy.end()); std::sort(shared.begin(), shared.end());
                std::printf("median chars=%zu legacy_ms=%.6f shared_ms=%.6f copied_payload_bytes=%zu shared_payload_bytes=0 checksum=%zu\n",
                    chars, (legacy[4]+legacy[5])/2, (shared[4]+shared[5])/2, chars*sizeof(wchar_t)*100, checksum);
            }
        }
        cache.running_ = false;
        return ok ? 0 : 1;
    }
};
}
int main(int argc, char** argv) {
    return pulse::ui::ThumbnailCacheTestAccess::Run(argc == 2 && std::strcmp(argv[1], "--bench") == 0);
}

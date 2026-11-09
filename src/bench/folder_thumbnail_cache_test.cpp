#include "../ui/folder_thumbnail_cache.h"
#include <cstdio>
#include <string_view>

namespace pulse::ui {
struct FolderThumbnailCacheTestAccess {
    static int Live(const wchar_t* folder) {
        wchar_t full[32768]{};
        const DWORD length = GetFullPathNameW(folder, ARRAYSIZE(full), full, nullptr);
        if (!length || length >= ARRAYSIZE(full)) return 2;
        const DWORD attrs = GetFileAttributesW(full);
        if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            std::printf("[FAIL] live fixture must be an existing directory\n"); return 2;
        }
        ComPtr<ID3D11Device> d3d;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
        ComPtr<IDXGIDevice> dxgi;
        if (SUCCEEDED(hr)) hr = d3d->QueryInterface(IID_PPV_ARGS(&dxgi));
        ComPtr<ID2D1Device> device;
        if (SUCCEEDED(hr)) hr = D2D1CreateDevice(dxgi.get(), nullptr, &device);
        ComPtr<ID2D1DeviceContext> dc;
        if (SUCCEEDED(hr)) hr = device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc);
        ComPtr<ID2D1Bitmap1> target;
        const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (SUCCEEDED(hr)) hr = dc->CreateBitmap(D2D1::SizeU(256, 256), nullptr, 0, &props, &target);
        if (FAILED(hr)) { std::printf("[FAIL] live WARP target: %08lx\n", hr); return 2; }
        dc->SetTarget(target.get());
        FolderThumbnailCache cache;
        cache.SetDeviceContext(dc.get());
        const auto start = GetTickCount64();
        PreviewDrawResult result = PreviewDrawResult::Pending;
        do {
            cache.BeginFrame();
            dc->BeginDraw();
            dc->Clear(D2D1::ColorF(0, 0.0f));
            result = cache.Draw(dc.get(), D2D1::RectF(0, 0, 256, 256), full, attrs,
                256, 1, 1, 0, 1, false, nullptr);
            hr = dc->EndDraw();
            cache.EndFrame();
            if (result == PreviewDrawResult::Bitmap || FAILED(hr)) break;
            Sleep(15);
        } while (GetTickCount64() - start < 5000);
        const auto stats = cache.ReadDebugStats();
        std::printf("[LIVE] visible=%zu queued=%zu pending=%zu cached=%zu bitmaps=%zu failed=%zu blocked=%zu "
            "oldest=%llu running=%d context=%d error=%ls elapsed=%llu draw=%d hr=%08lx\n",
            stats.visible, stats.queued, stats.pending, stats.cached, stats.bitmaps, stats.failed,
            stats.blocked, static_cast<unsigned long long>(stats.oldest_visible_ms), stats.running,
            stats.has_context, stats.error.c_str(), static_cast<unsigned long long>(GetTickCount64() - start),
            static_cast<int>(result), hr);
        const bool ok = result == PreviewDrawResult::Bitmap && SUCCEEDED(hr);
        std::printf("[%s] live folder worker IPC upload and WARP drawing\n", ok ? "PASS" : "FAIL");
        return ok ? 0 : 1;
    }
    static int Run() {
        ComPtr<ID3D11Device> d3d;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
        ComPtr<IDXGIDevice> dxgi;
        if (SUCCEEDED(hr)) hr = d3d->QueryInterface(IID_PPV_ARGS(&dxgi));
        ComPtr<ID2D1Device> device;
        if (SUCCEEDED(hr)) hr = D2D1CreateDevice(dxgi.get(), nullptr, &device);
        ComPtr<ID2D1DeviceContext> dc;
        if (SUCCEEDED(hr)) hr = device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc);
        if (FAILED(hr)) { std::printf("[FAIL] WARP initialization: %08lx\n", hr); return 1; }
        FolderThumbnailCache folders;
        auto& cache = folders.cache_;
        folders.SetDeviceContext(dc.get());
        cache.running_ = true; // Deterministic scheduler; inject worker completions below.
        bool ok = true;
        auto check = [&](bool condition, const char* label) {
            std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", label); ok &= condition;
        };
        auto draw = [&](const wchar_t* path, uint64_t generation = 1, bool single = false) {
            return folders.Draw(dc.get(), D2D1::RectF(0, 0, 100, 100), path,
                FILE_ATTRIBUTE_DIRECTORY, 64, generation, 1, 0, 1, false, nullptr, single);
        };
        auto settle = [&] { for (auto& [key, visible] : folders.visible_) visible.since = GetTickCount64() - FolderThumbnailCache::kSettleMs; };
        auto bitmap = [] {
            ThumbnailCache::Item item;
            item.kind = ipc::PreviewContentKind::Bitmap;
            item.w = item.h = 2; item.stride = 8; item.cost = 16;
            item.pixels.assign(16, 255);
            return item;
        };
        folders.BeginFrame(); draw(L"A"); folders.EndFrame();
        check(cache.queue_.empty() && !folders.Tick(GetTickCount64()), "newly visible folders wait for settling");
        const auto first_seen = folders.visible_.begin()->second.since;
        check(!folders.Tick(first_seen + 40), "brief scrolling exposure does not start folder decoding");
        check(folders.Tick(first_seen + 80), "stable folders become eligible within 80 milliseconds");
        settle(); check(folders.Tick(GetTickCount64()), "settling wakes idle rendering without pointer motion");
        draw(L"A"); auto obsolete = cache.queue_.front();
        folders.BeginFrame(); draw(L"B"); folders.EndFrame();
        check(cache.queue_.empty() && cache.pending_.empty() && !cache.FolderRequestCurrent(obsolete),
            "scrolling cancels queued and active invisible folder work");
        check(!cache.StoreResult(obsolete, bitmap()) && cache.items_.empty(), "cancelled result cannot enter cache");
        settle(); draw(L"B"); obsolete = cache.queue_.front();
        draw(L"B", 2); settle(); draw(L"B", 2); auto current = cache.queue_.front();
        check(current.id != obsolete.id && !cache.StoreResult(obsolete, bitmap()) && cache.pending_.size() == 1,
            "rapid generation change rejects stale response without clearing replacement");
        cache.queue_.clear(); cache.StoreResult(current, bitmap());
        check(draw(L"B", 2) == PreviewDrawResult::Bitmap && !folders.Tick(GetTickCount64()),
            "completed bitmap is retained until TTL");
        ThumbnailCache ordinary;
        folders.ShareUploadBudget(ordinary);
        folders.BeginFrame();
        ordinary.shared_upload_budget_->bytes = 4194304;
        cache.items_.at(current.key).bitmap.reset();
        check(draw(L"B", 2) == PreviewDrawResult::Pending && folders.TakeUploadFrameRequest(),
            "ordinary uploads exhaust shared folder frame budget and request retry");
        folders.BeginFrame();
        check(draw(L"B", 2) == PreviewDrawResult::Bitmap &&
            !cache.items_.at(current.key).pixels.empty() && ordinary.shared_upload_budget_->bytes == 16,
            "next frame uploads folder once and retains CPU recovery pixels");
        const auto uploaded_bytes = ordinary.shared_upload_budget_->bytes.load();
        draw(L"B", 2);
        check(ordinary.shared_upload_budget_->bytes == uploaded_bytes,
            "warm folder draw performs no upload");
        static bool upload_was_unlocked = false;
        const auto original_epoch = cache.epoch_.load(std::memory_order_relaxed);
        cache.items_.at(current.key).bitmap.reset();
        cache.upload_probe_ = [](ThumbnailCache& target) {
            upload_was_unlocked = target.mutex_.try_lock();
            if (upload_was_unlocked) target.mutex_.unlock();
            target.epoch_.fetch_add(1, std::memory_order_relaxed);
        };
        folders.BeginFrame();
        draw(L"B", 2);
        check(upload_was_unlocked && !cache.items_.at(current.key).bitmap.get() &&
            !cache.items_.at(current.key).pixels.empty(),
            "folder uploads outside mutex and rejects changed epoch without losing recovery pixels");
        cache.upload_probe_ = nullptr;
        cache.epoch_.store(original_epoch, std::memory_order_relaxed);
        folders.BeginFrame();
        draw(L"B", 2);
        cache.items_.at(current.key).retry_at = GetTickCount64();
        check(folders.Tick(GetTickCount64()), "expired artwork schedules child-mtime validation");
        folders.Refresh();
        check(draw(L"B", 2) == PreviewDrawResult::Bitmap && cache.queue_.size() == 1,
            "explicit refresh keeps old bitmap visible while replacement loads");
        auto refresh = cache.queue_.front(); cache.queue_.clear();
        check((refresh.flags & ipc::kPreviewRequestFlagFolderRefresh) != 0 &&
            !(current.flags & ipc::kPreviewRequestFlagFolderRefresh), "only explicit refresh forces host decoding");
        check(cache.IsTransientResponse(true, refresh, ERROR_TIMEOUT) &&
            !cache.IsTransientResponse(true, refresh, 1) &&
            !cache.IsTransientResponse(true, refresh, 0),
            "folder host timeout is transient while completed and unsupported responses are not");
        ThumbnailCache::Request regular;
        check(!cache.IsTransientResponse(true, regular, ERROR_TIMEOUT) &&
            cache.IsTransientResponse(false, regular, 0),
            "folder timeout policy leaves ordinary provider response classification unchanged");
        ThumbnailCache::Item failed; failed.failed = true;
        failed.transient = cache.IsTransientResponse(true, refresh, ERROR_TIMEOUT);
        cache.StoreResult(refresh, std::move(failed));
        check(draw(L"B", 2) == PreviewDrawResult::Bitmap && cache.queue_.empty(),
            "host budget timeout preserves old bitmap with negative retry TTL");
        cache.items_.at(refresh.key).retry_at = 0;
        draw(L"B", 2);
        check((cache.queue_.front().flags & ipc::kPreviewRequestFlagFolderRefresh) != 0,
            "explicit refresh remains forced after transient timeout");
        auto emptied = cache.queue_.front(); cache.queue_.clear();
        ThumbnailCache::Item unsupported; unsupported.failed = true;
        cache.StoreResult(emptied, std::move(unsupported));
        check(draw(L"B", 2) == PreviewDrawResult::Failed && !cache.still_by_identity_.contains(emptied.identity),
            "authoritative empty result removes old artwork and stale-size fallback");
        check(!folders.visible_.at(emptied.key).force, "authoritative empty response consumes refresh force");
        draw(L"B", 2, true); settle(); draw(L"B", 2, true);
        current = cache.queue_.front(); cache.queue_.clear();
        check((current.flags & ipc::kPreviewRequestFlagFolderSingle) != 0 && current.key != refresh.key,
            "single and stacked artwork have independent cache keys");
        cache.max_items_ = 1; cache.StoreResult(current, bitmap());
        check(cache.items_.size() == 1 && cache.cache_bytes_ == 32, "LRU budget counts retained CPU and GPU pixels");
        folders.SetDeviceContext(nullptr); folders.SetDeviceContext(dc.get());
        check(draw(L"B", 2, true) == PreviewDrawResult::Bitmap, "retained pixels recover after device context reset");
        folders.BeginFrame(); draw(L"empty"); folders.EndFrame(); settle(); draw(L"empty");
        current = cache.queue_.front(); cache.queue_.clear();
        ThumbnailCache::Item empty; empty.failed = true;
        cache.StoreResult(current, std::move(empty));
        check(draw(L"empty") == PreviewDrawResult::Failed && cache.queue_.empty(),
            "empty folder uses negative cache rather than decoding on every paint");
        folders.BeginFrame(); draw(L"large"); folders.EndFrame(); settle(); draw(L"large");
        current = cache.queue_.front(); cache.queue_.clear();
        cache.budget_bytes_ = 16; cache.StoreResult(current, bitmap());
        check(cache.items_.empty() && cache.cache_bytes_ == 0, "byte budget evicts oversized artwork");
        folders.BeginFrame(); folders.EndFrame();
        check(folders.visible_.empty() && !folders.Tick(GetTickCount64() + 60000),
            "leaving icon view stops all folder scheduling");
        FolderThumbnailCache pressure;
        pressure.SetDeviceContext(dc.get());
        auto& packed = pressure.cache_;
        packed.running_ = true;
        auto pressure_draw = [&](int i) {
            return pressure.Draw(dc.get(), D2D1::RectF(0, 0, 256, 256), L"pressure" + std::to_wstring(i),
                FILE_ATTRIBUTE_DIRECTORY, 512, 1, 1, 0, 1, false, nullptr);
        };
        pressure.BeginFrame();
        for (int i = 0; i < 20; ++i) pressure_draw(i);
        pressure.EndFrame();
        for (auto& [key, visible] : pressure.visible_) visible.since = GetTickCount64() - 160;
        pressure.BeginFrame();
        for (int i = 0; i < 20; ++i) pressure_draw(i);
        pressure.EndFrame();
        check(packed.queue_.size() == 8, "20 high-DPI visible folders admit only eight within 16MB");
        while (!packed.queue_.empty()) {
            auto request = packed.queue_.front(); packed.queue_.pop_front();
            auto image = bitmap();
            image.w = image.h = 512; image.stride = 2048;
            image.pixels.assign(512 * 512 * 4, 255); image.cost = image.pixels.size();
            packed.StoreResult(request, std::move(image));
        }
        for (int paint = 0; paint < 8; ++paint) {
            pressure.BeginFrame();
            for (int i = 0; i < 20; ++i) pressure_draw(i);
            pressure.EndFrame();
        }
        check(packed.items_.size() == 8 && packed.cache_bytes_ == 16ull * 1024 * 1024 &&
            packed.queue_.empty() && !pressure.Tick(GetTickCount64()),
            "over-budget stable viewport stays idle across repeated paints without LRU decode thrash");
        pressure.BeginFrame();
        for (int i = 8; i < 20; ++i) pressure_draw(i);
        pressure.EndFrame();
        check(pressure.Tick(GetTickCount64()), "scrolling releases reservations and wakes newly admitted folders");
        pressure.BeginFrame();
        for (int i = 8; i < 20; ++i) pressure_draw(i);
        pressure.EndFrame();
        check(packed.queue_.size() == 8, "changed viewport remains bounded while admitting replacement folders");
        FolderThumbnailCache negatives;
        negatives.SetDeviceContext(dc.get());
        auto& negative_cache = negatives.cache_;
        negative_cache.running_ = true; negative_cache.max_items_ = 2;
        const auto negative_draw = [&](int index) {
            return negatives.Draw(dc.get(), D2D1::RectF(0, 0, 64, 64), L"negative-pressure" + std::to_wstring(index),
                FILE_ATTRIBUTE_DIRECTORY, 64, 1, 1, 0, 1, false, nullptr);
        };
        negatives.BeginFrame();
        for (int i = 0; i < 3; ++i) negative_draw(i);
        negatives.EndFrame();
        for (auto& [key, visible] : negatives.visible_) visible.since = GetTickCount64() - 160;
        size_t decoded = 0;
        std::wstring good_key;
        bool kept_good = true;
        for (int paint = 0; paint < 12; ++paint) {
            negatives.BeginFrame();
            for (int i = 0; i < 3; ++i) {
                const auto drawn = negative_draw(i);
                if (paint > 0 && i == 0) kept_good &= drawn == PreviewDrawResult::Bitmap;
            }
            negatives.EndFrame();
            while (!negative_cache.queue_.empty()) {
                auto request = negative_cache.queue_.front(); negative_cache.queue_.pop_front();
                if (request.path == L"negative-pressure0") {
                    good_key = request.key; negative_cache.StoreResult(request, bitmap());
                } else {
                    ThumbnailCache::Item negative_result; negative_result.failed = true;
                    negative_cache.StoreResult(request, std::move(negative_result));
                }
                ++decoded;
            }
        }
        check(kept_good && negative_cache.items_.contains(good_key) && negative_cache.items_.size() == 2,
            "negative folder entries never evict visible successful artwork at item capacity");
        check(decoded == 3 && negative_cache.queue_.empty() && !negatives.Tick(GetTickCount64()),
            "evicted negative entries do not ping-pong decode in a stable viewport");

        FolderThumbnailCache timeout;
        timeout.SetDeviceContext(dc.get());
        auto& retry_cache = timeout.cache_;
        retry_cache.running_ = true;
        const auto retry_draw = [&] {
            return timeout.Draw(dc.get(), D2D1::RectF(0, 0, 64, 64), L"slow-folder",
                FILE_ATTRIBUTE_DIRECTORY, 64, 1, 1, 0, 1, false, nullptr);
        };
        retry_draw(); timeout.Refresh();
        for (auto& [key, visible] : timeout.visible_) visible.since = GetTickCount64() - 160;
        retry_draw();
        constexpr uint64_t backoff[] = {500, 2000, 5000, 30000};
        ThumbnailCache::Request retried;
        for (const auto delay : backoff) {
            retried = retry_cache.queue_.front(); retry_cache.queue_.clear();
            ThumbnailCache::Item timed_out;
            timed_out.failed = true;
            timed_out.transient = retry_cache.IsTransientResponse(true, retried, ERROR_TIMEOUT);
            retry_cache.StoreResult(retried, std::move(timed_out));
            const auto now = GetTickCount64();
            const auto deadline = retry_cache.items_.at(retried.key).retry_at;
            check(deadline > now && deadline <= now + delay && deadline + 100 >= now + delay &&
                retry_draw() == PreviewDrawResult::Failed && retry_cache.queue_.empty() && !timeout.Tick(now),
                "initial folder timeout uses bounded backoff without per-paint retry");
            retry_cache.items_.at(retried.key).retry_at = 0;
            retry_draw();
        }
        retried = retry_cache.queue_.front(); retry_cache.queue_.clear();
        retry_cache.StoreResult(retried, bitmap());
        retry_draw();
        check(retry_cache.transient_failures_.empty() && !timeout.visible_.at(retried.key).force,
            "successful folder retry clears timeout attempts and refresh force");
        retry_cache.transient_failures_[retried.key] = 2;
        timeout.BeginFrame(); timeout.EndFrame();
        check(retry_cache.transient_failures_.empty(), "leaving viewport clears folder timeout attempts");
        return ok ? 0 : 1;
    }
};
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring_view(argv[1]) == L"--live")
        return pulse::ui::FolderThumbnailCacheTestAccess::Live(argv[2]);
    return pulse::ui::FolderThumbnailCacheTestAccess::Run();
}

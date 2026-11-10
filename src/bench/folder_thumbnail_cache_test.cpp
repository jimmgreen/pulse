#include "../ui/folder_thumbnail_cache.h"
#include <cstdio>
#include <string_view>

namespace pulse::ui {
struct FolderThumbnailCacheTestAccess {
#include "folder_thumbnail_revisit_live.inc"
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
    static bool Revisit(ID2D1DeviceContext* dc) {
        FolderThumbnailCache folders;
        folders.SetDeviceContext(dc);
        auto& cache = folders.cache_;
        cache.running_ = true; // Inject completions without starting a host.
        bool loaded = true;
        const auto draw = [&](int index) {
            return folders.Draw(dc, D2D1::RectF(0, 0, 512, 512),
                L"long-scroll-return" + std::to_wstring(index), FILE_ATTRIBUTE_DIRECTORY,
                512, 1, 1, 0, 1, false, nullptr);
        };
        for (int first = 0; first < 40; first += 8) {
            folders.BeginFrame();
            for (int i = first; i < first + 8; ++i) draw(i);
            folders.EndFrame();
            for (auto& [key, visible] : folders.visible_)
                visible.since = GetTickCount64() - 160;
            folders.BeginFrame();
            for (int i = first; i < first + 8; ++i) draw(i);
            folders.EndFrame();
            loaded &= cache.queue_.size() == 8;
            while (!cache.queue_.empty()) {
                auto request = cache.queue_.front();
                cache.queue_.pop_front();
                ThumbnailCache::Item item;
                item.kind = ipc::PreviewContentKind::Bitmap;
                item.w = item.h = 512;
                item.stride = 2048;
                item.pixels.assign(512 * 512 * 4, 255);
                item.cost = item.pixels.size();
                loaded &= cache.StoreResult(request, std::move(item));
            }
            // Give each upload a fresh frame budget while preserving the
            // screen's visibility, then prove every source image was drawn.
            for (int i = first; i < first + 8; ++i) {
                cache.BeginFrame();
                loaded &= draw(i) == PreviewDrawResult::Bitmap;
            }
        }
        std::printf("[%s] five screens load and draw forty distinct 512px folder thumbnails\n",
            loaded ? "PASS" : "FAIL");
        const bool bounded = cache.cache_bytes_ <= cache.budget_bytes_ && cache.items_.size() < 40;
        std::printf("[%s] full-resolution cache stays bounded beyond its resident capacity\n",
            bounded ? "PASS" : "FAIL");
        folders.BeginFrame();
        int warm = 0;
        for (int i = 0; i < 8; ++i) warm += draw(i) == PreviewDrawResult::Bitmap;
        folders.EndFrame();
        const bool immediate = warm == 8 && cache.upload_bytes_ == 0 &&
            cache.folder_revisit_ && cache.folder_revisit_->upload_bytes_ == 0 &&
            cache.queue_.empty() && cache.pending_.empty();
        std::printf("[%s] returning after forty folders immediately draws eight thumbnails without decode or upload (warm=%d upload=%zu)\n",
            immediate ? "PASS" : "FAIL", warm, cache.upload_bytes_);
        return loaded && bounded && immediate;
    }
    static bool RevisitBoundaries(ID2D1DeviceContext* dc) {
        FolderThumbnailCache folders;
        folders.SetDeviceContext(dc);
        auto& cache = folders.cache_;
        cache.running_ = true;
        bool ok = true;
        const auto check = [&](bool value, const char* label) {
            std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label);
            ok &= value;
        };
        check(cache.folder_revisit_ != nullptr, "folder revisit tier exists");
        if (!cache.folder_revisit_) return false;
        auto& compact = *cache.folder_revisit_;
        const auto request = [&](int index) {
            ThumbnailCache::Request req;
            req.id = cache.next_id_++;
            req.path = L"revisit-boundary" + std::to_wstring(index);
            req.identity = req.path + L"\nfolder-stack";
            req.key = cache.Key(req.identity, 512, 1, 0);
            req.pixels = 512;
            req.attrs = FILE_ATTRIBUTE_DIRECTORY;
            req.flags = ipc::kPreviewRequestFlagFolderThumbnail;
            req.epoch = cache.epoch_.load(std::memory_order_relaxed);
            cache.folder_requests_[req.key] = req.id;
            cache.pending_.insert(req.key);
            return req;
        };
        const auto image = [](uint8_t value = 255) {
            ThumbnailCache::Item item;
            item.kind = ipc::PreviewContentKind::Bitmap;
            item.w = item.h = 512;
            item.stride = 2048;
            item.pixels.assign(512 * 512 * 4, value);
            item.cost = item.pixels.size();
            return item;
        };
        auto original = request(0);
        cache.StoreResult(original, image());
        compact.PrepareBitmap(original.identity, false);
        bool bounded = true;
        for (int i = 1; i < 80; ++i) {
            auto next = request(i);
            cache.StoreResult(next, image());
            compact.PrepareBitmap(next.identity, false);
            bounded &= compact.cache_bytes_ <= 8ull * 1024 * 1024 &&
                compact.items_.size() <= 128 && compact.items_.size() <= compact.max_items_ &&
                cache.cache_bytes_ + compact.cache_bytes_ <= 40ull * 1024 * 1024;
        }
        check(bounded && !compact.items_.contains(original.identity),
            "eighty folders bound both tiers to forty MiB and evict oldest compact artwork");
        auto retained = request(16);
        check(!cache.items_.contains(retained.key) && compact.items_.contains(retained.identity),
            "compact artwork outlives full-resolution eviction");
        if (!compact.items_.contains(retained.identity)) return false;
        auto* before = compact.items_.at(retained.identity).bitmap.get();
        const auto old_pixels = compact.items_.at(retained.identity).pixels;
        auto stale = retained;
        ++stale.id;
        check(!cache.StoreResult(stale, image(128)) &&
            compact.items_.at(retained.identity).pixels == old_pixels &&
            compact.items_.at(retained.identity).bitmap.get() == before,
            "stale folder completion cannot replace compact artwork");
        ThumbnailCache::Item transient;
        transient.failed = transient.transient = true;
        check(cache.StoreResult(retained, std::move(transient)) &&
            compact.items_.contains(retained.identity),
            "transient failure preserves compact artwork after full-resolution eviction");
        if (!compact.items_.contains(retained.identity)) return false;
        retained = request(16);
        cache.StoreResult(retained, image(128));
        check(compact.items_.at(retained.identity).bitmap.get() == before &&
            compact.items_.at(retained.identity).bitmap_dirty &&
            compact.items_.at(retained.identity).pixels != old_pixels,
            "changed folder pixels keep previous compact bitmap until replacement upload");
        compact.PrepareBitmap(retained.identity, false);
        check(compact.items_.at(retained.identity).bitmap.get() &&
            !compact.items_.at(retained.identity).bitmap_dirty &&
            !compact.items_.at(retained.identity).pixels.empty(),
            "replacement compact upload completes and retains recovery pixels");
        folders.SetDeviceContext(nullptr);
        folders.SetDeviceContext(dc);
        check(!compact.items_.at(retained.identity).bitmap.get() &&
            !compact.items_.at(retained.identity).pixels.empty(),
            "device reset releases compact GPU bitmap and retains pixels");
        compact.PrepareBitmap(retained.identity, false);
        check(compact.items_.at(retained.identity).bitmap.get() != nullptr,
            "compact artwork recovers after device reset");
        retained = request(16);
        auto small_image = image(64);
        small_image.w = small_image.h = 64;
        small_image.stride = 256;
        small_image.pixels.assign(64 * 64 * 4, 64);
        small_image.cost = small_image.pixels.size();
        const auto small_pixels = small_image.pixels;
        check(cache.StoreResult(retained, std::move(small_image)) &&
            compact.items_.contains(retained.identity) &&
            compact.items_.at(retained.identity).w == 64 &&
            compact.items_.at(retained.identity).h == 64 &&
            compact.items_.at(retained.identity).pixels == small_pixels,
            "successful 64px refresh replaces older compact pixels at native resolution");
        retained = request(16);
        ThumbnailCache::Item empty;
        empty.failed = true;
        check(cache.StoreResult(retained, std::move(empty)) &&
            !compact.items_.contains(retained.identity),
            "authoritative empty folder removes compact artwork");
        auto before_evict = request(81);
        cache.StoreResult(before_evict, image());
        auto late = request(82);
        cache.Evict();
        check(cache.items_.empty() && cache.cache_bytes_ == 0 &&
            compact.items_.empty() && compact.cache_bytes_ == 0 &&
            !cache.StoreResult(late, image()) && compact.items_.empty(),
            "eviction clears both tiers and rejects late results from the previous epoch");
        auto after_evict = request(83);
        check(cache.StoreResult(after_evict, image()) && !compact.items_.empty(),
            "current epoch can repopulate compact artwork after eviction");
        folders.Reset();
        check(cache.items_.empty() && cache.cache_bytes_ == 0 &&
            compact.items_.empty() && compact.cache_bytes_ == 0,
            "reset releases full-resolution and compact artwork");
        return ok;
    }
    static int Run(bool revisit_only = false) {
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
        const bool revisit_draw_ok = Revisit(dc.get());
        const bool revisit_boundaries_ok = RevisitBoundaries(dc.get());
        const bool revisit_ok = revisit_draw_ok && revisit_boundaries_ok;
        if (revisit_only) return revisit_ok ? 0 : 1;
        FolderThumbnailCache folders;
        auto& cache = folders.cache_;
        folders.SetDeviceContext(dc.get());
        cache.running_ = true; // Deterministic scheduler; inject worker completions below.
        bool ok = revisit_ok;
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
        // Two disjoint screens at the largest bucket: return must reuse the
        // actual D2D bitmap, with no new host work or GPU upload.
        FolderThumbnailCache scrolling;
        scrolling.SetDeviceContext(dc.get());
        auto& sc = scrolling.cache_;
        sc.running_ = true;
        const auto scroll_draw = [&](int i) {
            return scrolling.Draw(dc.get(), D2D1::RectF(0, 0, 512, 512),
                L"scroll-return" + std::to_wstring(i), FILE_ATTRIBUTE_DIRECTORY,
                512, 1, 1, 0, 1, false, nullptr);
        };
        const auto load_screen = [&](int first) {
            scrolling.BeginFrame();
            for (int i = first; i < first + 8; ++i) scroll_draw(i);
            scrolling.EndFrame();
            for (auto& [key, visible] : scrolling.visible_)
                visible.since = GetTickCount64() - 160;
            for (int i = first; i < first + 8; ++i) scroll_draw(i);
            while (!sc.queue_.empty()) {
                auto request = sc.queue_.front(); sc.queue_.pop_front();
                auto image = bitmap();
                image.w = image.h = 512; image.stride = 2048;
                image.pixels.assign(512 * 512 * 4, 255); image.cost = image.pixels.size();
                sc.StoreResult(request, std::move(image));
            }
            for (int i = first; i < first + 8; ++i) {
                sc.BeginFrame();
                scroll_draw(i);
            }
        };
        load_screen(0);
        load_screen(8);
        check(sc.items_.size() == 16, "two 512px folder screens remain resident");
        scrolling.BeginFrame();
        int warm = 0;
        for (int i = 0; i < 8; ++i)
            warm += scroll_draw(i) == PreviewDrawResult::Bitmap;
        scrolling.EndFrame();
        check(warm == 8 && sc.queue_.empty() && sc.pending_.empty() &&
            sc.upload_bytes_ == 0,
            "return scrolling displays eight warm bitmaps without decode or upload");
        load_screen(16);
        check(sc.cache_bytes_ <= sc.budget_bytes_ && sc.items_.size() <= 16,
            "further scrolling evicts old artwork within the memory ceiling");
        scrolling.Reset();
        check(sc.items_.empty() && sc.cache_bytes_ == 0,
            "closing the cache releases retained scroll artwork");
        FolderThumbnailCache pressure;
        pressure.SetDeviceContext(dc.get());
        auto& packed = pressure.cache_;
        packed.running_ = true;
        packed.budget_bytes_ = 16ull * 1024 * 1024; // Exercise pressure independently of the default.
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
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 4 && std::wstring_view(argv[1]) == L"--live-scroll")
        return pulse::ui::FolderThumbnailCacheTestAccess::LiveScroll(argv[2], argv[3]);
    if (argc == 3 && std::wstring_view(argv[1]) == L"--live")
        return pulse::ui::FolderThumbnailCacheTestAccess::Live(argv[2]);
    return pulse::ui::FolderThumbnailCacheTestAccess::Run(
        argc == 2 && std::wstring_view(argv[1]) == L"--revisit");
}

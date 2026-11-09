#include "../ui/thumbnail_cache.h"
#include "../ui/thumbnail_retry_timer.h"
#include "../ui/thumbnail_artwork_layout.h"
#include <cstdio>
#include <tlhelp32.h>
#include <psapi.h>
#include <filesystem>
#include <fstream>

namespace pulse::ui {
struct ThumbnailCacheTestAccess {
    static bool RealDownloadRetryRegression() {
        bool ok = true;
        const auto check = [&](bool value, const char* name) {
            std::printf("[%s] %s\n", value ? "PASS" : "FAIL", name); std::fflush(stdout); ok &= value;
        };
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const auto directory = std::filesystem::absolute(L"bench_data") /
            (L"download-retry-live-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(directory);
        const auto path = directory / L"downloading.png";
        std::vector<char> complete;
        {
            ComPtr<IWICImagingFactory> factory;
            ComPtr<IWICStream> stream;
            ComPtr<IWICBitmapEncoder> encoder;
            ComPtr<IWICBitmapFrameEncode> frame;
            HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&factory));
            if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
            if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
            if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
            if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.get(), WICBitmapEncoderNoCache);
            if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, nullptr);
            if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
            if (SUCCEEDED(hr)) hr = frame->SetSize(2, 2);
            WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
            if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
            BYTE pixels[] = {0, 0, 255, 0, 0, 255, 0, 0, 255, 0, 0, 255};
            if (SUCCEEDED(hr)) hr = frame->WritePixels(2, 6, sizeof(pixels), pixels);
            if (SUCCEEDED(hr)) hr = frame->Commit();
            if (SUCCEEDED(hr)) hr = encoder->Commit();
            check(SUCCEEDED(hr), "encode isolated complete PNG fixture with WIC");
        }
        if (ok) {
            std::ifstream input(path, std::ios::binary);
            complete.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
            check(complete.size() > 32, "complete PNG contains image data beyond partial header");
        }
        ComPtr<ID3D11Device> d3d;
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<ID2D1Device> device;
        ComPtr<ID2D1DeviceContext> dc;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
        if (SUCCEEDED(hr)) hr = d3d->QueryInterface(IID_PPV_ARGS(&dxgi));
        if (SUCCEEDED(hr)) hr = D2D1CreateDevice(dxgi.get(), nullptr, &device);
        if (SUCCEEDED(hr)) hr = device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc);
        check(SUCCEEDED(hr), "create real graphics context for partial download retry");
        if (ok) {
            const auto write = [&](size_t length) {
                std::ofstream output(path, std::ios::binary | std::ios::trunc);
                output.write(complete.data(), static_cast<std::streamsize>(length));
                output.close();
                return output.good();
            };
            check(write(24), "write only PNG header to same download path");
            ThumbnailCache cache;
            cache.SetParallelism(1); cache.SetDeviceContext(dc.get());
            const uint64_t frozen_size = complete.size();
            const auto key = cache.Key(path.wstring(), 128, 10, frozen_size);
            const auto draw = [&] {
                return cache.DrawGridThumbnail(dc.get(), D2D1::RectF(0, 0, 128, 128),
                    path.wstring(), FILE_ATTRIBUTE_NORMAL, 128, 1, 10, frozen_size, 1, false, nullptr);
            };
            draw();
            bool rejected = false;
            const auto first_deadline = GetTickCount64() + 15000;
            while (!rejected && GetTickCount64() < first_deadline) {
                { std::lock_guard lock(cache.mutex_);
                  const auto item = cache.items_.find(key);
                  rejected = item != cache.items_.end() && item->second.failed &&
                      item->second.transient && !cache.pending_.contains(key); }
                if (!rejected) Sleep(10);
            }
            check(rejected, "real preview host rejects incomplete PNG and schedules retry");
            check(write(complete.size()), "finish PNG at identical path without changing snapshot identity");
            bool bitmap = false;
            bool red_pixels = false;
            const auto retry_deadline = GetTickCount64() + 15000;
            while (!bitmap && GetTickCount64() < retry_deadline) {
                bool ready_to_draw = false;
                { std::lock_guard lock(cache.mutex_);
                  const auto item = cache.items_.find(key);
                  ready_to_draw = !cache.pending_.contains(key);
                  red_pixels |= item != cache.items_.end() && !item->second.failed &&
                      item->second.pixels.size() >= 4 && item->second.pixels[2] == 255 &&
                      item->second.pixels[1] == 0; }
                if (ready_to_draw) bitmap = draw() == PreviewDrawResult::Bitmap;
                if (!bitmap) Sleep(10);
            }
            check(bitmap, "real draw retries host and uploads completed PNG bitmap at unchanged size and mtime key");
            {
                std::lock_guard lock(cache.mutex_);
                const auto item = cache.items_.find(key);
                if (item != cache.items_.end())
                    std::printf("[INFO] decoded PNG width=%u height=%u red_before_upload=%d failed=%d retry_history=%d\n",
                        item->second.w, item->second.h, red_pixels, item->second.failed,
                        cache.transient_failures_.contains(key));
                check(item != cache.items_.end() && !item->second.failed && item->second.w == 2 &&
                    item->second.h == 2 && red_pixels &&
                    !cache.transient_failures_.contains(key), "real decoded pixels are red and retry history is cleared");
            }
            ThumbnailCache::Request stale;
            stale.key = key; stale.path = path.wstring(); stale.epoch = cache.epoch_.load() - 1;
            ThumbnailCache::Item failed; failed.failed = true;
            check(!cache.StoreResult(stale, std::move(failed)) && draw() == PreviewDrawResult::Bitmap,
                "cancelled old decode cannot overwrite actual completed download bitmap");
            cache.Reset();
        }
        std::error_code error;
        std::filesystem::remove(path, error);
        if (!error) std::filesystem::remove(directory, error);
        check(!error, "remove isolated partial download fixture after host shutdown");
        if (SUCCEEDED(initialized)) CoUninitialize();
        return ok;
    }
    static bool DownloadRetryRegression() {
        bool ok = true;
        const auto check = [&](bool value, const char* name) {
            std::printf("[%s] %s\n", value ? "PASS" : "FAIL", name); ok &= value;
        };
        ThumbnailCache cache;
        ThumbnailCache::Request req;
        req.path = L"downloading.PNG"; req.pixels = 256;
        req.key = cache.Key(req.path, req.pixels, 10, 100);
        req.identity = cache.Key(req.path, 0, 10, 100);
        req.epoch = cache.epoch_.load();
        bool retries = true;
        for (unsigned i = 0; i < 8; ++i) {
            ThumbnailCache::Item failed; failed.failed = true;
            const auto before = GetTickCount64();
            cache.StoreResult(req, std::move(failed));
            const auto& stored = cache.items_.at(req.key);
            retries &= stored.transient && stored.retry_at > before && stored.retry_at <= GetTickCount64() + 30000;
        }
        check(retries, "incomplete image remains retryable after a long download with bounded backoff");
        check(cache.Key(req.path, req.pixels, 10, 101) != req.key &&
            cache.Key(req.path, req.pixels, 11, 100) != req.key,
            "size or timestamp updates bypass old negative cache");
        ThumbnailCache::Item good; good.kind = ipc::PreviewContentKind::Bitmap;
        cache.StoreResult(req, std::move(good));
        check(!cache.items_.at(req.key).failed && !cache.transient_failures_.contains(req.key),
            "completed image replaces failure and clears retry history");
        cache.Evict(); cache.pending_.insert(req.key);
        ThumbnailCache::Item old; old.failed = true;
        check(!cache.StoreResult(req, std::move(old)) && cache.pending_.contains(req.key),
            "cancelled generation cannot replace completion or remove new pending request");
        req.epoch = cache.epoch_.load(); req.path = L"unsupported.xyz";
        ThumbnailCache::Item unsupported; unsupported.failed = true;
        cache.StoreResult(req, std::move(unsupported));
        check(!cache.items_.at(req.key).transient, "unsupported non-image remains a permanent negative");
        ok = RealDownloadRetryRegression() && ok;
        return ok;
    }
    static bool PsdProgressiveRegression(const char* fixture) {
        bool ok = true;
        const auto check = [&](bool value, const char* name) {
            std::printf("[%s] %s\n", value ? "PASS" : "FAIL", name); std::fflush(stdout); ok &= value;
        };
        const auto image = [](uint32_t edge) {
            ThumbnailCache::Item item;
            item.kind = ipc::PreviewContentKind::Bitmap;
            item.w = edge; item.h = edge * 3 / 4; item.stride = edge * 4;
            item.source_width = 11648; item.source_height = 8736;
            item.pixels.assign(static_cast<size_t>(item.stride) * item.h, 255);
            item.cost = item.pixels.size();
            return item;
        };
        {
            ThumbnailCache cache;
            ThumbnailCache::Request req;
            req.id = 1; req.key = L"psd"; req.path = L"test.psd"; req.identity = L"psd";
            req.epoch = cache.epoch_.load(); req.pixels = 512;
            auto first = image(160); first.needs_psd_refine = true;
            check(cache.StoreResult(req, std::move(first)), "PSD first image is retained before refinement");
            cache.running_ = true; // Exercise scheduling without creating a worker.
            cache.lane_count_ = 4;
            cache.QueuePsdRefinement(req); cache.QueuePsdRefinement(req);
            check(cache.queue_.size() == 1 && cache.pending_.contains(req.key), "PSD refinement is queued once while pending");
            auto ordinary = req; ordinary.id = 2; ordinary.key = L"ordinary";
            cache.queue_.push_back(ordinary);
            ThumbnailCache::Request selected; bool slow = false;
            check(cache.TakeRequestLocked(selected, slow) && selected.id == 2,
                "ordinary first images precede queued PSD refinement");
            check(cache.TakeRequestLocked(selected, slow) && slow && !selected.urgent &&
                (selected.flags & ipc::kPreviewRequestFlagPsdRefine), "PSD refinement occupies a slow slot without priority boost");
            auto second_refine = selected; second_refine.id = 3; second_refine.key = L"second-refine";
            cache.queue_.push_back(second_refine);
            ThumbnailCache::Request concurrent;
            check(!cache.TakeRequestLocked(concurrent, slow), "large PSD refinements cannot occupy all slow lanes concurrently");
            auto first_psd = ordinary; first_psd.id = 4; first_psd.timeout_ms = 8000;
            cache.queue_.push_back(first_psd);
            check(cache.TakeRequestLocked(concurrent, slow) && concurrent.id == 4,
                "incoming PSD first pass can use another slow lane while refinement runs");
            cache.queue_.clear();
            ThumbnailCache::Item failure; failure.failed = true; failure.transient = true;
            check(!cache.StoreResult(selected, std::move(failure)) && cache.items_.at(req.key).w == 160 &&
                !cache.pending_.contains(req.key) && !cache.items_.at(req.key).needs_psd_refine,
                "failed PSD refinement preserves first image without automatic retry");
            check(cache.StoreResult(selected, image(512)) && cache.items_.at(req.key).w == 512,
                "successful PSD refinement replaces low resolution image");
            selected.interest_tracked = true;
            cache.items_.at(req.key).needs_psd_refine = true;
            check(!cache.StoreResult(selected, image(256)) && cache.items_.at(req.key).w == 512 &&
                cache.items_.at(req.key).needs_psd_refine,
                "offscreen refinement cannot overwrite displayed result");
            cache.QueuePsdRefinement(selected);
            check(cache.queue_.empty(), "offscreen PSD refinement is not queued");
            cache.Evict(); cache.pending_.insert(req.key);
            check(!cache.StoreResult(req, image(160)) && cache.pending_.contains(req.key),
                "stale PSD completion cannot clear a replacement generation");
            cache.running_ = false;
        }
        {
            ComPtr<ID3D11Device> d3d;
            HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
            ComPtr<IDXGIDevice> dxgi;
            if (SUCCEEDED(hr)) hr = d3d->QueryInterface(IID_PPV_ARGS(&dxgi));
            ComPtr<ID2D1Device> device;
            if (SUCCEEDED(hr)) hr = D2D1CreateDevice(dxgi.get(), nullptr, &device);
            ComPtr<ID2D1DeviceContext> dc;
            if (SUCCEEDED(hr)) hr = device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc);
            check(SUCCEEDED(hr), "create real graphics context for PSD return-to-view test");
            if (SUCCEEDED(hr)) {
                ThumbnailCache cache;
                cache.SetDeviceContext(dc.get());
                ThumbnailCache::Request req;
                req.path = L"return.psd"; req.pixels = 512;
                req.key = cache.Key(req.path, 512, 0, 0); req.identity = cache.Key(req.path, 0, 0, 0);
                req.epoch = cache.epoch_.load();
                auto first = image(160); first.needs_psd_refine = true;
                cache.StoreResult(req, std::move(first));
                cache.running_ = true; // Keep this a deterministic paint/scheduling check.
                cache.DrawGridThumbnail(dc.get(), D2D1::RectF(0, 0, 512, 384), req.path, 0,
                    512, 0, 0, 0, 1.0f, false, nullptr);
                check(cache.queue_.size() == 1 && (cache.queue_.front().flags & ipc::kPreviewRequestFlagPsdRefine),
                    "returning to cached PSD queues its cancelled refinement from production draw path");
                cache.queue_.clear(); cache.pending_.clear();
                cache.StoreResult(req, image(512));
                cache.DrawGridThumbnail(dc.get(), D2D1::RectF(0, 0, 1024, 768), req.path, 0,
                    1024, 0, 0, 0, 1.0f, false, nullptr);
                check(cache.queue_.size() == 1 && cache.queue_.front().pixels == 1024 &&
                    cache.queue_.front().key != req.key,
                    "enlarging PSD requests a new 1024px decode from production draw path");
                cache.running_ = false;
            }
        }
        if (!fixture) return ok;
        struct Store final : ThumbnailStore {
            std::atomic<unsigned> loads{0}, saves{0}, saved_width{0};
            std::atomic<bool> forced{false};
            bool legacy = true;
            bool hot = false;
            StoredThumbnail saved;
            bool Load(const ThumbnailStoreKey&, StoredThumbnail& out) override {
                ++loads;
                if (hot) { out = saved; return true; }
                if (!legacy) return false;
                out.width = 160; out.height = 120;
                out.source_width = 11648; out.source_height = 8736;
                out.pixels.assign(160 * 120 * 4, 255); return true;
            }
            void Save(const ThumbnailStoreKey& key, uint32_t, const StoredThumbnail& out) override {
                saved = out;
                forced = key.keep_always; saved_width = out.width; ++saves;
            }
        } store;
        const auto path = std::filesystem::absolute(std::filesystem::path(fixture));
        check(std::filesystem::is_regular_file(path), "large PSD fixture exists for real worker test");
        if (!std::filesystem::is_regular_file(path)) return false;
        ThumbnailCache::SetGridStore(&store);
        for (const bool legacy : {true, false}) {
            store.legacy = legacy; store.loads = 0; store.saves = 0; store.saved_width = 0; store.forced = false;
            ThumbnailCache cache;
            cache.SetParallelism(1);
            ThumbnailCache::Request req;
            req.id = 1; req.path = path.wstring(); req.key = L"old-psd-cache"; req.identity = req.path;
            req.epoch = cache.epoch_.load(); req.pixels = 512; req.timeout_ms = 8000;
            req.size = std::filesystem::file_size(path);
            {
                std::lock_guard lock(cache.mutex_);
                cache.pending_.insert(req.key); cache.queue_.push_back(req); cache.StartWorkersLocked();
            }
            cache.cv_.notify_one();
            const auto deadline = GetTickCount64() + 15000;
            bool saw_first = false;
            while (!store.saves && GetTickCount64() < deadline) {
                { std::lock_guard lock(cache.mutex_);
                  const auto it = cache.items_.find(req.key);
                  saw_first |= it != cache.items_.end() && it->second.w == 160; }
                Sleep(1);
            }
            check(saw_first, legacy ? "legacy low resolution image is visible during upgrade" :
                "cold real-host PSD publishes embedded first image before composite");
            check(store.loads == 1 && store.saves == 1 && store.saved_width == 512 && store.forced,
                legacy ? "legacy 160px disk entry upgrades through real host and forces 512px persistence" :
                "cold PSD upgrades through real host without persisting embedded JPEG");
            {
                std::lock_guard lock(cache.mutex_);
                const auto it = cache.items_.find(req.key);
                check(it != cache.items_.end() && it->second.w == 512 && !it->second.failed &&
                    it->second.pixels.size() >= 4 && it->second.pixels[2] == 255 && it->second.pixels[1] == 0,
                    "real worker publishes actual composite pixels rather than enlarging embedded JPEG");
            }
            cache.Reset();
        }
        store.hot = true;
        for (const uint32_t edge : {512u, 1024u}) {
            store.loads = 0; store.saves = 0;
            ThumbnailCache cache;
            cache.SetParallelism(1);
            ThumbnailCache::Request req;
            req.id = 1; req.path = path.wstring(); req.key = L"hot-psd"; req.identity = req.path;
            req.epoch = cache.epoch_.load(); req.pixels = edge; req.timeout_ms = 8000;
            req.size = std::filesystem::file_size(path);
            {
                std::lock_guard lock(cache.mutex_);
                cache.pending_.insert(req.key); cache.queue_.push_back(req); cache.StartWorkersLocked();
            }
            cache.cv_.notify_one();
            const auto deadline = GetTickCount64() + 15000;
            bool complete = false;
            while (!complete && GetTickCount64() < deadline) {
                {
                    std::lock_guard lock(cache.mutex_);
                    const auto it = cache.items_.find(req.key);
                    complete = it != cache.items_.end() && it->second.w == edge &&
                        !it->second.needs_psd_refine && !cache.pending_.contains(req.key);
                }
                if (!complete) Sleep(1);
            }
            check(complete && store.loads == 1 && (edge == 512 ? store.saves == 0 :
                store.saves == 1 && store.saved_width == 1024), edge == 512 ?
                "hot PSD returns refined 512px cached pixels without another decode" :
                "zoomed PSD upgrades cached 512px image to real 1024px composite");
            cache.Reset();
        }
        ThumbnailCache::SetGridStore(nullptr);
        return ok;
    }

    static bool InterestRegression() {
        ThumbnailCache cache;
        const HWND a = reinterpret_cast<HWND>(1), b = reinterpret_cast<HWND>(2);
        cache.AdvanceFrame(a); cache.interests_[a].drawing.insert(L"shared"); cache.EndInterestFrame();
        cache.AdvanceFrame(b); cache.interests_[b].drawing.insert(L"shared"); cache.EndInterestFrame();
        cache.AdvanceFrame(a); cache.EndInterestFrame();
        bool ok = cache.WantedByWindowLocked(L"shared");
        cache.AdvanceFrame(b);
        ok &= cache.WantedByWindowLocked(L"shared");
        cache.EndInterestFrame();
        ok &= !cache.WantedByWindowLocked(L"shared");
        ThumbnailCache::Request req; req.key = L"shared"; req.interest_tracked = true;
        req.epoch = cache.epoch_.load(); cache.pending_.insert(req.key);
        ok &= !cache.FolderRequestCurrent(req) && !cache.pending_.contains(req.key);
        cache.pending_.insert(req.key);
        ok &= !cache.StoreResult(req, {}) && !cache.pending_.contains(req.key);
        cache.AdvanceFrame(a); cache.interests_[a].drawing.insert(L"shared"); cache.EndInterestFrame();
        cache.queue_.push_back(req); cache.pending_.insert(req.key);
        cache.RemoveNotifyWindow(a);
        ok &= !cache.WantedByWindowLocked(L"shared") && cache.queue_.empty() && cache.pending_.empty();
        cache.AdvanceFrame(a); cache.EndInterestFrame();
        ok &= !cache.WantedByWindowLocked(L"shared");
        const auto old = req;
        cache.Evict(); req.epoch = cache.epoch_.load(); cache.pending_.insert(req.key);
        ok &= !cache.FolderRequestCurrent(old) && cache.pending_.contains(req.key);
        ok &= !cache.StoreResult(old, {}) && cache.pending_.contains(req.key);
        cache.Reset(); ok &= cache.interests_.empty();
        std::printf("[%s] multi-window interests survive other paints and reject unneeded completion\n", ok ? "PASS" : "FAIL");
        return ok;
    }

    static bool IdleRegression(bool baseline) {
        bool ok = true;
        const auto check = [&](bool value, const char* name) {
            std::printf("[%s] %s\n", value ? "PASS" : "FAIL", name); std::fflush(stdout); ok &= value;
        };
        struct Memory { size_t children = 0, commit = 0, private_ws = 0; bool valid = true; };
        const auto sample = [&](const char* stage) {
            Memory total;
            HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            PROCESSENTRY32W entry{sizeof(entry)};
            if (snapshot == INVALID_HANDLE_VALUE) { total.valid = false; return total; }
            if (Process32FirstW(snapshot, &entry)) do {
                if (entry.th32ParentProcessID != GetCurrentProcessId() || _wcsicmp(entry.szExeFile, L"Pulse.Preview.exe")) continue;
                HANDLE child = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, entry.th32ProcessID);
                if (!child) { total.valid = false; continue; }
                ++total.children;
                PROCESS_MEMORY_COUNTERS_EX counters{sizeof(counters)};
                total.valid &= GetProcessMemoryInfo(child, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)) != FALSE;
                total.commit += counters.PrivateUsage;
                std::vector<unsigned char> buffer(4 * 1024 * 1024);
                const bool got = QueryWorkingSet(child, buffer.data(), static_cast<DWORD>(buffer.size())) != FALSE;
                total.valid &= got;
                if (got) {
                    const auto* ws = reinterpret_cast<const PSAPI_WORKING_SET_INFORMATION*>(buffer.data());
                    SYSTEM_INFO system{}; GetSystemInfo(&system);
                    for (ULONG_PTR i = 0; i < ws->NumberOfEntries; ++i)
                        if (!ws->WorkingSetInfo[i].Shared) total.private_ws += system.dwPageSize;
                }
                CloseHandle(child);
            } while (Process32NextW(snapshot, &entry));
            CloseHandle(snapshot);
            std::printf("[MEMORY] %s children=%zu private_commit=%zu private_ws=%zu valid=%d\n",
                stage, total.children, total.commit, total.private_ws, total.valid); std::fflush(stdout);
            return total;
        };
        const auto path = std::filesystem::temp_directory_path() /
            (L"pulse-idle-probe-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L".bmp");
        BITMAPFILEHEADER file{}; BITMAPINFOHEADER bitmap{};
        bitmap.biSize = sizeof(bitmap); bitmap.biWidth = 1024; bitmap.biHeight = 1024;
        bitmap.biPlanes = 1; bitmap.biBitCount = 24; bitmap.biSizeImage = 1024 * 1024 * 3;
        file.bfType = 0x4d42; file.bfOffBits = sizeof(file) + sizeof(bitmap); file.bfSize = file.bfOffBits + bitmap.biSizeImage;
        {
            std::ofstream stream(path, std::ios::binary); std::vector<char> pixels(bitmap.biSizeImage, 100);
            stream.write(reinterpret_cast<const char*>(&file), sizeof(file));
            stream.write(reinterpret_cast<const char*>(&bitmap), sizeof(bitmap)); stream.write(pixels.data(), pixels.size());
            check(stream.good(), "create isolated real BMP fixture");
        }
        ThumbnailCache cache;
        cache.SetParallelism(4); cache.Warm();
        const auto enqueue = [&](int index) {
            std::lock_guard lock(cache.mutex_);
            ThumbnailCache::Request req; req.id = index + 1; req.epoch = cache.epoch_.load();
            req.path = path.wstring(); req.key = L"idle-probe-" + std::to_wstring(index);
            req.pixels = 128 + index; req.timeout_ms = 2500;
            cache.pending_.insert(req.key); cache.queue_.push_back(std::move(req)); cache.cv_.notify_all();
        };
        const auto await_items = [&](size_t count) {
            const auto until = GetTickCount64() + 10000;
            while (GetTickCount64() < until) {
                { std::lock_guard lock(cache.mutex_); if (cache.items_.size() >= count) {
                    for (const auto& [key, item] : cache.items_) if (item.failed || item.pixels.empty()) return false;
                    return true;
                } }
                Sleep(10);
            }
            return false;
        };
        for (int i = 0; i < 4; ++i) enqueue(i);
        check(await_items(4), "four real thumbnail decodes succeed");
        Sleep(1000);
        const auto active = sample("loaded"); check(active.valid && active.children == 4, "four warmed decode hosts measured");
        const auto first = GetTickCount64(); enqueue(4); check(await_items(5), "warm single thumbnail succeeds");
        std::printf("[TIME] warm_single_ms=%llu\n", static_cast<unsigned long long>(GetTickCount64() - first));
        Sleep(33000);
        const auto idle = sample("idle-33s");
        check(idle.valid && idle.children == (baseline ? 4u : 1u), "idle host count matches policy");
        { std::lock_guard lock(cache.mutex_); check(cache.items_.size() == 5 && cache.pending_.empty(), "idle keeps ready thumbnails and clears no live work"); }
        const auto resume = GetTickCount64(); enqueue(5); check(await_items(6), "first thumbnail after idle succeeds");
        std::printf("[TIME] resumed_single_ms=%llu\n", static_cast<unsigned long long>(GetTickCount64() - resume));
        const auto resumed = sample("single-resumed");
        check(resumed.children == idle.children, "single resume uses warm host without spawning spares");
        for (int i = 6; i < 22; ++i) enqueue(i);
        check(await_items(22), "burst after idle completes all thumbnails");
        const auto burst = sample("burst-resumed");
        check(burst.valid && burst.children > 1 && burst.children <= 4,
              "burst restores spare decode hosts within concurrency limit");
        cache.Reset();
        check(sample("reset").children == 0, "reset cleans up all owned hosts");
        std::error_code error; std::filesystem::remove(path, error); check(!error, "isolated fixture cleanup");
        return ok;
    }

    static bool RetryRegression() {
        bool ok = true;
        const auto check = [&](bool value, const char* name) { std::printf("[%s] %s\n", value ? "PASS" : "FAIL", name); ok &= value; };
        using Kind = ipc::PreviewRequestKind;
        check(ThumbnailCache::ResponseTimeoutMs(L"small.png", Kind::Content, (32ull << 20) - 1) == 2500 &&
            ThumbnailCache::ResponseTimeoutMs(L"large.jpg", Kind::Content, 180ull << 20) == 2500 &&
            ThumbnailCache::ResponseTimeoutMs(L"large.png", Kind::Properties, 180ull << 20) == 4000,
            "large PNG allowance preserves small images other codecs and properties deadlines");
        check(ThumbnailCache::ResponseTimeoutMs(L"large.PNG", Kind::Content, 90ull << 20) == 10000 &&
            ThumbnailCache::ResponseTimeoutMs(L"large.png", Kind::Content, 180ull << 20) == 15000 &&
            ThumbnailCache::ResponseTimeoutMs(L"huge.png", Kind::Content, UINT64_MAX) == 30000,
            "snapshot PNG size selects bounded 10 to 30 second deadline without file IO");
        WNDCLASSW wc{}; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"PulseThumbnailRetryRegression";
        wc.lpfnWndProc = [](HWND window, UINT message, WPARAM wp, LPARAM lp) -> LRESULT {
            if (message == WM_TIMER && HandleThumbnailRetryTimer(window, wp)) {
                auto* count = reinterpret_cast<int*>(GetWindowLongPtrW(window, GWLP_USERDATA));
                if (count) ++*count;
                return 0;
            }
            return DefWindowProcW(window, message, wp, lp);
        };
        if (!RegisterClassW(&wc)) return false;
        HWND a = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        HWND b = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        int fired_a = 0, fired_b = 0;
        SetWindowLongPtrW(a, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&fired_a));
        SetWindowLongPtrW(b, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&fired_b));
        const auto pump = [&](uint64_t duration) {
            const auto end = GetTickCount64() + duration;
            do { MSG msg{}; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
                 Sleep(1); } while (GetTickCount64() < end);
        };
        ThumbnailCache cache;
        auto& first = cache.items_[L"a"]; first.failed = first.transient = true; first.retry_at = GetTickCount64() + 20;
        auto& second = cache.items_[L"b"]; second.failed = second.transient = true; second.retry_at = first.retry_at + 1000;
        cache.interests_[a].committed.insert(L"a"); cache.interests_[b].committed.insert(L"b");
        cache.interests_[a].visible.insert(L"a"); cache.interests_[b].visible.insert(L"b");
        check(cache.WindowRetryDeadline(a) == first.retry_at && cache.WindowRetryDeadline(b) == second.retry_at,
            "each window sees only its own subscribed retry deadline");
        ArmThumbnailRetryTimer(a, cache.WindowRetryDeadline(a));
        const auto until = GetTickCount64() + 2000;
        while (!fired_a && GetTickCount64() < until) pump(10);
        pump(80);
        check(fired_a == 1 && fired_b == 0, "native retry timer wakes its own window once without continuous frames");
        cache.pending_.insert(L"a");
        check(cache.WindowRetryDeadline(a) == 0, "in-flight retry removes its scheduled wakeup");
        cache.pending_.erase(L"a");
        cache.interests_[a].visible.clear();
        check(cache.WindowRetryDeadline(a) == 0, "prefetch-only failed entries do not schedule repeated paint retries");
        cache.AdvanceFrame(a); cache.EndInterestFrame();
        ArmThumbnailRetryTimer(a, cache.WindowRetryDeadline(a));
        ArmThumbnailRetryTimer(b, GetTickCount64() + 20);
        cache.RemoveNotifyWindow(b); ArmThumbnailRetryTimer(b, cache.WindowRetryDeadline(b));
        check(!HandleThumbnailRetryTimer(b, kThumbnailRetryTimer), "cancelled timer ignores an already queued stale message");
        pump(80);
        check(cache.WindowRetryDeadline(a) == 0 && fired_a == 1 && fired_b == 0,
            "removed viewport or window cancels retry without stale wakeups");
        ThumbnailCache::Request slow; slow.key = L"large"; slow.path = L"large.png";
        slow.timeout_ms = ThumbnailCache::ResponseTimeoutMs(slow.path, Kind::Content, 180ull << 20);
        slow.interest_tracked = true; slow.epoch = cache.epoch_.load();
        cache.interests_[a].committed.insert(slow.key); cache.pending_.insert(slow.key);
        check(cache.FolderRequestCurrent(slow), "large PNG is initially subscribed");
        cache.AdvanceFrame(a); cache.EndInterestFrame();
        check(!cache.FolderRequestCurrent(slow) && !cache.pending_.contains(slow.key),
            "long PNG allowance does not delay viewport cancellation");
        DestroyWindow(a); DestroyWindow(b); UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return ok;
    }

    static bool QueueRegression() {
        bool ok = true;
        const auto check = [&](bool value, const char* name) {
            std::printf("[%s] %s\n", value ? "PASS" : "FAIL", name);
            ok &= value;
        };
        ThumbnailCache cache;
        cache.SetParallelism(4);
        const auto add = [&](const std::wstring& key, uint64_t frame, uint64_t order, bool details = false) {
            ThumbnailCache::Request request;
            request.key = key; request.path = key; request.details = details;
            cache.queue_.push_back(request);
            cache.pending_.insert(key);
            cache.wanted_[key] = {frame, order};
        };
        for (int i = 0; i < 120; ++i) add(L"old" + std::to_wstring(i), 1, i);
        cache.AdvanceFrame();
        check(cache.queue_.size() == 120, "one-frame grace preserves quick reverse scrolling");
        cache.AdvanceFrame();
        // Refresh visible requests as Draw does, including both panes.
        for (int i = 0; i < 8; ++i) cache.wanted_[L"old" + std::to_wstring(i)] = {3, i};
        cache.AdvanceFrame();
        const auto retained = cache.queue_.size();
        std::printf("[MEASURE] scroll backlog before=120 retained=%zu dropped=%zu\n", retained, 120 - retained);
        check(retained == 8, "offscreen backlog retires without dropping current pane requests");
        check(cache.pending_.size() == retained && cache.wanted_.size() == retained,
              "retired requests release pending keys and ordering metadata");
        check(!cache.pending_.contains(L"old119"), "returning to retired file can request immediately");
        const std::wstring ahead_path = L"C:\\isolated\\ahead.png";
        const auto ahead_key = cache.Key(ahead_path, 128, 1, 1);
        add(ahead_key, 1, 1);
        cache.Prefetch(ahead_path, 0, 128, 1, 1, 1);
        check(cache.wanted_.at(ahead_key).first == 0, "offscreen item entering prefetch range stays wanted");
        const auto visible_key = cache.Key(L"visible.png", 128, 1, 1);
        add(visible_key, cache.frame_, 1);
        cache.Prefetch(L"visible.png", 0, 128, 1, 1, 1);
        check(cache.wanted_.at(visible_key).first == cache.frame_, "other pane prefetch cannot demote visible work");
        add(L"prefetch", 0, 1);
        add(L"details", 1, 0, true);
        cache.AdvanceFrame();
        check(cache.pending_.contains(L"prefetch") && cache.pending_.contains(L"details"),
              "next-screen prefetch and explicit preview remain queued");
        ThumbnailCache::Request out;
        bool slow = false;
        check(cache.TakeRequestLocked(out, slow) && out.key == L"details", "explicit preview retains priority");
        // A running request has already left the queue; pruning must not clear its pending key.
        cache.pending_.insert(L"running");
        cache.wanted_[L"running"] = {1, 0};
        cache.AdvanceFrame();
        check(cache.pending_.contains(L"running"), "running decode remains deduplicated");
        ThumbnailCache single;
        ThumbnailCache::Request request; request.key = L"hover";
        single.queue_.push_back(request); single.pending_.insert(request.key);
        single.wanted_[request.key] = {1, 0};
        for (int i = 0; i < 5; ++i) single.AdvanceFrame();
        check(single.queue_.size() == 1, "non-grid queues keep their lifecycle");
        return ok;
    }

    static bool MemoryRegression() {
        bool ok = true;
        bool backoff_ok = true;
        for (bool byte_limit : {false, true}) {
            ThumbnailCache cache(byte_limit ? 32 : 1024 * 1024, 32);
            const auto store = [&](const std::wstring& key) {
                ThumbnailCache::Request req;
                req.key = key;
                req.epoch = cache.epoch_.load();
                ThumbnailCache::Item item;
                item.failed = item.transient = true;
                item.cost = 4;
                return cache.StoreResult(req, std::move(item));
            };
            std::wstring last;
            for (int i = 0; i < 20000; ++i) {
                last = std::wstring(200, L'x') + std::to_wstring(i);
                ok &= store(last);
            }
            size_t retained_key_bytes = 0;
            for (const auto& [key, attempts] : cache.transient_failures_)
                retained_key_bytes += (key.capacity() + 1) * sizeof(wchar_t);
            const bool bounded = cache.transient_failures_.size() == cache.items_.size() &&
                cache.items_.size() == (byte_limit ? 8u : 32u);
            std::printf("[%s] retry metadata %s limit: items=%zu retries=%zu retained_key_bytes=%zu\n",
                bounded ? "PASS" : "FAIL", byte_limit ? "byte" : "item",
                cache.items_.size(), cache.transient_failures_.size(), retained_key_bytes);
            ok &= bounded;
            backoff_ok &= store(last) && cache.transient_failures_.at(last) == 2;
            backoff_ok &= store(last) && cache.transient_failures_.at(last) == 3;
            cache.Evict();
            backoff_ok &= cache.transient_failures_.empty();
        }
        ThumbnailCache folders;
        ThumbnailCache::Request folder;
        folder.identity = L"folder";
        folder.key = L"folder\n128";
        folder.epoch = folders.epoch_.load();
        folder.flags = ipc::kPreviewRequestFlagFolderThumbnail;
        folder.id = 1;
        folders.folder_requests_[folder.key] = folder.id;
        ThumbnailCache::Item transient;
        transient.failed = transient.transient = true;
        folders.StoreResult(folder, std::move(transient));
        bool folder_ok = folders.transient_failures_.contains(folder.key);
        folder.key = L"folder\n256";
        folders.folder_requests_[folder.key] = folder.id;
        ThumbnailCache::Item unsupported;
        unsupported.failed = true;
        folders.StoreResult(folder, std::move(unsupported));
        folder_ok &= folders.items_.size() == 1 && folders.transient_failures_.empty();
        std::printf("[%s] authoritative folder result clears old-size retry metadata\n", folder_ok ? "PASS" : "FAIL");
        ok &= folder_ok;

        ThumbnailCache animation;
        animation.latest_details_identity_ = L"animation";
        for (int i = 0; i < 5; ++i) {
            ThumbnailCache::Request frame;
            frame.key = L"frame" + std::to_wstring(i);
            frame.identity = L"animation";
            frame.details = true;
            frame.epoch = animation.epoch_.load();
            ThumbnailCache::Item failed_frame;
            failed_frame.failed = failed_frame.transient = true;
            failed_frame.frame_count = 5;
            failed_frame.frame_delay_ms = 100;
            failed_frame.animation_identity = frame.identity;
            animation.StoreResult(frame, std::move(failed_frame));
        }
        const bool animation_ok = animation.items_.size() == 4 &&
            animation.transient_failures_.size() == 4 &&
            !animation.transient_failures_.contains(L"frame0");
        std::printf("[%s] animation frame eviction clears only retired retry metadata\n", animation_ok ? "PASS" : "FAIL");
        ok &= animation_ok;
        std::printf("[%s] live retry backoff survives replacement; eviction clears metadata\n", backoff_ok ? "PASS" : "FAIL");
        return ok && backoff_ok;
    }

    static bool VolumeReleaseRegression() {
        ThumbnailCache cache;
        ThumbnailCache::Request target, other;
        target.path = L"Q:\\target.png"; target.key = L"target";
        other.path = L"R:\\other.png"; other.key = L"other";
        cache.queue_.push_back(target); cache.queue_.push_back(other);
        cache.pending_.insert(target.key); cache.pending_.insert(other.key);
        auto ready = cache.SuspendVolume(L"q:\\");
        bool ok = ready.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready &&
            cache.queue_.size() == 1 && cache.queue_.front().path == other.path &&
            !cache.pending_.contains(target.key) && cache.pending_.contains(other.key) &&
            !cache.FolderRequestCurrent(target) && cache.FolderRequestCurrent(other);
        auto other_ready = cache.SuspendVolume(L"r:/");
        ok = ok && other_ready.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready &&
            !cache.FolderRequestCurrent(target) && !cache.FolderRequestCurrent(other);
        cache.ResumeVolume(L"q:\\");
        ok = ok && cache.FolderRequestCurrent(target);
        ok = ok && !cache.FolderRequestCurrent(other);
        cache.ResumeVolume(L"R:\\");
        ok = ok && cache.FolderRequestCurrent(other);
        cache.queue_.clear();
        cache.lane_paths_[0] = target.path;
        auto pending = cache.SuspendVolume(L"Q:\\");
        ok = ok && pending.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout;
        cache.running_ = true;
        cache.worker_ = std::thread([&cache] { cache.Worker(0); });
        ok = ok && pending.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
        cache.Reset();
        std::printf("[%s] volume pause isolates queued work and waits for worker release\n", ok ? "PASS" : "FAIL");
        return ok;
    }

    // A cut-out whose lower half is transparent: pictures rest the whole
    // bitmap on the grid baseline, icons rest their visible ink on it.
    static bool ArtworkCanvasRegression() {
        std::vector<uint8_t> px(4 * 4 * 4, 0);
        for (size_t i = 0; i < 8; ++i) px[i * 4 + 3] = 255;
        const auto place = [&](const wchar_t* path) {
            return ContainedThumbnailRect(D2D1::RectF(0, 0, 100, 100), 4, 4,
                ThumbnailArtworkBounds(path, px, 4, 4, 16), true);
        };
        const D2D1_RECT_F webp = place(L"C:\\cut\\Product.WEBP"), psd = place(L"art.psd");
        const D2D1_RECT_F icon = place(L"app.ico"), svg = place(L"logo.svg");
        const bool ok = webp.top == 0.0f && webp.bottom == 100.0f && psd.bottom == 100.0f &&
            icon.bottom == 150.0f && svg.bottom == 150.0f && IsPictureCanvasPath(L"raw.NEF") &&
            !IsPictureCanvasPath(L"dir.v2\\noext");
        std::printf("[%s] grid bottom alignment keeps pictures whole and measures icon ink\n",
                    ok ? "PASS" : "FAIL");
        return ok;
    }
    static bool DeadlineAndPageRegression() {
        ThumbnailCache cache;
        cache.running_ = true;
        const auto epoch = cache.epoch_.load();
        ThumbnailCache::Item item;
        item.failed = item.transient = true;
        item.retry_at = GetTickCount64() + 50;
        cache.items_[L"retry"] = std::move(item);
        cache.retry_interest_.insert(L"retry");
        const bool deadline = cache.NextRetryDeadline() > GetTickCount64();
        cache.items_[L"retry"].retry_at = GetTickCount64() - 1;
        const bool expired = cache.NextRetryDeadline() != 0;
        cache.BeginFrame();
        const bool inactive = cache.NextRetryDeadline() == 0;
        cache.retry_interest_.insert(L"retry");
        cache.pending_.insert(L"retry");
        const bool queued = cache.NextRetryDeadline() == 0;
        cache.Evict();
        const bool evicted = cache.NextRetryDeadline() == 0 && cache.epoch_.load() != epoch;
        cache.running_ = false;
        std::printf("[%s] preview retry deadline excludes pending work and capability eviction changes epoch\n", deadline && expired && inactive && queued && evicted ? "PASS" : "FAIL");
        return deadline && expired && inactive && queued && evicted;
    }
    static bool UploadRegression() {
        ComPtr<ID3D11Device> d3d;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
        ComPtr<IDXGIDevice> dxgi;
        if (SUCCEEDED(hr)) hr = d3d->QueryInterface(IID_PPV_ARGS(&dxgi));
        ComPtr<ID2D1Device> device;
        if (SUCCEEDED(hr)) hr = D2D1CreateDevice(dxgi.get(), nullptr, &device);
        ComPtr<ID2D1DeviceContext> dc;
        if (SUCCEEDED(hr)) hr = device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc);
        if (FAILED(hr)) return false;
        ThumbnailCache cache;
        cache.SetDeviceContext(dc.get());
        const auto key = cache.Key(L"upload.gif", 256, 1, 1, 0);
        cache.lru_.push_front(key);
        auto& item = cache.items_[key];
        item.lru_position = cache.lru_.begin();
        item.w = 2; item.h = 2; item.stride = 8; item.pixels.assign(16, 255);
        const auto rect = D2D1::RectF(0, 0, 32, 32);
        const auto window = reinterpret_cast<HWND>(1);
        cache.AdvanceFrame(window);
        cache.interests_[window].drawing.insert(key);
        cache.EndInterestFrame();
        ThumbnailCache::Request outgoing;
        outgoing.key = key; outgoing.interest_tracked = true;
        cache.queue_.push_back(outgoing); cache.pending_.insert(key);
        cache.AdvanceFrame(window);
        bool cached_only = cache.DrawCachedGridThumbnail(dc.get(), rect, L"upload.gif", 0,
            256, 1, 1, 1, true, nullptr) == PreviewDrawResult::Pending;
        cached_only &= !item.bitmap.get() && item.pixels.size() == 16 &&
            cache.interests_[window].drawing.empty() && !cache.TakeUploadFrameRequest();
        cache.EndInterestFrame();
        cached_only &= cache.queue_.empty() && cache.pending_.empty();
        cached_only &= cache.DrawCachedGridThumbnail(dc.get(), rect, L"missing.gif", 0,
            256, 1, 1, 1, true, nullptr) == PreviewDrawResult::Pending && cache.queue_.empty();
        cache.BeginFrame(); cache.upload_elapsed_us_ = 3000;
        cache.PrepareBitmap(key, true);
        bool ok = cached_only && !item.bitmap.get() && item.pixels.size() == 16 && cache.TakeRetryDeadline() != 0;
        cache.upload_probe_ = [](ThumbnailCache& target) {
            if (target.mutex_.try_lock()) target.mutex_.unlock();
            else std::abort();
        };
        cache.BeginFrame(); cache.PrepareBitmap(key, true);
        ok &= item.bitmap.get() && item.pixels.empty() && !item.upload_token;
        ok &= cache.DrawCachedGridThumbnail(dc.get(), rect, L"upload.gif", 0,
            256, 1, 1, 1, true, nullptr) == PreviewDrawResult::Bitmap;
        cache.still_by_identity_[cache.Key(L"upload.gif", 0, 1, 1)] = key;
        ok &= cache.DrawCachedGridThumbnail(dc.get(), rect, L"upload.gif", 0,
            128, 1, 1, 1, true, nullptr) == PreviewDrawResult::Bitmap && cache.queue_.empty();
        std::printf("[%s] outgoing thumbnails reuse uploaded sizes without decode interest or upload\n", ok ? "PASS" : "FAIL");
        item.bitmap.reset(); item.pixels.assign(16, 255); item.w = 0;
        cache.BeginFrame(); cache.PrepareBitmap(key, true);
        ok &= !item.bitmap.get() && item.pixels.size() == 16 && !item.upload_token;
        item.w = 2; cache.PrepareBitmap(key, false);
        ok &= item.bitmap.get() && item.pixels.empty();
        item.bitmap.reset(); item.pixels.assign(16, 255);
        cache.upload_probe_ = [](ThumbnailCache& target) { target.SetDeviceContext(nullptr); };
        cache.PrepareBitmap(key, false);
        ok &= !item.bitmap.get() && item.pixels.size() == 16 && !item.upload_token;
        cache.SetDeviceContext(dc.get());
        cache.upload_probe_ = [](ThumbnailCache& target) { target.Evict(); };
        cache.PrepareBitmap(key, false);
        ok &= cache.items_.empty();
        std::printf("[%s] upload budget defers CPU data and failed upload restores retryable ownership\n", ok ? "PASS" : "FAIL");
        return ok;
    }
    static bool SheetRegression() {
        ComPtr<ID3D11Device> d3d;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
        ComPtr<IDXGIDevice> dxgi;
        if (SUCCEEDED(hr)) hr = d3d->QueryInterface(IID_PPV_ARGS(&dxgi));
        ComPtr<ID2D1Device> device;
        if (SUCCEEDED(hr)) hr = D2D1CreateDevice(dxgi.get(), nullptr, &device);
        ComPtr<ID2D1DeviceContext> dc;
        if (SUCCEEDED(hr)) hr = device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc);
        if (FAILED(hr)) return false;
        ThumbnailCache cache;
        cache.SetDeviceContext(dc.get());
        cache.running_ = true;
        bool ok = true;
        auto check = [&](bool value, const char* label) {
            std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label);
            ok &= value;
        };
        std::wstring text;
        preview::Integrity integrity;
        const auto draw = [&](uint32_t sheet) {
            return cache.Draw(dc.get(), D2D1::RectF(0, 0, 100, 100), L"workbook.XLSX",
                0, 512, 1, 2, 3, 1, &text, nullptr, nullptr, true, nullptr,
                nullptr, nullptr, nullptr, nullptr, sheet, nullptr, nullptr, nullptr,
                nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false, nullptr, nullptr, &integrity);
        };
        draw(0);
        const auto old = cache.queue_.front();
        draw(16);
        draw(99);
        check(cache.queue_.size() == 1 && cache.queue_.front().frame_index == 99 &&
            cache.pending_.size() == 1, "rapid sheet switches keep only latest queued content request");
        const auto current = cache.queue_.front();
        cache.queue_.clear();
        ThumbnailCache::Item stale;
        stale.kind = ipc::PreviewContentKind::Table;
        stale.text = L"sheet0";
        cache.StoreResult(old, std::move(stale));
        check(draw(99) == PreviewDrawResult::Pending && text.empty(), "old sheet response cannot satisfy current sheet request");
        ThumbnailCache::Item ready;
        ready.kind = ipc::PreviewContentKind::Table;
        ready.text = L"sheet99";
        ready.integrity.state = preview::IntegrityState::Partial;
        ready.integrity.reason = preview::IntegrityReason::OnDemand;
        ready.integrity.loaded = 1;
        ready.integrity.total = 100;
        cache.StoreResult(current, std::move(ready));
        check(draw(99) == PreviewDrawResult::Table && text == L"sheet99" &&
            integrity.reason == preview::IntegrityReason::OnDemand && integrity.total == 100,
            "cached sheet response retains structured integrity and selected content");
        cache.Evict();
        ThumbnailCache::Item first_page;
        const uint32_t pixel = 0xffffffffu;
        const auto props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        dc->CreateBitmap(D2D1::SizeU(1,1), &pixel, 4, props, &first_page.bitmap);
        first_page.w = first_page.h = 1;
        first_page.kind = ipc::PreviewContentKind::Bitmap;
        const auto first_key = cache.Key(L"pages.tiff", 128, 2, 3, 0);
        cache.lru_.push_front(first_key);
        first_page.lru_position = cache.lru_.begin();
        cache.items_[first_key] = std::move(first_page);
        cache.still_by_identity_[cache.Key(L"pages.tiff", 0, 2, 3)] = first_key;
        ThumbnailCache::Item failed_page;
        failed_page.failed = true;
        const auto failed_key = cache.Key(L"pages.tiff", 512, 2, 3, 1);
        cache.lru_.push_front(failed_key);
        failed_page.lru_position = cache.lru_.begin();
        cache.items_[failed_key] = std::move(failed_page);
        const auto failed = cache.Draw(dc.get(), D2D1::RectF(0,0,100,100), L"pages.tiff", 0,512,1,2,3,
            1,nullptr,nullptr,nullptr,true,nullptr,nullptr,nullptr,nullptr,nullptr,1);
        check(failed == PreviewDrawResult::Failed, "failed nonfirst page cannot borrow the cached first-page bitmap");
        cache.running_ = false;
        return ok;
    }
    static bool PropertiesRegression() {
        ThumbnailCache cache;
        // Inspect queued requests deterministically without launching a worker.
        cache.running_ = true;
        bool ok = true;
        auto check = [&](bool result, const char* name) {
            std::printf("[%s] %s\n", result ? "PASS" : "FAIL", name);
            ok &= result;
        };
        std::vector<PreviewProperty> properties;
        cache.Properties(L"preview-a.docx", 0, 1, 2, 3, properties);
        check(cache.queue_.size() == 1, "properties queue without any cover drawing");
        const auto old = cache.queue_.front();
        cache.Properties(L"preview-b.mp3", 0, 2, 3, 4, properties);
        check(cache.queue_.size() == 1 && cache.queue_.front().path == L"preview-b.mp3",
              "selection switch cancels old queued properties");
        check(!cache.StoreResult(old, {}), "late properties cannot replace new selection");
        const auto req = cache.queue_.front();
        cache.queue_.clear();
        ThumbnailCache::Item failed;
        failed.failed = failed.transient = true;
        cache.StoreResult(req, std::move(failed));
        cache.Properties(req.path, 0, 2, 3, 4, properties);
        check(cache.queue_.empty(), "transient failure waits for retry deadline");
        cache.items_.at(req.key).retry_at = 0;
        cache.Properties(req.path, 0, 2, 3, 4, properties);
        cache.Properties(req.path, 0, 2, 3, 4, properties);
        check(cache.queue_.size() == 1, "expired properties failure retries once");
        ThumbnailCache::Item good;
        good.properties.push_back({L"Title", L"correct selection"});
        cache.queue_.clear();
        cache.StoreResult(req, std::move(good));
        check(cache.Properties(req.path, 0, 2, 3, 4, properties) && properties.size() == 1,
              "successful retry returns current properties");
        cache.running_ = false;
        return ok;
    }
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
            req.details = true;
            cache.latest_details_identity_ = req.identity;
            ThumbnailCache::Item item;
            item.kind = ipc::PreviewContentKind::Bitmap;
            req.frame_index = static_cast<uint32_t>(i);
            item.frame_count = 10;
            item.frame_delay_ms = 100;
            item.frame_index = static_cast<uint32_t>(i);
            item.animation_identity = req.identity;
            item.cost = 1;
            if (i == 4) cache.Touch(cache.items_.at(L"0"));
            cache.StoreResult(req, std::move(item));
        }
        check(cache.items_.size() == 4 && cache.items_.contains(L"0") &&
              !cache.items_.contains(L"1") && cache.cache_bytes_ == 4,
              "animation keeps four most recently used frames");
        check(!cache.still_by_identity_.contains(L"animation"),
              "direct animation is not reused as a static grid fallback");
        cache.Evict();
        for (int i = 0; i < 5; ++i) {
            auto req = request(std::to_wstring(i));
            req.identity = L"paged document";
            ThumbnailCache::Item item;
            item.frame_count = 10;
            item.frame_delay_ms = 0;
            item.frame_index = static_cast<uint32_t>(i);
            item.animation_identity = req.identity;
            item.cost = 1;
            cache.StoreResult(req, std::move(item));
        }
        check(cache.items_.size() == 5 && cache.items_.contains(L"0") &&
              cache.items_.contains(L"4") && cache.cache_bytes_ == 5,
              "zero-delay document pages are not limited to four animation frames");
        cache.Reset();
        check(cache.items_.empty() && cache.lru_.empty() && cache.cache_bytes_ == 0,
              "reset clears all cache state");

        {
            // Grid budget: a screenful of 256 px thumbnails must fit without
            // the view thrashing between thumbnails and icons.
            ThumbnailCache grid(96ull * 1024ull * 1024ull, 1024);
            for (int i = 0; i < 300; ++i) {
                ThumbnailCache::Request req;
                req.key = L"g" + std::to_wstring(i);
                req.epoch = grid.epoch_.load();
                ThumbnailCache::Item item;
                item.cost = 256u * 256u * 4u;
                grid.StoreResult(req, std::move(item));
            }
            check(grid.items_.size() == 300 && grid.items_.contains(L"g0"),
                  "grid budget keeps 300 thumbnails of 256 px");
        }
        {
            cache.Evict();
            auto req = request(L"slow");
            bool retried = true;
            for (int attempt = 0; attempt < 3; ++attempt) {
                ThumbnailCache::Item item;
                item.failed = item.transient = true;
                const ULONGLONG before = GetTickCount64();
                cache.StoreResult(req, std::move(item));
                const auto& stored = cache.items_.at(L"slow");
                retried &= stored.transient && stored.retry_at > before;
            }
            check(retried, "host timeout is remembered as retryable, not as no preview");
            ThumbnailCache::Item last;
            last.failed = last.transient = true;
            cache.StoreResult(req, std::move(last));
            check(!cache.items_.at(L"slow").transient && !cache.transient_failures_.contains(L"slow"),
                  "fourth consecutive timeout settles as a failure");
            for (const auto* budget : {L"gif-thumbnail-budget", L"gif-memory-budget"}) {
                for (int attempt = 0; attempt < 5; ++attempt) {
                    ThumbnailCache::Item limited;
                    limited.failed = true;
                    limited.error = budget;
                    cache.StoreResult(req, std::move(limited));
                }
                check(cache.items_.at(L"slow").transient &&
                      !cache.transient_failures_.contains(L"slow") &&
                      cache.items_.at(L"slow").retry_at > GetTickCount64(),
                      "GIF resource budget stays retryable without exhausting transport retries");
            }
            ThumbnailCache::Item good;
            good.kind = ipc::PreviewContentKind::Bitmap;
            good.cost = 1;
            auto ok_req = request(L"slow-ok");
            ok_req.identity = L"slow-identity";
            cache.StoreResult(ok_req, std::move(good));
            check(cache.still_by_identity_.contains(L"slow-identity") &&
                  cache.still_by_identity_.at(L"slow-identity") == L"slow-ok",
                  "decoded still bitmap is remembered for other sizes");
            cache.Evict();
            check(cache.still_by_identity_.empty() && cache.transient_failures_.empty(),
                  "evict clears retry and stale bookkeeping");
        }

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
                for (bool animated : {false, true}) {
                    // A different size of an already decoded file draws the
                    // old bitmap instead of falling back to the icon.
                    cache.Evict();
                    const std::wstring identity = cache.Key(L"stale", 0, 5, 6);
                    ThumbnailCache::Item item;
                    item.w = item.h = 64;
                    item.stride = 64 * 4;
                    item.pixels.assign(static_cast<size_t>(item.stride) * item.h, 255);
                    item.cost = item.pixels.size();
                    item.kind = ipc::PreviewContentKind::Bitmap;
                    auto req = request(cache.Key(L"stale", 128, 5, 6));
                    req.identity = identity;
                    item.frame_count = animated ? 2 : 1;
                    item.frame_delay_ms = animated ? 100 : 0;
                    cache.StoreResult(req, std::move(item));
                    context->BeginDraw();
                    cache.running_ = true; // Inspect the queued resize without launching a helper.
                    const auto result = animated
                        ? cache.DrawGridThumbnail(context.get(), D2D1::RectF(0, 0, 100, 100),
                            L"stale", 0, 256, 0, 5, 6, 1.0f, false, nullptr)
                        : cache.Draw(context.get(), D2D1::RectF(0, 0, 100, 100),
                            L"stale", 0, 256, 0, 5, 6);
                    cache.running_ = false;
                    context->EndDraw();
                    bool queued = false;
                    {
                        std::lock_guard lock(cache.mutex_);
                        const std::wstring wanted = cache.Key(L"stale", 256, 5, 6);
                        queued = cache.pending_.contains(wanted) || cache.items_.contains(wanted);
                    }
                    check(result == PreviewDrawResult::Bitmap && queued,
                          animated ? "AUD-024: grid animated first frame survives new-size decode (2 frames, 100ms)"
                                   : "new size shows previous bitmap while it decodes");
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

bool RunThumbnailPropertiesRegression() {
    return pulse::ui::ThumbnailCacheTestAccess::PropertiesRegression();
}

bool RunThumbnailSheetRegression() {
    return pulse::ui::ThumbnailCacheTestAccess::SheetRegression();
}

bool RunThumbnailDeadlineRegression() {
    const bool deadline = pulse::ui::ThumbnailCacheTestAccess::DeadlineAndPageRegression();
    const bool artwork = pulse::ui::ThumbnailCacheTestAccess::ArtworkCanvasRegression();
    return deadline && artwork;
}

bool RunThumbnailVolumeRegression() { return pulse::ui::ThumbnailCacheTestAccess::VolumeReleaseRegression(); }

bool RunThumbnailMemoryRegression() { return pulse::ui::ThumbnailCacheTestAccess::MemoryRegression(); }

bool RunThumbnailQueueRegression() { return pulse::ui::ThumbnailCacheTestAccess::QueueRegression(); }

bool RunThumbnailIdleRegression(bool baseline) { return pulse::ui::ThumbnailCacheTestAccess::IdleRegression(baseline); }

bool RunThumbnailUploadRegression() { return pulse::ui::ThumbnailCacheTestAccess::UploadRegression(); }

bool RunThumbnailInterestRegression() { return pulse::ui::ThumbnailCacheTestAccess::InterestRegression(); }

bool RunThumbnailRetryRegression() { return pulse::ui::ThumbnailCacheTestAccess::RetryRegression(); }
bool RunThumbnailDownloadRetryRegression() { return pulse::ui::ThumbnailCacheTestAccess::DownloadRetryRegression(); }
bool RunPsdProgressiveRegression(const char* fixture) { return pulse::ui::ThumbnailCacheTestAccess::PsdProgressiveRegression(fixture); }

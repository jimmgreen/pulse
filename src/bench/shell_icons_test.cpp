#include "../ui/shell_icons.h"
#include "../ui/thumbnail_artwork_layout.h"
#include <shellapi.h>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <algorithm>
#include <filesystem>

static bool TestArtworkBounds() {
    bool ok = true;
    const auto check = [&](pulse::ui::IconArtworkBounds bounds,
                           float left, float top, float right, float bottom,
                           const char* name) {
        const bool pass = bounds.left == left && bounds.top == top &&
                          bounds.right == right && bounds.bottom == bottom;
        std::printf("[%s] %s\n", pass ? "PASS" : "FAIL", name);
        ok &= pass;
    };
    std::vector<uint8_t> pixels(8 * 4 * 4, 0);
    using pulse::ui::MeasureIconArtwork;
    check(MeasureIconArtwork(pixels, 8, 4), 0, 0, 1, 1, "transparent artwork uses full-frame fallback");
    for (size_t y = 1; y < 3; ++y)
        for (size_t x = 2; x < 6; ++x) pixels[(y * 8 + x) * 4 + 3] = 255;
    check(MeasureIconArtwork(pixels, 8, 4), .25f, .25f, .75f, .75f,
          "transparent padding produces normalized pixel edges");
    pixels[(3 * 8 + 7) * 4 + 3] = 1;
    check(MeasureIconArtwork(pixels, 8, 4), .25f, .25f, 1, 1,
          "faint edge pixels count as artwork");
    std::fill(pixels.begin(), pixels.end(), uint8_t{255});
    check(MeasureIconArtwork(pixels, 8, 4), 0, 0, 1, 1, "opaque artwork preserves full frame");
    check(MeasureIconArtwork(pixels, 0, 4), 0, 0, 1, 1, "zero-size input uses fallback");
    check(MeasureIconArtwork(pixels, 9, 4), 0, 0, 1, 1, "truncated input uses fallback");
    std::vector<uint8_t> padded(2 * 16, 0);
    padded[16 + 4 + 3] = 255;
    padded[15] = 255;  // Padding is not image artwork.
    check(MeasureIconArtwork(padded, 2, 2, 16), .5f, .5f, 1, 1,
          "thumbnail alpha bounds honor padded row stride");
    check(MeasureIconArtwork(padded, 2, 2, 4), 0, 0, 1, 1,
          "undersized row stride uses fallback");
    const D2D1_RECT_F dest = D2D1::RectF(10, 20, 110, 120);
    const pulse::ui::IconArtworkBounds inset{.25f, .25f, .75f, .75f};
    const auto geometry = [&](uint32_t w, uint32_t h,
                              pulse::ui::IconArtworkBounds bounds, bool bottom,
                              float left, float top, float right, float lower,
                              const char* name) {
        const auto fitted = pulse::ui::ContainedThumbnailRect(dest, w, h, bounds, bottom);
        const auto artwork = pulse::ui::ThumbnailArtworkRect(fitted, bounds);
        check({artwork.left, artwork.top, artwork.right, artwork.bottom},
              left, top, right, lower, name);
    };
    geometry(100, 100, inset, false, 35, 45, 85, 95,
             "default thumbnail retains centered transparent artwork");
    geometry(100, 100, inset, true, 35, 70, 85, 120,
             "transparent thumbnail aligns actual artwork to slot bottom");
    geometry(200, 100, {}, false, 10, 45, 110, 95,
             "default opaque landscape thumbnail retains contain layout");
    geometry(200, 100, {}, true, 10, 70, 110, 120,
             "landscape thumbnail bottom alignment preserves aspect ratio");
    geometry(100, 200, {}, true, 35, 20, 85, 120,
             "portrait thumbnail bottom alignment preserves horizontal centering");

    // The 256-pixel list's frame around an icon that has only a small image.
    using pulse::ui::UndersizedIconSide;
    using pulse::ui::CropUndersizedIcon;
    const auto frame = [](size_t side, size_t from, size_t to) {
        std::vector<uint8_t> bgra(side * side * 4, 0);
        for (size_t y = from; y < to; ++y)
            for (size_t x = from; x < to; ++x) {
                const size_t at = (y * side + x) * 4;
                bgra[at] = static_cast<uint8_t>(x); bgra[at + 1] = static_cast<uint8_t>(y);
                bgra[at + 2] = 7; bgra[at + 3] = 255;
            }
        return bgra;
    };
    const auto side_of = [&](size_t side, size_t from, size_t to) {
        const auto bgra = frame(side, from, to);
        return UndersizedIconSide(MeasureIconArtwork(bgra, side, side), side, side);
    };
    const bool sides = side_of(256, 0, 48) == 48 && side_of(256, 4, 44) == 48 && side_of(256, 0, 32) == 32 &&
                       side_of(256, 2, 30) == 32 && side_of(256, 0, 16) == 16 && side_of(256, 0, 128) == 128 &&
                       side_of(256, 0, 129) == 0 && side_of(256, 0, 256) == 0 && side_of(256, 40, 200) == 0 &&
                       side_of(256, 100, 120) == 128 && side_of(48, 0, 20) == 0 && side_of(64, 0, 24) == 0 &&
                       UndersizedIconSide({}, 256, 256) == 0 &&
                       UndersizedIconSide({0, 0, .1f, .1f}, 256, 128) == 0;
    std::printf("[%s] %s\n", sides ? "PASS" : "FAIL",
                "only a small image in the top-left of a large frame is taken for an undersized icon");
    ok &= sides;
    {
        auto bgra = frame(256, 4, 44);
        uint32_t width = 256, height = 256;
        auto bounds = MeasureIconArtwork(bgra, width, height);
        CropUndersizedIcon(bgra, width, height, bounds);
        const size_t at = (10 * 48 + 20) * 4;
        const bool cropped = width == 48 && height == 48 && bgra.size() == 48u * 48u * 4u &&
            bounds.native_side == 48 && bounds.left == 4.0f / 48 && bounds.top == 4.0f / 48 &&
            bounds.right == 44.0f / 48 && bounds.bottom == 44.0f / 48 &&
            bgra[at] == 20 && bgra[at + 1] == 10 && bgra[at + 3] == 255 && bgra[3] == 0;
        std::printf("[%s] %s\n", cropped ? "PASS" : "FAIL",
                    "an undersized icon is cut to its own image with its margins and artwork kept");
        ok &= cropped;
        auto full = frame(256, 0, 256);
        uint32_t full_width = 256, full_height = 256;
        auto full_bounds = MeasureIconArtwork(full, full_width, full_height);
        CropUndersizedIcon(full, full_width, full_height, full_bounds);
        const bool kept = full_width == 256 && full.size() == 256u * 256u * 4u && full_bounds.native_side == 0;
        std::printf("[%s] %s\n", kept ? "PASS" : "FAIL", "a real large icon is left as it is");
        ok &= kept;
    }
    using pulse::ui::IconDrawSide;
    const bool draw_sides = IconDrawSide(256, 48) == 96 && IconDrawSide(80, 48) == 80 && IconDrawSide(96, 48) == 96 &&
                            IconDrawSide(256, 32) == 64 && IconDrawSide(256, 0) == 256 && IconDrawSide(40, 0) == 40;
    std::printf("[%s] %s\n", draw_sides ? "PASS" : "FAIL",
                "an undersized icon is drawn at most twice its size, anything else fills the cell");
    ok &= draw_sides;
    return ok;
}

// A generated .ico with only 32- and 48-pixel images: the Shell shows an .ico
// file as its own icon, like a program without a large icon.
static bool WriteSmallIco(const std::wstring& path) {
    std::vector<uint8_t> out;
    const auto put16 = [&](uint32_t v) { out.push_back(static_cast<uint8_t>(v)); out.push_back(static_cast<uint8_t>(v >> 8)); };
    const auto put32 = [&](uint32_t v) { put16(v & 0xffff); put16(v >> 16); };
    const uint32_t sides[] = {32, 48};
    put16(0); put16(1); put16(2);
    uint32_t offset = 6 + 16 * 2;
    std::vector<std::vector<uint8_t>> images;
    for (const uint32_t side : sides) {
        std::vector<uint8_t> image;
        const auto i16 = [&](uint32_t v) { image.push_back(static_cast<uint8_t>(v)); image.push_back(static_cast<uint8_t>(v >> 8)); };
        const auto i32 = [&](uint32_t v) { i16(v & 0xffff); i16(v >> 16); };
        i32(40); i32(side); i32(side * 2); i16(1); i16(32); i32(0); i32(0); i32(0); i32(0); i32(0); i32(0);
        for (uint32_t y = 0; y < side; ++y)
            for (uint32_t x = 0; x < side; ++x) {
                const bool ink = x >= side / 8 && x < side - side / 8 && y >= side / 8 && y < side - side / 8;
                image.push_back(ink ? 200 : 0); image.push_back(ink ? 80 : 0);
                image.push_back(ink ? 30 : 0); image.push_back(ink ? 255 : 0);
            }
        const uint32_t mask_row = ((side + 31) / 32) * 4;
        image.insert(image.end(), static_cast<size_t>(mask_row) * side, 0);
        out.push_back(static_cast<uint8_t>(side)); out.push_back(static_cast<uint8_t>(side));
        out.push_back(0); out.push_back(0); put16(1); put16(32);
        put32(static_cast<uint32_t>(image.size())); put32(offset);
        offset += static_cast<uint32_t>(image.size());
        images.push_back(std::move(image));
    }
    for (const auto& image : images) out.insert(out.end(), image.begin(), image.end());
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool wrote = WriteFile(file, out.data(), static_cast<DWORD>(out.size()), &written, nullptr) &&
                       written == out.size();
    CloseHandle(file);
    return wrote;
}

namespace pulse::ui {
struct ShellIconCacheTestAccess {
    static bool MeasureShortcuts(const std::filesystem::path& folder) {
        ShellIconCache cache;
        std::vector<std::wstring> paths;
        for (const auto& file : std::filesystem::directory_iterator(std::filesystem::absolute(folder))) {
            if (file.path().extension() == L".lnk") paths.push_back(std::filesystem::path(file.path()).make_preferred().wstring());
        }
        const auto started = std::chrono::steady_clock::now();
        for (const auto& path : paths) cache.RequestExact(path);
        if (!Wait(cache, 60000)) {
            std::printf("[FAIL] shortcut icon queries exceeded 60 seconds\n");
            return false;
        }
        const auto indexed = std::chrono::steady_clock::now();
        {
            std::lock_guard lock(cache.mutex_);
            if (cache.exact_index_.size() != paths.size()) {
                std::printf("[FAIL] only %zu/%zu shortcut indices resolved\n", cache.exact_index_.size(), paths.size());
                return false;
            }
        }
        ComPtr<ID3D11Device> d3d;
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<ID2D1Factory1> factory;
        ComPtr<ID2D1Device> device;
        ComPtr<ID2D1DeviceContext> context;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr)) ||
            FAILED(d3d->QueryInterface(IID_PPV_ARGS(&dxgi))) ||
            FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&factory))) ||
            FAILED(factory->CreateDevice(dxgi.get(), &device)) ||
            FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context))) return false;
        cache.SetDeviceContext(context.get());
        const auto draw_start = std::chrono::steady_clock::now();
        size_t ready = 0;
        double longest_ms = 0;
        for (const auto& path : paths) {
            const auto one = std::chrono::steady_clock::now();
            ready += cache.ExactBitmapFor(path, 48) != nullptr;
            longest_ms = (std::max)(longest_ms, std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - one).count());
        }
        const double draw_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - draw_start).count();
        std::printf("shortcuts=%zu index_ms=%.3f first_draw_ms=%.3f longest_icon_ms=%.3f ready=%zu\n",
            paths.size(), std::chrono::duration<double, std::milli>(indexed - started).count(),
            draw_ms, longest_ms, ready);
        std::fflush(stdout);
        size_t completed = 0;
        for (const auto& path : paths) completed += ReadyBitmap(cache, path, L"", false, FILE_ATTRIBUTE_NORMAL, 48) != nullptr;
        std::printf("[%s] %zu/%zu exact shortcut bitmaps eventually available; completion_ms=%.3f\n",
            completed == paths.size() ? "PASS" : "FAIL", completed, paths.size(),
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - draw_start).count());
        return paths.size() == 265 && completed == paths.size();
    }
    static ID2D1Bitmap* ReadyBitmap(ShellIconCache& cache, const std::wstring& path,
        const std::wstring& name, bool is_dir, DWORD attrs, float size) {
        const auto deadline = GetTickCount64() + 10000;
        do {
            auto* bitmap = path.empty() ? cache.BitmapFor(path, name, is_dir, attrs, size)
                                       : cache.ExactBitmapFor(path, size);
            if (bitmap) return bitmap;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (GetTickCount64() < deadline);
        return nullptr;
    }
    static bool Wait(ShellIconCache& cache, ULONGLONG timeout_ms = 10000) {
        const auto end = GetTickCount64() + timeout_ms;
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
            {
                ShellIconCache pending;
                pending.SetDeviceContext(context.get());
                // Hold the worker before dispatch: a draw must only queue the
                // work, even when the native Shell index is already known.
                pending.running_ = true;
                pending.generic_index_[L"<dir>"] = cache.generic_index_.at(L"<dir>");
                const auto* first = pending.BitmapFor(L"", L"", true, FILE_ATTRIBUTE_DIRECTORY, 48);
                const auto* second = pending.BitmapFor(L"", L"", true, FILE_ATTRIBUTE_DIRECTORY, 48);
                check(!first && !second && pending.convert_queue_.size() == 1 &&
                      pending.convert_pending_.size() == 1 && pending.bitmaps_.empty(),
                      "ordinary paints queue one conversion without extracting icons on the UI thread");
                {
                    std::lock_guard lock(pending.mutex_);
                    pending.running_ = false;
                    pending.StartWorkerLocked();
                }
                pending.cv_.notify_one();
                check(ReadyBitmap(pending, L"", L"", true, FILE_ATTRIBUTE_DIRECTORY, 48) != nullptr,
                      "queued ordinary icon becomes available after worker dispatch");
            }
            for (const float size : {16.0f, 24.0f, 32.0f, 48.0f, 96.0f, 256.0f}) {
                auto* bitmap = ReadyBitmap(cache, L"", L"", true, FILE_ATTRIBUTE_DIRECTORY, size);
                check(bitmap && bitmap->GetPixelSize().width >= 16,
                      "native folder converts to Direct2D bitmap at requested scale");
                const auto bounds = cache.CachedArtworkBounds(L"", L"", true,
                    FILE_ATTRIBUTE_DIRECTORY, size);
                check(bounds.left >= 0 && bounds.top >= 0 && bounds.right <= 1 &&
                      bounds.bottom <= 1 && bounds.left < bounds.right && bounds.top < bounds.bottom &&
                      cache.artwork_bounds_.size() == cache.bitmaps_.size(),
                      "converted icon caches valid normalized artwork bounds");
            }
            // Multi-window M3d: a second window's context on the same device
            // keeps every bitmap, and the first window's context may go away.
            ComPtr<ID2D1DeviceContext> second;
            ComPtr<ID2D1Bitmap1> second_target;
            const auto target_props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            const bool second_ok =
                SUCCEEDED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &second)) &&
                SUCCEEDED(second->CreateBitmap(D2D1::SizeU(64, 64), nullptr, 0, &target_props, &second_target));
            check(second_ok, "second window context on the same device");
            if (second_ok) {
                second->SetTarget(second_target.get());
                auto* const before = ReadyBitmap(cache, L"", L"", true, FILE_ATTRIBUTE_DIRECTORY, 32);
                // Held so a flushed cache cannot free it or hand its address to a new bitmap.
                ComPtr<ID2D1Bitmap> held;
                if (before) { before->AddRef(); held.p = before; }
                const size_t cached = cache.bitmaps_.size();
                cache.SetDeviceContext(second.get());
                check(cache.bitmaps_.size() == cached &&
                          ReadyBitmap(cache, L"", L"", true, FILE_ATTRIBUTE_DIRECTORY, 32) == before,
                      "another window on the same device keeps the cached bitmaps");
                second->BeginDraw();
                if (held.get()) second->DrawBitmap(held.get(), D2D1::RectF(0, 0, 32, 32));
                check(before && SUCCEEDED(second->EndDraw()), "a cached icon draws in the other window");
                context.reset();
                check(ReadyBitmap(cache, L"", L"", true, FILE_ATTRIBUTE_DIRECTORY, 20) != nullptr,
                      "icons still convert after the first window's context is released");
                cache.SetDeviceContext(second.get());
            }
            // A file whose icon has no large image: cut to it at the large
            // size and marked to be drawn at most twice that; untouched at
            // its own size, and a folder's real large icon stays whole.
            wchar_t temp[MAX_PATH]{};
            GetTempPathW(MAX_PATH, temp);
            const std::wstring ico_dir = std::wstring(temp) + L"pulse_icon_test_" + std::to_wstring(GetCurrentProcessId());
            const std::wstring ico = ico_dir + L"\\small only.ico";
            CreateDirectoryW(ico_dir.c_str(), nullptr);
            check(WriteSmallIco(ico), "generated an icon file with only 32- and 48-pixel images");
            cache.RequestExact(ico);
            check(Wait(cache), "the generated icon file's own icon resolves");
            auto* const large = ReadyBitmap(cache, ico, L"small only.ico", false, FILE_ATTRIBUTE_NORMAL, 256);
            const auto large_px = large ? large->GetPixelSize() : D2D1_SIZE_U{};
            const uint32_t large_native = cache.CachedNativeSide(ico, L"small only.ico", false, FILE_ATTRIBUTE_NORMAL, 256);
            std::printf("small-only icon at 256: bitmap %ux%u native %u\n", large_px.width, large_px.height, large_native);
            check(large && large_px.width == 48 && large_px.height == 48 && large_native == 48,
                  "an icon without a large image is cut to its 48-pixel image at the large size");
            auto* const own = ReadyBitmap(cache, ico, L"small only.ico", false, FILE_ATTRIBUTE_NORMAL, 48);
            check(own && cache.CachedNativeSide(ico, L"small only.ico", false, FILE_ATTRIBUTE_NORMAL, 48) == 0,
                  "the same icon at its own size is drawn as it is");
            auto* const folder = ReadyBitmap(cache, L"", L"", true, FILE_ATTRIBUTE_DIRECTORY, 256);
            check(folder && folder->GetPixelSize().width == 256 &&
                      cache.CachedNativeSide(L"", L"", true, FILE_ATTRIBUTE_DIRECTORY, 256) == 0,
                  "a folder's real large icon stays whole");
            DeleteFileW(ico.c_str());
            RemoveDirectoryW(ico_dir.c_str());
            cache.SetDeviceContext(nullptr);
            check(cache.artwork_bounds_.empty(), "device replacement clears artwork metadata");
            if (!context.get()) context = std::move(second);
            cache.SetDeviceContext(context.get());
            check(ReadyBitmap(cache, L"", L"", true, FILE_ATTRIBUTE_DIRECTORY, 32) != nullptr,
                  "device recreation preserves native indices and rebuilds bitmap");
            cache.Reset();
        }
        return ok;
    }
};
}

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--shortcut-latency") == 0) {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const bool ok = pulse::ui::ShellIconCacheTestAccess::MeasureShortcuts(std::filesystem::path(argv[2]));
        if (SUCCEEDED(hr)) CoUninitialize();
        return ok ? 0 : 1;
    }
    const bool bounds_ok = TestArtworkBounds();
    if (argc == 2 && std::strcmp(argv[1], "--bounds-only") == 0) return bounds_ok ? 0 : 1;
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool ok = pulse::ui::ShellIconCacheTestAccess::Run() && bounds_ok;
    if (SUCCEEDED(hr)) CoUninitialize();
    return ok ? 0 : 1;
}

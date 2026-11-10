    static bool EditorSaveRegression() {
        bool ok = true;
        const auto check = [&](bool value, const char* name) {
            std::printf("[%s] %s\n", value ? "PASS" : "FAIL", name);
            std::fflush(stdout); ok &= value;
        };
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const auto directory = std::filesystem::absolute(L"bench_data") /
            (L"editor-save-live-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(directory);
        const auto path = directory / L"edited.bmp";
        const auto temporary = directory / L"save.tmp";
        const auto bytes = [](unsigned channel) {
            BITMAPFILEHEADER file{};
            BITMAPINFOHEADER info{};
            file.bfType = 0x4d42; file.bfOffBits = sizeof(file) + sizeof(info);
            file.bfSize = file.bfOffBits + 16;
            info.biSize = sizeof(info); info.biWidth = 2; info.biHeight = 2;
            info.biPlanes = 1; info.biBitCount = 24; info.biSizeImage = 16;
            std::vector<char> data(file.bfSize);
            memcpy(data.data(), &file, sizeof(file));
            memcpy(data.data() + sizeof(file), &info, sizeof(info));
            for (unsigned y = 0; y < 2; ++y)
                for (unsigned x = 0; x < 2; ++x) data[file.bfOffBits + y * 8 + x * 3 + channel] = char(-1);
            return data;
        };
        const auto write = [&](const std::filesystem::path& target, unsigned channel) {
            const auto data = bytes(channel);
            std::ofstream output(target, std::ios::binary | std::ios::trunc);
            output.write(data.data(), static_cast<std::streamsize>(data.size()));
            output.close(); return output.good();
        };
        ComPtr<ID3D11Device> d3d;
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<ID2D1Device> device;
        ComPtr<ID2D1DeviceContext> dc;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
        if (SUCCEEDED(hr)) hr = d3d->QueryInterface(IID_PPV_ARGS(&dxgi));
        if (SUCCEEDED(hr)) hr = D2D1CreateDevice(dxgi.get(), nullptr, &device);
        if (SUCCEEDED(hr)) hr = device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc);
        check(SUCCEEDED(hr), "create real WARP thumbnail graphics context");
        if (ok) {
            ThumbnailCache cache;
            cache.SetParallelism(1); cache.SetDeviceContext(dc.get());
            uint64_t modified = 0, size = 0;
            const auto metadata = [&] {
                WIN32_FILE_ATTRIBUTE_DATA data{};
                if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
                modified = (uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
                size = (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
                return true;
            };
            const auto draw = [&] {
                return cache.DrawGridThumbnail(dc.get(), D2D1::RectF(0, 0, 128, 128),
                    path.wstring(), FILE_ATTRIBUTE_NORMAL, 128, 1, modified, size, 1, false, nullptr);
            };
            const auto await_pixels = [&](unsigned channel) {
                bool pixels = false;
                const auto end = GetTickCount64() + 12000;
                do {
                    { std::lock_guard lock(cache.mutex_);
                      const auto found = cache.items_.find(cache.Key(path.wstring(), 128, modified, size));
                      if (found != cache.items_.end() && !found->second.failed && found->second.pixels.size() >= 4) {
                          const auto& p = found->second.pixels;
                          pixels = p[channel] == 255 && p[(channel + 1) % 3] == 0 && p[(channel + 2) % 3] == 0;
                      } }
                    if (draw() == PreviewDrawResult::Bitmap && pixels) return true;
                    Sleep(10);
                } while (GetTickCount64() < end);
                return false;
            };
            check(write(path, 2) && metadata() && await_pixels(2), "original image decodes red pixels through actual preview host");
            Sleep(20);
            HANDLE writer = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            check(writer != INVALID_HANDLE_VALUE && metadata(), "editor truncates original file and holds exclusive save handle");
            draw();
            bool failed = false;
            const auto deadline = GetTickCount64() + 12000;
            do {
                { std::lock_guard lock(cache.mutex_);
                  const auto item = cache.items_.find(cache.Key(path.wstring(), 128, modified, size));
                  failed = item != cache.items_.end() && item->second.failed && item->second.transient; }
                if (!failed) Sleep(10);
            } while (!failed && GetTickCount64() < deadline);
            check(failed, "actual locked image decode fails with automatic retry scheduled");
            const auto green = bytes(1); DWORD written = 0;
            const bool saved = writer != INVALID_HANDLE_VALUE && WriteFile(writer, green.data(),
                static_cast<DWORD>(green.size()), &written, nullptr) && written == green.size();
            if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
            check(saved && await_pixels(1), "overwrite close recovers green thumbnail even before snapshot metadata refresh");
            Sleep(20);
            check(write(temporary, 0) && ReplaceFileW(path.c_str(), temporary.c_str(), nullptr, 0, nullptr, nullptr) &&
                metadata() && await_pixels(0), "real atomic replacement at same path displays new blue pixels");

            // A host timeout/crash can occur during saving. Inject transport failures
            // at the cache boundary, then require a real decoder request to recover.
            ThumbnailCache::Request req;
            req.path = path.wstring(); req.pixels = 128; req.modified = modified; req.size = size;
            req.key = cache.Key(req.path, 128, modified, size);
            req.identity = cache.Key(req.path, 0, modified, size); req.epoch = cache.epoch_.load();
            for (unsigned i = 0; i < 4; ++i) {
                ThumbnailCache::Item error; error.failed = true; error.transient = true;
                cache.StoreResult(req, std::move(error));
            }
            bool retryable = false;
            { std::lock_guard lock(cache.mutex_);
              auto& item = cache.items_.at(req.key);
              retryable = item.transient && item.retry_at > GetTickCount64();
              item.retry_at = GetTickCount64(); } // Fast-forward backoff, not decode.
            check(retryable, "four consecutive transport failures do not permanently suppress image thumbnail");
            check(await_pixels(0), "real preview and GPU upload recover after four injected transport failures");
            cache.Reset();
        }
        std::error_code error;
        std::filesystem::remove_all(directory, error);
        check(!error, "isolated editor fixtures cleaned after preview shutdown");
        if (SUCCEEDED(initialized)) CoUninitialize();
        return ok;
    }

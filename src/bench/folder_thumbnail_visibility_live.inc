    static int LiveVisibility(const wchar_t* root, const wchar_t* output) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
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
        if (SUCCEEDED(hr)) hr = dc->CreateBitmap(D2D1::SizeU(864, 96), nullptr, 0, &props, &target);
        if (FAILED(hr)) return 2;
        dc->SetTarget(target.get());
        const HWND a = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
        const HWND b = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
        if (!a || !b) return 2;
        FolderThumbnailCache folders; folders.SetDeviceContext(dc.get());
        auto& cache = folders.cache_;
        const auto begin = [&](HWND owner, bool full = true) {
#ifdef PULSE_FOLDER_VISIBILITY_BASELINE
            (void)owner; (void)full; folders.BeginFrame();
#else
            folders.BeginFrame(owner, full);
#endif
        };
        const auto paint = [&] {
            begin(a); dc->BeginDraw(); dc->Clear(D2D1::ColorF(0.16f, 0.16f, 0.16f));
            int count = 0;
            for (int i = 0; i < 9; ++i) {
                const float x = static_cast<float>(i * 96);
                count += folders.Draw(dc.get(), D2D1::RectF(x + 8, 8, x + 88, 88),
                    std::wstring(root) + L"\\folder" + std::to_wstring(i), FILE_ATTRIBUTE_DIRECTORY,
                    96, 1, 1, 0, 1, false, nullptr) == PreviewDrawResult::Bitmap;
            }
            hr = dc->EndDraw(); folders.EndFrame(); return count;
        };
        const auto deadline = GetTickCount64() + 20000;
        int warm = 0;
        while (GetTickCount64() < deadline && SUCCEEDED(hr)) {
            warm = paint(); if (warm == 9) break; Sleep(15);
        }
        std::printf("[%s] real folder worker IPC and WARP warm=%d hr=%08lx\n", warm == 9 ? "PASS" : "FAIL", warm, hr);
        bool ok = warm == 9 && SUCCEEDED(hr);
        if (ok) {
            ok &= CaptureRevisit(dc.get(), target.get(), std::wstring(output) + L"\\before.bmp");
            begin(b); folders.EndFrame();
            const size_t freed = folders.TrimInactiveWorkingSet();
            warm = paint();
            ok &= CaptureRevisit(dc.get(), target.get(), std::wstring(output) + L"\\after-other-window.bmp");
            const bool retained = warm == 9 && freed == 0 && cache.pending_.empty() && cache.queue_.empty();
            std::printf("[%s] real decoded folders after other-window frame/trim warm=%d freed=%zu queued=%zu\n",
                retained ? "PASS" : "FAIL", warm, freed, cache.queue_.size());
            ok &= retained;
            begin(a, false); folders.EndFrame();
            folders.TrimInactiveWorkingSet(); warm = paint();
            ok &= CaptureRevisit(dc.get(), target.get(), std::wstring(output) + L"\\after-partial.bmp");
            const bool partial = warm == 9 && cache.pending_.empty() && cache.queue_.empty();
            std::printf("[%s] real decoded folders after damage-only frame/trim warm=%d\n", partial ? "PASS" : "FAIL", warm);
            ok &= partial && SUCCEEDED(hr);
        }
        folders.Reset(); DestroyWindow(a); DestroyWindow(b);
        return ok ? 0 : 1;
    }

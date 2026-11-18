    static bool CaptureRevisit(ID2D1DeviceContext* dc, ID2D1Bitmap1* target, const std::wstring& path) {
        ComPtr<ID2D1Bitmap1> read;
        const auto size = target->GetPixelSize();
        const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            target->GetPixelFormat());
        if (FAILED(dc->CreateBitmap(size, nullptr, 0, &props, &read)) ||
            FAILED(read->CopyFromBitmap(nullptr, target, nullptr))) return false;
        D2D1_MAPPED_RECT mapped{};
        if (FAILED(read->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
        FILE* file = nullptr;
        bool ok = _wfopen_s(&file, path.c_str(), L"wb") == 0 && file;
        if (ok) {
            BITMAPFILEHEADER header{};
            header.bfType = 0x4d42; header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
            header.bfSize = header.bfOffBits + size.width * size.height * 4;
            BITMAPINFOHEADER info{};
            info.biSize = sizeof(info); info.biWidth = size.width; info.biHeight = -static_cast<LONG>(size.height);
            info.biPlanes = 1; info.biBitCount = 32; info.biCompression = BI_RGB;
            ok = fwrite(&header, sizeof(header), 1, file) == 1 && fwrite(&info, sizeof(info), 1, file) == 1;
            for (UINT y = 0; y < size.height; ++y)
                ok &= fwrite(mapped.bits + y * mapped.pitch, size.width * 4, 1, file) == 1;
            fclose(file);
        }
        read->Unmap(); return ok;
    }
    static int LiveScroll(const wchar_t* root, const wchar_t* output) {
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
        if (SUCCEEDED(hr)) hr = dc->CreateBitmap(D2D1::SizeU(1024, 512), nullptr, 0, &props, &target);
        if (FAILED(hr)) { printf("[FAIL] live scroll target hr=%08lx\n", hr); return 2; }
        dc->SetTarget(target.get());
        FolderThumbnailCache folders;
        folders.SetDeviceContext(dc.get());
        auto& cache = folders.cache_;
        const auto paint = [&](int first) {
            folders.BeginFrame(); dc->BeginDraw(); dc->Clear(D2D1::ColorF(1, 1, 1));
            int drawn = 0;
            for (int i = 0; i < 8; ++i) {
                const auto path = std::wstring(root) + L"\\folder" + std::to_wstring(first + i);
                const float x = static_cast<float>(i % 4) * 256 + 15;
                const float y = static_cast<float>(i / 4) * 256 + 15;
                drawn += folders.Draw(dc.get(), D2D1::RectF(x, y, x + 226, y + 226), path,
                    FILE_ATTRIBUTE_DIRECTORY, 512, 1, 1, 0, 1, false, nullptr) == PreviewDrawResult::Bitmap;
            }
            hr = dc->EndDraw(); folders.EndFrame();
            return drawn;
        };
        const auto started = GetTickCount64();
        for (int first = 0; first < 40; first += 8) {
            const auto deadline = GetTickCount64() + 15000;
            bool ready = false;
            while (GetTickCount64() < deadline) {
                const int drawn = paint(first);
                {
                    std::lock_guard lock(cache.mutex_);
                    size_t full = 0;
                    for (const auto& [key, visible] : folders.visible_) {
                        const auto it = cache.items_.find(key);
                        full += it != cache.items_.end() && it->second.bitmap.get();
                    }
                    ready = drawn == 8 && full == 8 && cache.pending_.empty();
                }
                if (ready || FAILED(hr)) break;
                Sleep(15);
            }
            printf("[%s] live screen %d decoded uploaded and drawn\n", ready ? "PASS" : "FAIL", first / 8);
            if (!ready || FAILED(hr)) return 1;
            if (first == 0 && !CaptureRevisit(dc.get(), target.get(), std::wstring(output) + L"\\first.bmp")) return 2;
        }
        const int warm = paint(0);
        const auto uploads = cache.shared_upload_budget_->bytes.load();
        const bool captured = CaptureRevisit(dc.get(), target.get(), std::wstring(output) + L"\\return.bmp");
        const bool ok = warm == 8 && uploads == 0 && captured && SUCCEEDED(hr);
        printf("[%s] live return warm=%d upload=%zu elapsed=%llu ms\n", ok ? "PASS" : "FAIL",
            warm, uploads, static_cast<unsigned long long>(GetTickCount64() - started));
        folders.Reset();
        return ok ? 0 : 1;
    }

    static bool OrientationZoomRegression() {
        int count=0, failures=0;
        auto check=[&](bool ok,const char* label){++count;failures+=!ok;std::printf("[%s] %s\n",ok?"PASS":"FAIL",label);std::fflush(stdout);};
        CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        ComPtr<ID3D11Device> d3d;ComPtr<IDXGIDevice> dxgi;ComPtr<ID2D1Device> device;ComPtr<ID2D1DeviceContext> dc;
        HRESULT hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&d3d,nullptr,nullptr);
        if(SUCCEEDED(hr))hr=d3d->QueryInterface(IID_PPV_ARGS(&dxgi));
        if(SUCCEEDED(hr))hr=D2D1CreateDevice(dxgi.get(),nullptr,&device);
        if(SUCCEEDED(hr))hr=device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,&dc);
        check(SUCCEEDED(hr),"create actual WARP D2D thumbnail context");if(FAILED(hr)){CoUninitialize();return false;}
        ComPtr<ID2D1Bitmap1> target,readback;
        auto props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED));
        hr=dc->CreateBitmap(D2D1::SizeU(512,512),nullptr,0,&props,&target);
        props.bitmapOptions=D2D1_BITMAP_OPTIONS_CPU_READ|D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
        if(SUCCEEDED(hr))hr=dc->CreateBitmap(D2D1::SizeU(512,512),nullptr,0,&props,&readback);
        check(SUCCEEDED(hr),"create actual render target and pixel readback");if(FAILED(hr)){CoUninitialize();return false;}
        dc->SetTarget(target.get());
        const uint8_t colors[4][4]={{0,0,255,255},{0,255,0,255},{255,0,0,255},{0,255,255,255}};
        ThumbnailCache cache;cache.SetDeviceContext(dc.get());
        for(bool alpha:{false,true}) {
            std::vector<uint8_t> pixels(64*48*4);
            for(unsigned y=0;y<48;++y)for(unsigned x=0;x<64;++x){auto* p=pixels.data()+(y*64+x)*4;const auto* c=colors[(y>=24?2:0)+(x>=32?1:0)];for(int k=0;k<4;++k)p[k]=c[k];if(alpha){p[3]=192;for(int k=0;k<3;++k)p[k]=static_cast<uint8_t>(p[k]*192/255);}}
            MicroThumbData micro;check(EncodeMicroThumb(pixels.data(),64,48,256,64,48,64,micro),"encode asymmetric BC micro fixture");
            std::vector<uint8_t> decoded;uint32_t w=0,h=0,stride=0;
            bool cpu=DecodeMicroThumbToPbgra(micro,decoded,w,h,stride);
            if(cpu)for(unsigned corner=0;corner<4;++corner){const unsigned x=corner%2?48:16,y=corner/2?36:12;for(unsigned c=0;c<3;++c)cpu &= std::abs(int(decoded[y*stride+x*4+c])-int(colors[corner][c]*(alpha?192:255)/255))<16;}
            check(cpu,"BC CPU decode preserves four-corner orientation");
            for(uint64_t id=1;id<=70;++id)cache.StageMicroLocked(id+(alpha?1000:0),5,6,micro);
            cache.FlushStagedMicros(dc.get(),0,140);
            for(uint64_t id:{1ull,2ull,33ull,65ull,70ull})for(unsigned edge:{32u,64u,96u,128u,256u,512u,48u,192u,64u}) {
                dc->BeginDraw();dc->Clear(D2D1::ColorF(0,0.0f));D2D1_RECT_F fitted{};
                const auto result=cache.TryDrawMicroThumb(dc.get(),D2D1::RectF(0,0,float(edge),float(edge)),id+(alpha?1000:0),5,6,1,false,&fitted,nullptr,nullptr,nullptr,nullptr);
                const HRESULT ended=dc->EndDraw();
                bool correct=result==PreviewDrawResult::Bitmap&&SUCCEEDED(ended)&&SUCCEEDED(readback->CopyFromBitmap(nullptr,target.get(),nullptr));
                D2D1_MAPPED_RECT mapped{};
                if(correct&&SUCCEEDED(readback->Map(D2D1_MAP_OPTIONS_READ,&mapped))){
                    for(unsigned corner=0;corner<4;++corner){const unsigned x=static_cast<unsigned>(fitted.left+(fitted.right-fitted.left)*(corner%2?0.75f:0.25f)),y=static_cast<unsigned>(fitted.top+(fitted.bottom-fitted.top)*(corner/2?0.75f:0.25f));for(unsigned c=0;c<3;++c)correct &= std::abs(int(mapped.bits[y*mapped.pitch+x*4+c])-int(colors[corner][c]*(alpha?192:255)/255))<24;}
                    readback->Unmap();
                }else correct=false;
                char label[128];sprintf_s(label,"GPU micro atlas four corners alpha=%d slot=%llu zoom=%u",alpha,static_cast<unsigned long long>(id),edge);check(correct,label);
            }
        }
        cache.Evict();
        cache.visible_order_ = true;
        const std::wstring path = L"orientation-grid.jpg";
        const auto identity = cache.Key(path, 0, 5, 6);
        std::vector<uint8_t> pixels(64 * 48 * 4);
        for (unsigned y = 0; y < 48; ++y) for (unsigned x = 0; x < 64; ++x)
            std::memcpy(pixels.data() + (y * 64 + x) * 4, colors[(y >= 24 ? 2 : 0) + (x >= 32 ? 1 : 0)], 4);
        MicroThumbData micro;
        EncodeMicroThumb(pixels.data(), 64, 48, 256, 64, 48, 64, micro);
        cache.StageMicroLocked(MicroThumbPack::IdentityHash(path), 5, 6, micro);
        for (bool glide : {false, true}) for (unsigned tier : {64u, 128u, 256u, 64u, 256u, 128u}) {
            ThumbnailCache::Request req;
            req.path = path; req.key = cache.Key(path, tier, 5, 6); req.identity = identity;
            req.epoch = cache.epoch_.load(); req.pixels = tier;
            ThumbnailCache::Item item;
            item.w = 64; item.h = 48; item.stride = 256; item.pixels = pixels;
            item.cost = pixels.size(); item.kind = ipc::PreviewContentKind::Bitmap;
            check(cache.StoreResult(req, std::move(item)), "current generation completion accepted");
            auto stale = req; --stale.epoch;
            ThumbnailCache::Item wrong;
            wrong.w = 64; wrong.h = 48; wrong.stride = 256; wrong.pixels.assign(pixels.size(), 0);
            wrong.kind = ipc::PreviewContentKind::Bitmap;
            check(!cache.StoreResult(stale, std::move(wrong)), "late stale generation cannot overwrite upright thumbnail");
            cache.SetFastScrollGlide(glide);
            for (unsigned requested : {tier, tier == 64 ? 128u : 64u}) {
                cache.running_ = true;
                dc->BeginDraw(); dc->Clear(D2D1::ColorF(0, 0.0f));
                D2D1_RECT_F fitted{};
                const auto result = cache.DrawGridThumbnail(dc.get(), D2D1::RectF(0, 0, 256, 256),
                    path, 0, requested, 0, 5, 6, 1.0f, false, &fitted);
                const HRESULT ended = dc->EndDraw();
                cache.running_ = false;
                bool correct = result == PreviewDrawResult::Bitmap && SUCCEEDED(ended) &&
                    SUCCEEDED(readback->CopyFromBitmap(nullptr, target.get(), nullptr));
                D2D1_MAPPED_RECT mapped{};
                if (correct && SUCCEEDED(readback->Map(D2D1_MAP_OPTIONS_READ, &mapped))) {
                    for (unsigned corner = 0; corner < 4; ++corner) {
                        const unsigned x = static_cast<unsigned>(fitted.left + (fitted.right - fitted.left) * (corner % 2 ? 0.75f : 0.25f));
                        const unsigned y = static_cast<unsigned>(fitted.top + (fitted.bottom - fitted.top) * (corner / 2 ? 0.75f : 0.25f));
                        for (unsigned c = 0; c < 3; ++c)
                            correct &= std::abs(int(mapped.bits[y * mapped.pitch + x * 4 + c]) - int(colors[corner][c])) < 24;
                    }
                    readback->Unmap();
                } else correct = false;
                check(correct, "actual grid full/stale/micro tier switching preserves four corners with glide on/off");
            }
        }
        dc->SetTarget(nullptr);CoUninitialize();std::printf("THUMBNAIL_ORIENTATION_ZOOM_RESULT cases=%d failed=%d\n",count,failures);return failures==0;
    }

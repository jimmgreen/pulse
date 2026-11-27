// video_wallpaper_probe.cpp - Prototype for a video wallpaper, measuring what each playback path costs.
// Paths (--path):
//   engine-screen  IMFMediaEngine, windowless swap chain sized to the screen (UpdateVideoStream dst = screen)
//   engine-native  IMFMediaEngine, swap chain at the video's own size, DirectComposition scales it
//   reader         IMFSourceReader with hardware decode (D3D11VA); each decoded NV12/P010 surface goes through
//                  ID3D11VideoProcessor straight into a 2-buffer composition swap chain no bigger than the
//                  screen (NV12 when the GPU takes it, else BGRA); DirectComposition scales to the screen.
// Measures process private bytes / working set, this process's GPU local + non-local memory
// (IDXGIAdapter3::QueryVideoMemoryInfo), CPU %, frames shown / late, first-frame time, then the same after
// pausing, after releasing the player, and the first-frame time of reopening it.
// Usage: pulse_desktop_video_wallpaper_probe.exe <video> [--path P] [--swap nv12|bgra] [--seconds N]
//        [--onscreen] [--csv out.csv]
// The window sits off-screen unless --onscreen (then behind every window on the primary monitor).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_4.h>
#include <dcomp.h>
#include <mfapi.h>
#include <mferror.h>
#include <mftransform.h>
#include <mfidl.h>
#include <mfmediaengine.h>
#include <mfreadwrite.h>
#include <strmif.h>
#include <codecapi.h>
#include <pdh.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <wincodec.h>
#include "hevc_dxva.h"
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

struct Sample {
    double private_mb = 0, working_mb = 0, gpu_local_mb = 0, gpu_shared_mb = 0, cpu_pct = 0;
};

double Mb(uint64_t b) { return static_cast<double>(b) / (1024.0 * 1024.0); }

uint64_t CpuTime100ns() {
    FILETIME c{}, e{}, k{}, u{};
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    auto v = [](const FILETIME& f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return v(k) + v(u);
}

struct Meter {
    ComPtr<IDXGIAdapter3> adapter;
    uint64_t last_cpu = 0, last_tick = 0;
    unsigned cores = 1;
    void Start() {
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        cores = std::max<unsigned>(1, si.dwNumberOfProcessors);
        last_cpu = CpuTime100ns();
        last_tick = GetTickCount64();
    }
    Sample Take() {
        Sample s;
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof(pmc);
        if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
            s.private_mb = Mb(pmc.PrivateUsage);
            s.working_mb = Mb(pmc.WorkingSetSize);
        }
        if (adapter) {
            DXGI_QUERY_VIDEO_MEMORY_INFO local{}, shared{};
            adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
            adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared);
            s.gpu_local_mb = Mb(local.CurrentUsage);
            s.gpu_shared_mb = Mb(shared.CurrentUsage);
        }
        const uint64_t cpu = CpuTime100ns(), tick = GetTickCount64();
        const double wall_100ns = static_cast<double>(tick - last_tick) * 10000.0;
        if (wall_100ns > 0) s.cpu_pct = 100.0 * static_cast<double>(cpu - last_cpu) / (wall_100ns * cores);
        last_cpu = cpu;
        last_tick = tick;
        return s;
    }
};

// dwm.exe's GPU memory (PDH "GPU Process Memory"), so a cheaper path is not just cost moved into DWM.
struct DwmGpuMeter {
    PDH_HQUERY query = nullptr;
    std::vector<PDH_HCOUNTER> dedicated, shared;
    bool Start() {
        DWORD dwm = 0;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe{sizeof(pe)};
            if (Process32FirstW(snap, &pe)) do {
                if (_wcsicmp(pe.szExeFile, L"dwm.exe") == 0) dwm = pe.th32ProcessID;
            } while (Process32NextW(snap, &pe));
            CloseHandle(snap);
        }
        if (!dwm || PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) return false;
        auto add = [&](const wchar_t* counter, std::vector<PDH_HCOUNTER>& out) {
            wchar_t wild[256];
            std::swprintf(wild, 256, L"\\GPU Process Memory(pid_%lu_*)\\%s", dwm, counter);
            DWORD len = 0;
            PdhExpandWildCardPathW(nullptr, wild, nullptr, &len, 0);
            if (!len) return;
            std::vector<wchar_t> list(len + 2);
            if (PdhExpandWildCardPathW(nullptr, wild, list.data(), &len, 0) != ERROR_SUCCESS) return;
            for (const wchar_t* path = list.data(); *path; path += wcslen(path) + 1) {
                PDH_HCOUNTER h = nullptr;
                if (PdhAddEnglishCounterW(query, path, 0, &h) == ERROR_SUCCESS) out.push_back(h);
            }
        };
        add(L"Dedicated Usage", dedicated);
        add(L"Shared Usage", shared);
        PdhCollectQueryData(query);
        return !dedicated.empty();
    }
    void Take(double& ded_mb, double& sh_mb) {
        ded_mb = sh_mb = -1;
        if (!query || PdhCollectQueryData(query) != ERROR_SUCCESS) return;
        auto sum = [](const std::vector<PDH_HCOUNTER>& v) {
            double total = 0;
            for (auto h : v) {
                PDH_FMT_COUNTERVALUE val{};
                if (PdhGetFormattedCounterValue(h, PDH_FMT_LARGE, nullptr, &val) == ERROR_SUCCESS)
                    total += static_cast<double>(val.largeValue);
            }
            return total / (1024.0 * 1024.0);
        };
        ded_mb = sum(dedicated);
        sh_mb = sum(shared);
    }
};

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

double NowMs() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}

struct Ctx {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IMFDXGIDeviceManager* mgr = nullptr;
    IDCompositionDevice* dcomp = nullptr;
    IDCompositionTarget* target = nullptr;
    IDXGIFactory2* factory = nullptr;
    UINT screen_w = 0, screen_h = 0;
    bool force_bgra = false;
    std::wstring dump_png;  // the 45th shown frame as DWM composed it (PrintWindow of the window)
    HWND hwnd = nullptr;
    int dump_frame = 45;
    int pool = -1;             // reader: decoder extra output samples (MF_SA_MINIMUM_OUTPUT_SAMPLE_COUNT), -1 = default
    bool decode_swap = false;  // reader: present decoder surfaces through IDXGIDecodeSwapChain (no copy, no buffers)
};

bool SavePngBgra(const std::wstring& path, UINT w, UINT h, const BYTE* bgra, UINT pitch);

// What DWM shows for our window (DirectComposition content included), as PNG.
bool CaptureWindowPng(HWND hwnd, const std::wstring& path) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int w = rc.right, h = rc.bottom;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    const bool ok = PrintWindow(hwnd, mem, PW_RENDERFULLCONTENT) &&
                    SavePngBgra(path, static_cast<UINT>(w), static_cast<UINT>(h), static_cast<const BYTE*>(bits),
                                static_cast<UINT>(w) * 4);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return ok;
}

bool SavePngBgra(const std::wstring& path, UINT w, UINT h, const BYTE* bgra, UINT pitch) {
    std::vector<BYTE> px(static_cast<size_t>(w) * h * 4);
    for (UINT y = 0; y < h; ++y) {
        BYTE* dst = px.data() + static_cast<size_t>(y) * w * 4;
        std::memcpy(dst, bgra + static_cast<size_t>(y) * pitch, static_cast<size_t>(w) * 4);
        for (UINT x = 0; x < w; ++x) dst[x * 4 + 3] = 255;
    }
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
           SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
           SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
           SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
           SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
           SUCCEEDED(frame->SetSize(w, h)) && SUCCEEDED(frame->SetPixelFormat(&fmt)) && fmt == GUID_WICPixelFormat32bppBGRA &&
           SUCCEEDED(frame->WritePixels(h, w * 4, static_cast<UINT>(px.size()), px.data())) &&
           SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
}


// Scales `visual` from a w x h surface to the screen (aspect fill, centred) and makes it the root.
bool ShowScaled(const Ctx& c, IDCompositionVisual* visual, UINT w, UINT h) {
    const float s = std::max(static_cast<float>(c.screen_w) / static_cast<float>(w),
                             static_cast<float>(c.screen_h) / static_cast<float>(h));
    D2D_MATRIX_3X2_F m{};
    m._11 = s;
    m._22 = s;
    m._31 = (static_cast<float>(c.screen_w) - static_cast<float>(w) * s) * 0.5f;
    m._32 = (static_cast<float>(c.screen_h) - static_cast<float>(h) * s) * 0.5f;
    visual->SetTransform(m);
    visual->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);
    return SUCCEEDED(c.target->SetRoot(visual)) && SUCCEEDED(c.dcomp->Commit());
}

struct Player {
    virtual ~Player() = default;
    virtual bool Open(const Ctx& c, const std::wstring& url) = 0;
    virtual void Tick() {}
    virtual void Pause() = 0;
    virtual void Close(const Ctx& c) = 0;
    virtual void Stats(double& shown, double& late) = 0;
    double first_frame_ms = -1;
    UINT video_w = 0, video_h = 0, surface_w = 0, surface_h = 0;
    std::string detail, error;
};

// ---------------------------------------------------------------------------------------------- engine

struct Notify final : IMFMediaEngineNotify {
    std::atomic<ULONG> refs{1};
    std::atomic<bool> metadata{false}, first_frame{false}, error{false};
    std::atomic<DWORD> error_code{0};
    std::atomic<HRESULT> error_hr{S_OK};
    STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEngineNotify)) {
            *out = static_cast<IMFMediaEngineNotify*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG r = --refs;
        if (r == 0) delete this;
        return r;
    }
    STDMETHODIMP EventNotify(DWORD event, DWORD_PTR p1, DWORD p2) override {
        if (event == MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA) metadata = true;
        else if (event == MF_MEDIA_ENGINE_EVENT_FIRSTFRAMEREADY) first_frame = true;
        else if (event == MF_MEDIA_ENGINE_EVENT_ERROR) {
            error_code = static_cast<DWORD>(p1);
            error_hr = static_cast<HRESULT>(p2);
            error = true;
        }
        return S_OK;
    }
};

void Pump(DWORD ms, Player* p = nullptr) {
    const double until = NowMs() + ms;
    while (NowMs() < until) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (p) p->Tick();
        Sleep(p ? 2 : 10);
    }
}

struct EnginePlayer final : Player {
    bool native = false;
    ComPtr<IMFMediaEngineEx> engine;
    ComPtr<IDCompositionVisual> visual;
    Notify* notify = nullptr;
    explicit EnginePlayer(bool native_size) : native(native_size) {}

    bool Open(const Ctx& c, const std::wstring& url) override {
        notify = new Notify();
        ComPtr<IMFMediaEngineClassFactory> factory;
        ComPtr<IMFAttributes> attrs;
        ComPtr<IMFMediaEngine> base;
        if (FAILED(CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
            FAILED(MFCreateAttributes(&attrs, 4)))
            return Fail("media engine factory");
        attrs->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, c.mgr);
        attrs->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify);
        attrs->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);
        if (FAILED(factory->CreateInstance(0, attrs.Get(), &base)) || FAILED(base.As(&engine))) return Fail("CreateInstance");
        if (FAILED(engine->EnableWindowlessSwapchainMode(TRUE))) return Fail("EnableWindowlessSwapchainMode");
        engine->SetMuted(TRUE);
        engine->SetLoop(TRUE);
        engine->SetAutoPlay(TRUE);
        const double t0 = NowMs();
        BSTR src = SysAllocString(url.c_str());
        const HRESULT hr = engine->SetSource(src);
        SysFreeString(src);
        if (FAILED(hr)) return Fail("SetSource");
        while (!notify->metadata && !notify->error && NowMs() - t0 < 15000) Pump(5);
        if (notify->error) return Fail("load");
        DWORD streams = 0;
        if (SUCCEEDED(engine->GetNumberOfStreams(&streams))) {  // a wallpaper never plays sound
            bool changed = false;
            for (DWORD i = 0; i < streams; ++i) {
                PROPVARIANT pv;
                PropVariantInit(&pv);
                if (SUCCEEDED(engine->GetStreamAttribute(i, MF_MT_MAJOR_TYPE, &pv)) && pv.vt == VT_CLSID && pv.puuid &&
                    *pv.puuid == MFMediaType_Audio) {
                    engine->SetStreamSelection(i, FALSE);
                    changed = true;
                }
                PropVariantClear(&pv);
            }
            if (changed) engine->ApplyStreamSelections();
        }
        engine->Play();
        while (!notify->first_frame && !notify->error && NowMs() - t0 < 20000) Pump(5);
        if (notify->error) return Fail("decode");
        if (!notify->first_frame) return Fail("no first frame in 20 s");
        first_frame_ms = NowMs() - t0;
        DWORD vw = 0, vh = 0;
        engine->GetNativeVideoSize(&vw, &vh);
        video_w = vw;
        video_h = vh;
        surface_w = native ? video_w : c.screen_w;
        surface_h = native ? video_h : c.screen_h;
        MFVideoNormalizedRect full{0.f, 0.f, 1.f, 1.f};
        const RECT dst{0, 0, static_cast<LONG>(surface_w), static_cast<LONG>(surface_h)};
        const MFARGB black{0, 0, 0, 255};
        engine->UpdateVideoStream(&full, &dst, &black);
        HANDLE handle = nullptr;
        for (int i = 0; i < 200 && !handle; ++i)
            if (FAILED(engine->GetVideoSwapchainHandle(&handle)) || !handle) Pump(10);
        if (!handle) return Fail("GetVideoSwapchainHandle");
        ComPtr<IUnknown> surf;
        if (FAILED(c.dcomp->CreateSurfaceFromHandle(handle, &surf)) || FAILED(c.dcomp->CreateVisual(&visual)) ||
            FAILED(visual->SetContent(surf.Get())) || !ShowScaled(c, visual.Get(), surface_w, surface_h))
            return Fail("DirectComposition");
        detail = native ? "engine swap chain at video size" : "engine swap chain at screen size";
        return true;
    }
    bool Fail(const char* what) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s (mf error %lu hr 0x%08lX)", what, notify ? notify->error_code.load() : 0ul,
                      notify ? static_cast<unsigned long>(notify->error_hr.load()) : 0ul);
        error = buf;
        return false;
    }
    void Pause() override {
        if (engine) engine->Pause();
    }
    void Close(const Ctx& c) override {
        c.target->SetRoot(nullptr);
        c.dcomp->Commit();
        visual.Reset();
        if (engine) engine->Shutdown();
        engine.Reset();
        if (notify) notify->Release();
        notify = nullptr;
    }
    void Stats(double& shown, double& late) override {
        shown = late = -1;
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (engine && SUCCEEDED(engine->GetStatistics(MF_MEDIA_ENGINE_STATISTIC_FRAMES_RENDERED, &pv)))
            shown = static_cast<double>(pv.vt == VT_UI8 ? pv.uhVal.QuadPart : pv.ulVal);
        PropVariantClear(&pv);
        if (engine && SUCCEEDED(engine->GetStatistics(MF_MEDIA_ENGINE_STATISTIC_FRAMES_DROPPED, &pv)))
            late = static_cast<double>(pv.vt == VT_UI8 ? pv.uhVal.QuadPart : pv.ulVal);
        PropVariantClear(&pv);
    }
};

// ---------------------------------------------------------------------------------------------- reader

struct ReaderPlayer final : Player {
    ComPtr<IMFSourceReader> reader;
    ComPtr<ID3D11VideoDevice> vdev;
    ComPtr<ID3D11VideoContext> vctx;
    ComPtr<ID3D11VideoProcessorEnumerator> venum;
    ComPtr<ID3D11VideoProcessor> vp;
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<IDCompositionVisual> visual;
    ComPtr<ID3D11Texture2D> upload;  // software-decoded frames land here
    std::map<std::pair<ID3D11Texture2D*, UINT>, ComPtr<ID3D11VideoProcessorInputView>> in_views;
    ComPtr<IMFSample> pending;
    LONGLONG pending_pts = 0, base_pts = 0;
    double base_ms = 0, open_ms = 0, frame_ms = 33.3;
    bool have_base = false, paused = false, sw_decode = false, first_shown = false;
    uint64_t shown = 0, late = 0;
    DXGI_FORMAT in_fmt = DXGI_FORMAT_NV12, out_fmt = DXGI_FORMAT_NV12;
    UINT coded_w = 0, coded_h = 0, pool_slices = 0;
    RECT src_rect{};
    const Ctx* c = nullptr;
    ComPtr<IMFMediaType> out_type;
    // --path mft: the reader only demuxes; the decoder MFT is ours, configured before it allocates anything.
    bool use_mft = false;
    // --path dxva: our own HEVC front end on D3D11 video decoding, pool sized from the stream's SPS.
    bool use_dxva = false, eos_flushing = false;
    hevc::Decoder hd;
    int pending_surface = -1;
    LONGLONG pts_offset = 0, max_raw_pts = 0;
    ComPtr<IMFTransform> mft;
    GUID out_sub = MFVideoFormat_NV12;
    // Zero-copy present: the decoder's texture array is the swap chain; the shown sample stays held.
    ComPtr<IDXGIDecodeSwapChain> dswap;
    HANDLE dsurface = nullptr;
    ID3D11Texture2D* dswap_tex = nullptr;
    ComPtr<IMFSample> showing;
    bool vp_ready = false, used_decode_swap = false, decode_swap_failed = false;
    std::map<std::pair<ID3D11Texture2D*, UINT>, bool> surfaces_seen;  // distinct decoder output surfaces = pool
    double surface_mb = 0;

    bool Fail(const char* what, HRESULT hr = S_OK) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s (hr 0x%08lX)", what, static_cast<unsigned long>(hr));
        error = buf;
        return false;
    }

    bool Open(const Ctx& ctx, const std::wstring& url) override {
        c = &ctx;
        open_ms = NowMs();
        ComPtr<IMFAttributes> attrs;
        MFCreateAttributes(&attrs, 3);
        if (!use_mft && !use_dxva) {
            attrs->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, ctx.mgr);
            attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        }
        HRESULT hr = MFCreateSourceReaderFromURL(url.c_str(), attrs.Get(), &reader);
        if (FAILED(hr)) return Fail("MFCreateSourceReaderFromURL", hr);
        reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);  // no audio at all
        reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
        if (use_mft) {
            if (!OpenMft()) return false;
            return FinishOpen(ctx);
        }
        if (use_dxva) {
            if (!OpenDxva()) return false;
            return FinishOpen(ctx);
        }
        // The decoder's own output: P010 for 10-bit sources, else NV12; nothing converts on the CPU.
        bool typed = false;
        for (const GUID& sub : {MFVideoFormat_P010, MFVideoFormat_NV12}) {
            ComPtr<IMFMediaType> t;
            MFCreateMediaType(&t);
            t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            t->SetGUID(MF_MT_SUBTYPE, sub);
            if (SUCCEEDED(reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, t.Get()))) {
                in_fmt = sub == MFVideoFormat_P010 ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
                out_type = t;
                typed = true;
                break;
            }
        }
        if (!typed) return Fail("no NV12/P010 decoder output");
        if (ctx.pool >= 0) TrimPool(static_cast<UINT32>(ctx.pool));
        return FinishOpen(ctx);
    }

    // Decoder MFT on our terms: D3D manager, spare-sample count and displayable surfaces set before the
    // output type, so its very first allocation already follows them.
    bool OpenMft() {
        ComPtr<IMFMediaType> native;
        HRESULT hr = reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &native);
        if (FAILED(hr)) return Fail("GetNativeMediaType", hr);
        reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, native.Get());
        GUID sub{};
        native->GetGUID(MF_MT_SUBTYPE, &sub);
        MFT_REGISTER_TYPE_INFO in_info{MFMediaType_Video, sub};
        IMFActivate** acts = nullptr;
        UINT32 count = 0;
        hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                       &in_info, nullptr, &acts, &count);
        if (FAILED(hr) || count == 0) return Fail("no decoder MFT for this codec", hr);
        for (UINT32 i = 0; i < count && !mft; ++i) {
            ComPtr<IMFTransform> t;
            if (FAILED(acts[i]->ActivateObject(IID_PPV_ARGS(&t)))) continue;
            ComPtr<IMFAttributes> ta;
            UINT32 aware = 0;
            if (SUCCEEDED(t->GetAttributes(&ta)) && ta && SUCCEEDED(ta->GetUINT32(MF_SA_D3D11_AWARE, &aware)) && aware) mft = t;
        }
        for (UINT32 i = 0; i < count; ++i) acts[i]->Release();
        CoTaskMemFree(acts);
        if (!mft) return Fail("no D3D11-aware decoder MFT");
        hr = mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(c->mgr));
        if (FAILED(hr)) return Fail("MFT_MESSAGE_SET_D3D_MANAGER", hr);
        ComPtr<IMFAttributes> oa;
        if (SUCCEEDED(mft->GetOutputStreamAttributes(0, &oa)) && oa) {
            if (c->pool >= 0) {
                oa->SetUINT32(MF_SA_MINIMUM_OUTPUT_SAMPLE_COUNT, static_cast<UINT32>(c->pool));
                oa->SetUINT32(MF_SA_MINIMUM_OUTPUT_SAMPLE_COUNT_PROGRESSIVE, static_cast<UINT32>(c->pool));
            }
            if (c->decode_swap) oa->SetUINT32(MF_SA_D3D11_ALLOCATE_DISPLAYABLE_RESOURCES, TRUE);
        }
        hr = mft->SetInputType(0, native.Get(), 0);
        if (FAILED(hr)) return Fail("decoder SetInputType", hr);
        if (!PickMftOutput()) return false;
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return true;
    }

    bool PickMftOutput() {
        for (const GUID& want : {MFVideoFormat_P010, MFVideoFormat_NV12}) {
            for (DWORD i = 0;; ++i) {
                ComPtr<IMFMediaType> t;
                if (FAILED(mft->GetOutputAvailableType(0, i, &t))) break;
                GUID sub{};
                t->GetGUID(MF_MT_SUBTYPE, &sub);
                if (sub != want || FAILED(mft->SetOutputType(0, t.Get(), 0))) continue;
                out_sub = sub;
                in_fmt = sub == MFVideoFormat_P010 ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
                out_type = t;
                return true;
            }
        }
        return Fail("decoder offers no NV12/P010 output");
    }

    // One decoded frame from our MFT (feeding it compressed samples as it asks); false when none yet.
    bool NextDecoded(ComPtr<IMFSample>& out, LONGLONG& ts) {
        for (int guard = 0; guard < 64; ++guard) {
            MFT_OUTPUT_DATA_BUFFER ob{};
            DWORD status = 0;
            HRESULT hr = mft->ProcessOutput(0, 1, &ob, &status);
            if (ob.pEvents) ob.pEvents->Release();
            if (SUCCEEDED(hr) && ob.pSample) {
                out.Attach(ob.pSample);
                out->GetSampleTime(&ts);
                return true;
            }
            if (ob.pSample) ob.pSample->Release();
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                if (!PickMftOutput()) return false;
                continue;
            }
            if (hr != MF_E_TRANSFORM_NEED_MORE_INPUT) {
                Fail("decoder ProcessOutput", hr);
                return false;
            }
            DWORD idx = 0, flags = 0;
            LONGLONG its = 0;
            ComPtr<IMFSample> in;
            hr = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &idx, &flags, &its, &in);
            if (FAILED(hr)) {
                Fail("ReadSample", hr);
                return false;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {  // loop: drop what the decoder still holds, restart
                mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
                PROPVARIANT pv;
                PropVariantInit(&pv);
                pv.vt = VT_I8;
                pv.hVal.QuadPart = 0;
                reader->SetCurrentPosition(GUID_NULL, pv);
                have_base = false;
                return false;
            }
            if (in) mft->ProcessInput(0, in.Get(), 0);
        }
        return false;
    }

    bool OpenDxva() {
        ComPtr<IMFMediaType> native;
        HRESULT hr = reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &native);
        if (FAILED(hr)) return Fail("GetNativeMediaType", hr);
        GUID sub{};
        native->GetGUID(MF_MT_SUBTYPE, &sub);
        if (sub != MFVideoFormat_HEVC && sub != MFVideoFormat_HEVC_ES) return Fail("dxva path is HEVC only");
        reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, native.Get());
        std::string err;
        if (!hd.Init(c->dev, c->ctx, c->pool >= 0 ? c->pool : 1, err)) return Fail(err.c_str());
        UINT32 blob = 0;
        if (SUCCEEDED(native->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &blob)) && blob) {
            std::vector<UINT8> cfg(blob);
            if (SUCCEEDED(native->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, cfg.data(), blob, nullptr))) hd.ParseConfig(cfg.data(), cfg.size());
        }
        out_type = native;
        return true;
    }

    // Next frame in display order from our decoder (reads and decodes access units as needed; loops at the end).
    bool NextDxva() {
        for (int guard = 0; guard < 32; ++guard) {
            hevc::Frame f;
            if (hd.PopOutput(f, eos_flushing)) {
                pending_surface = f.surface;
                if (getenv("PULSE_PROBE_DEBUG") && (hd.Decoded() < 12 || ((f.pts + pts_offset) > 96000000 && (f.pts + pts_offset) < 106000000))) std::printf("out poc %d pts %.1f ms at %.1f ms\n", f.poc, (f.pts + pts_offset) / 10000.0, NowMs() - open_ms);
                pending_pts = f.pts + pts_offset;
                return true;
            }
            if (eos_flushing) {  // everything shown: start over, timestamps continue after the last frame
                eos_flushing = false;
                PROPVARIANT pv;
                PropVariantInit(&pv);
                pv.vt = VT_I8;
                pv.hVal.QuadPart = 0;
                reader->SetCurrentPosition(GUID_NULL, pv);
                hd.Reset();
                pts_offset += max_raw_pts + static_cast<LONGLONG>(frame_ms * 10000.0);
                max_raw_pts = 0;
                continue;
            }
            DWORD idx = 0, flags = 0;
            LONGLONG ts = 0;
            ComPtr<IMFSample> smp;
            const HRESULT hr = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &idx, &flags, &ts, &smp);
            if (FAILED(hr)) {
                Fail("ReadSample", hr);
                return false;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                eos_flushing = true;
                if (getenv("PULSE_PROBE_DEBUG")) std::printf("EOS at %.1f ms\n", NowMs() - open_ms);
                continue;
            }
            if (!smp) continue;
            ComPtr<IMFMediaBuffer> buf;
            BYTE* data = nullptr;
            DWORD len = 0;
            if (FAILED(smp->ConvertToContiguousBuffer(&buf)) || FAILED(buf->Lock(&data, nullptr, &len))) {
                Fail("sample buffer");
                return false;
            }
            std::string err;
            const bool ok = hd.DecodeAccessUnit(data, len, ts, err);
            buf->Unlock();
            if (!ok) {
                Fail(("hevc: " + err).c_str());
                return false;
            }
            max_raw_pts = std::max(max_raw_pts, ts);
        }
        return false;
    }

    bool PresentDxva() {
        if (!hd.Ready()) return false;
        if (!vp_ready) {  // the decoder's real surface size and crop are known only now
            coded_w = hd.CodedWidth();
            coded_h = hd.CodedHeight();
            src_rect = hd.Crop();
            in_fmt = hd.Format();
            video_w = static_cast<UINT>(src_rect.right - src_rect.left);
            video_h = static_cast<UINT>(src_rect.bottom - src_rect.top);
            const double fit = std::min({1.0, static_cast<double>(c->screen_w) / video_w, static_cast<double>(c->screen_h) / video_h});
            surface_w = std::max(2u, static_cast<UINT>(video_w * fit) & ~1u);
            surface_h = std::max(2u, static_cast<UINT>(video_h * fit) & ~1u);
        }
        ComPtr<ID3D11Texture2D> tex = hd.Texture();
        return PresentTexture(tex, static_cast<UINT>(pending_surface), nullptr);
    }

    bool FinishOpen(const Ctx& ctx) {
        HRESULT hr = S_OK;
        (void)hr;
        ComPtr<IMFMediaType> cur;
        if (use_mft || use_dxva) cur = out_type;
        else reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &cur);
        MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &coded_w, &coded_h);
        video_w = coded_w;
        video_h = coded_h;
        MFVideoArea area{};
        if (SUCCEEDED(cur->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area), nullptr))) {
            video_w = static_cast<UINT>(area.Area.cx);
            video_h = static_cast<UINT>(area.Area.cy);
        }
        src_rect = {0, 0, static_cast<LONG>(video_w), static_cast<LONG>(video_h)};
        UINT32 num = 0, den = 0;
        if (SUCCEEDED(MFGetAttributeRatio(cur.Get(), MF_MT_FRAME_RATE, &num, &den)) && num && den)
            frame_ms = 1000.0 * den / num;

        // Never larger than the screen: a bigger video is scaled down once, here, by the video processor.
        const double fit = std::min({1.0, static_cast<double>(ctx.screen_w) / video_w, static_cast<double>(ctx.screen_h) / video_h});
        surface_w = std::max(2u, static_cast<UINT>(video_w * fit) & ~1u);
        surface_h = std::max(2u, static_cast<UINT>(video_h * fit) & ~1u);

        if (!ctx.decode_swap && !use_dxva && !EnsureVp()) return false;

        // First frame (pumping, since the decoder may need a few samples).
        while (!first_shown && error.empty() && NowMs() - open_ms < 20000) Pump(1, this);
        if (!error.empty()) return false;
        if (!first_shown) return Fail("no first frame in 20 s");
        return true;
    }

    // Ask the decoder for fewer spare output surfaces, then renegotiate so it reallocates its pool.
    void TrimPool(UINT32 extra) {
        ComPtr<IMFSourceReaderEx> rex;
        if (FAILED(reader.As(&rex))) return;
        for (DWORD i = 0;; ++i) {
            GUID cat{};
            ComPtr<IMFTransform> t;
            if (FAILED(rex->GetTransformForStream(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), i, &cat, &t))) break;
            if (cat != MFT_CATEGORY_VIDEO_DECODER) continue;
            ComPtr<IMFAttributes> oa;
            if (SUCCEEDED(t->GetOutputStreamAttributes(0, &oa)) && oa) {
                oa->SetUINT32(MF_SA_MINIMUM_OUTPUT_SAMPLE_COUNT, extra);
                oa->SetUINT32(MF_SA_MINIMUM_OUTPUT_SAMPLE_COUNT_PROGRESSIVE, extra);
            }
        }
        reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, out_type.Get());
    }

    bool EnsureVp() {
        if (vp_ready) return true;
        const Ctx& ctx = *c;
        HRESULT hr = S_OK;
        ctx.dev->QueryInterface(IID_PPV_ARGS(&vdev));
        ctx.ctx->QueryInterface(IID_PPV_ARGS(&vctx));
        if (!vdev || !vctx) return Fail("no D3D11 video device");
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
        cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        cd.InputWidth = coded_w;
        cd.InputHeight = coded_h;
        cd.OutputWidth = surface_w;
        cd.OutputHeight = surface_h;
        cd.InputFrameRate = {30, 1};
        cd.OutputFrameRate = {30, 1};
        cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
        hr = vdev->CreateVideoProcessorEnumerator(&cd, &venum);
        if (FAILED(hr)) return Fail("CreateVideoProcessorEnumerator", hr);
        hr = vdev->CreateVideoProcessor(venum.Get(), 0, &vp);
        if (FAILED(hr)) return Fail("CreateVideoProcessor", hr);

        // Two buffers, flip model, for composition: NV12 when the processor and swap chain take it.
        auto make_swap = [&](DXGI_FORMAT f) {
            UINT support = 0;
            if (FAILED(venum->CheckVideoProcessorFormat(f, &support)) ||
                !(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT))
                return false;
            DXGI_SWAP_CHAIN_DESC1 sd{};
            sd.Width = surface_w;
            sd.Height = surface_h;
            sd.Format = f;
            sd.SampleDesc.Count = 1;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.BufferCount = 2;
            sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            swap.Reset();
            if (FAILED(ctx.factory->CreateSwapChainForComposition(ctx.dev, &sd, nullptr, &swap))) return false;
            out_fmt = f;
            return true;
        };
        if (!(ctx.force_bgra ? false : make_swap(DXGI_FORMAT_NV12)) && !make_swap(DXGI_FORMAT_B8G8R8A8_UNORM))
            return Fail("composition swap chain");

        ComPtr<ID3D11VideoContext1> vctx1;
        vctx.As(&vctx1);
        const DXGI_COLOR_SPACE_TYPE in_cs = DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
        const DXGI_COLOR_SPACE_TYPE out_cs = out_fmt == DXGI_FORMAT_NV12 ? DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709
                                                                         : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        if (vctx1) {
            vctx1->VideoProcessorSetStreamColorSpace1(vp.Get(), 0, in_cs);
            vctx1->VideoProcessorSetOutputColorSpace1(vp.Get(), out_cs);
        }
        vctx->VideoProcessorSetStreamSourceRect(vp.Get(), 0, TRUE, &src_rect);
        const RECT out_rect{0, 0, static_cast<LONG>(surface_w), static_cast<LONG>(surface_h)};
        vctx->VideoProcessorSetStreamDestRect(vp.Get(), 0, TRUE, &out_rect);
        vctx->VideoProcessorSetOutputTargetRect(vp.Get(), TRUE, &out_rect);
        vctx->VideoProcessorSetStreamAutoProcessingMode(vp.Get(), 0, FALSE);

        visual.Reset();
        if (FAILED(ctx.dcomp->CreateVisual(&visual)) || FAILED(visual->SetContent(swap.Get())) ||
            !ShowScaled(ctx, visual.Get(), surface_w, surface_h))
            return Fail("DirectComposition");
        vp_ready = true;
        return true;
    }

    // IDXGIDecodeSwapChain over the decoder's own texture array: DWM reads the decoded NV12/P010 surface.
    bool EnsureDecodeSwap(ID3D11Texture2D* tex) {
        if (dswap && dswap_tex == tex) return true;
        dswap.Reset();
        if (dsurface) CloseHandle(dsurface);
        dsurface = nullptr;
        ComPtr<IDXGIFactoryMedia> fm;
        if (FAILED(c->factory->QueryInterface(IID_PPV_ARGS(&fm)))) return false;
        if (FAILED(DCompositionCreateSurfaceHandle(COMPOSITIONOBJECT_ALL_ACCESS, nullptr, &dsurface))) return false;
        ComPtr<IDXGIResource> res;
        tex->QueryInterface(IID_PPV_ARGS(&res));
        DXGI_DECODE_SWAP_CHAIN_DESC dd{};
        HRESULT hr = fm->CreateDecodeSwapChainForCompositionSurfaceHandle(c->dev, dsurface, &dd, res.Get(), nullptr, &dswap);
        if (FAILED(hr)) {
            std::printf("decode swap chain unavailable (hr 0x%08lX), using the video processor\n", static_cast<unsigned long>(hr));
            return false;
        }
        const RECT target{0, 0, static_cast<LONG>(surface_w), static_cast<LONG>(surface_h)};
        dswap->SetSourceRect(&src_rect);
        dswap->SetTargetRect(&target);
        dswap->SetDestSize(surface_w, surface_h);
        dswap->SetColorSpace(DXGI_MULTIPLANE_OVERLAY_YCbCr_FLAG_BT709);
        ComPtr<IUnknown> surf;
        visual.Reset();
        if (FAILED(c->dcomp->CreateSurfaceFromHandle(dsurface, &surf)) || FAILED(c->dcomp->CreateVisual(&visual)) ||
            FAILED(visual->SetContent(surf.Get())) || !ShowScaled(*c, visual.Get(), surface_w, surface_h))
            return false;
        dswap_tex = tex;
        return true;
    }

    ID3D11VideoProcessorInputView* InputView(ID3D11Texture2D* tex, UINT slice) {
        auto key = std::make_pair(tex, slice);
        if (auto it = in_views.find(key); it != in_views.end()) return it->second.Get();
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC d{};
        d.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        d.Texture2D.ArraySlice = slice;
        ComPtr<ID3D11VideoProcessorInputView> v;
        if (FAILED(vdev->CreateVideoProcessorInputView(tex, venum.Get(), &d, &v))) return nullptr;
        in_views[key] = v;
        return v.Get();
    }

    // A software decoder hands over system memory: copy it into one reusable GPU texture.
    bool Upload(IMFMediaBuffer* buf, ComPtr<ID3D11Texture2D>& tex) {
        if (!upload) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = coded_w;
            td.Height = coded_h;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = in_fmt;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(c->dev->CreateTexture2D(&td, nullptr, &upload))) return false;
        }
        ComPtr<IMF2DBuffer> b2;
        BYTE* data = nullptr;
        LONG pitch = 0;
        if (SUCCEEDED(buf->QueryInterface(IID_PPV_ARGS(&b2))) && SUCCEEDED(b2->Lock2D(&data, &pitch))) {
            c->ctx->UpdateSubresource(upload.Get(), 0, nullptr, data, static_cast<UINT>(pitch), 0);
            b2->Unlock2D();
        } else {
            DWORD len = 0;
            if (FAILED(buf->Lock(&data, nullptr, &len))) return false;
            const UINT bpp = in_fmt == DXGI_FORMAT_P010 ? 2 : 1;
            c->ctx->UpdateSubresource(upload.Get(), 0, nullptr, data, coded_w * bpp, 0);
            buf->Unlock();
        }
        tex = upload;
        return true;
    }

    bool Present(IMFSample* sample) {
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(sample->GetBufferByIndex(0, &buf))) return false;
        ComPtr<ID3D11Texture2D> tex;
        UINT slice = 0;
        ComPtr<IMFDXGIBuffer> dxgi;
        if (SUCCEEDED(buf.As(&dxgi)) && SUCCEEDED(dxgi->GetResource(IID_PPV_ARGS(&tex)))) {
            dxgi->GetSubresourceIndex(&slice);
        } else {
            sw_decode = true;
            if (!Upload(buf.Get(), tex)) return false;
        }
        return PresentTexture(tex, slice, sample);
    }

    bool PresentTexture(ComPtr<ID3D11Texture2D> tex, UINT slice, IMFSample* sample) {
        if (shown == 0) {
            D3D11_TEXTURE2D_DESC td{};
            tex->GetDesc(&td);
            std::printf("decoded surface: %ux%u format %d array %u slice %u bind 0x%X misc 0x%X\n", td.Width, td.Height,
                        static_cast<int>(td.Format), td.ArraySize, slice, td.BindFlags, td.MiscFlags);
        }
        {
            D3D11_TEXTURE2D_DESC td{};
            tex->GetDesc(&td);
            pool_slices = std::max(pool_slices, td.ArraySize);
            if (!sw_decode && !surfaces_seen.count({tex.Get(), slice})) {
                surfaces_seen[{tex.Get(), slice}] = true;
                surface_mb = static_cast<double>(td.Width) * td.Height * (td.Format == DXGI_FORMAT_P010 ? 3.0 : 1.5) / (1024.0 * 1024.0);
            }
            if (c->decode_swap && !decode_swap_failed && !sw_decode && (td.BindFlags & D3D11_BIND_DECODER)) {
                if (!EnsureDecodeSwap(tex.Get())) decode_swap_failed = true;
            }
            if (dswap && dswap_tex == tex.Get()) {
                if (FAILED(dswap->PresentBuffer(slice, 0, 0))) return false;
                showing = sample;  // DWM reads this surface until the next frame replaces it
                used_decode_swap = true;
                if (shown == static_cast<uint64_t>(c->dump_frame) && !c->dump_png.empty()) CaptureWindowPng(c->hwnd, c->dump_png);
                return true;
            }
        }
        if (!EnsureVp()) return false;
        ID3D11VideoProcessorInputView* iv = InputView(tex.Get(), slice);
        ComPtr<ID3D11Texture2D> back;
        if (!iv || FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC od{};
        od.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11VideoProcessorOutputView> ov;
        if (FAILED(vdev->CreateVideoProcessorOutputView(back.Get(), venum.Get(), &od, &ov))) return false;
        D3D11_VIDEO_PROCESSOR_STREAM st{};
        st.Enable = TRUE;
        st.pInputSurface = iv;
        if (FAILED(vctx->VideoProcessorBlt(vp.Get(), ov.Get(), 0, 1, &st))) return false;
        const bool ok = SUCCEEDED(swap->Present(0, 0));
        if (shown == static_cast<uint64_t>(c->dump_frame) && !c->dump_png.empty()) CaptureWindowPng(c->hwnd, c->dump_png);
        return ok;
    }

    void Tick() override {
        if (paused || !reader || !error.empty()) return;
        if (!pending && pending_surface < 0 && use_dxva && !NextDxva()) return;
        if (!pending && use_mft) {
            ComPtr<IMFSample> s;
            LONGLONG ts = 0;
            if (!NextDecoded(s, ts)) return;
            pending = s;
            pending_pts = ts;
        }
        if (!pending && pending_surface < 0) {
            DWORD idx = 0, flags = 0;
            LONGLONG ts = 0;
            ComPtr<IMFSample> s;
            const HRESULT hr = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &idx, &flags, &ts, &s);
            if (FAILED(hr)) {
                Fail("ReadSample", hr);
                return;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {  // loop
                PROPVARIANT pv;
                PropVariantInit(&pv);
                pv.vt = VT_I8;
                pv.hVal.QuadPart = 0;
                reader->SetCurrentPosition(GUID_NULL, pv);
                have_base = false;
                return;
            }
            if (!s) return;
            pending = s;
            pending_pts = ts;
        }
        const double now = NowMs();
        if (!have_base) {
            base_pts = pending_pts;
            base_ms = now;
            have_base = true;
        }
        const double due = base_ms + static_cast<double>(pending_pts - base_pts) / 10000.0;
        if (now < due) return;
        if (first_shown && now - due > 2 * frame_ms) {
            ++late;  // behind by two frames: drop this one to catch up
        } else if (pending_surface >= 0 ? PresentDxva() : Present(pending.Get())) {
            ++shown;
            if (!first_shown) {
                first_shown = true;
                first_frame_ms = now - open_ms;
            }
        } else if (error.empty()) {
            Fail("present");
        }
        pending.Reset();
        if (pending_surface >= 0) hd.Release(pending_surface);  // shown (copied to the swap chain) or dropped
        pending_surface = -1;
    }

    void Pause() override { paused = true; }
    void Close(const Ctx& ctx) override {
        ctx.target->SetRoot(nullptr);
        ctx.dcomp->Commit();
        pending.Reset();
        showing.Reset();
        dswap.Reset();
        if (dsurface) CloseHandle(dsurface);
        dsurface = nullptr;
        in_views.clear();
        visual.Reset();
        swap.Reset();
        upload.Reset();
        vp.Reset();
        venum.Reset();
        if (mft) {
            mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
            mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
        }
        mft.Reset();
        hd.Close();
        pending_surface = -1;
        reader.Reset();
        ctx.ctx->ClearState();
        ctx.ctx->Flush();
    }
    void Stats(double& s, double& l) override {
        s = static_cast<double>(shown);
        l = static_cast<double>(late);
        char buf[200];
        if (use_dxva) {
            std::snprintf(buf, sizeof(buf), "own HEVC DXVA decoder, %s, %d surfaces (DPB %d + %d) x %.1f MB, %llu decoded %llu skipped %llu busy-waits, %s 2-buffer swap chain %ux%u",
                          in_fmt == DXGI_FORMAT_P010 ? "P010" : "NV12", hd.Surfaces(), hd.DpbSize(), hd.Surfaces() - hd.DpbSize(),
                          static_cast<double>(hd.CodedWidth()) * hd.CodedHeight() * (in_fmt == DXGI_FORMAT_P010 ? 3.0 : 1.5) / (1024.0 * 1024.0),
                          static_cast<unsigned long long>(hd.Decoded()), static_cast<unsigned long long>(hd.Skipped()), static_cast<unsigned long long>(hd.PendingWaits()),
                          out_fmt == DXGI_FORMAT_NV12 ? "NV12" : "BGRA", surface_w, surface_h);
            detail = buf;
            return;
        }
        if (used_decode_swap)
            std::snprintf(buf, sizeof(buf), "reader %s decode, %s, %zu decoder surfaces x %.1f MB (array %u), decode swap chain (zero copy) %ux%u",
                          sw_decode ? "SOFTWARE" : "hardware", in_fmt == DXGI_FORMAT_P010 ? "P010" : "NV12", surfaces_seen.size(),
                          surface_mb, pool_slices, surface_w, surface_h);
        else
            std::snprintf(buf, sizeof(buf), "reader %s decode, %s, %zu decoder surfaces x %.1f MB (array %u), %s 2-buffer swap chain %ux%u",
                          sw_decode ? "SOFTWARE" : "hardware", in_fmt == DXGI_FORMAT_P010 ? "P010" : "NV12", surfaces_seen.size(),
                          surface_mb, pool_slices, out_fmt == DXGI_FORMAT_NV12 ? "NV12" : "BGRA", surface_w, surface_h);
        detail = buf;
    }
};

std::string U8(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(std::max(0, n - 1)), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

// ------------------------------------------------------------------------------------------- transcode
// A video bigger than the screen, once, to the screen's size: hardware decode -> one VideoProcessor scale
// (aspect kept, nothing cropped) -> hardware encoder through the Sink Writer -> MP4. HEVC (Main10 for 10-bit
// sources, so no precision is dropped), H.264 when there is no HEVC encoder. No audio (a wallpaper is silent).
int Transcode(const Ctx& c, Meter& meter, const std::wstring& in, const std::wstring& out, UINT tw, UINT th) {
    const double t0 = NowMs();
    ComPtr<IMFAttributes> ra;
    MFCreateAttributes(&ra, 2);
    if (!getenv("PULSE_READER_SOFTWARE")) ra->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, c.mgr);  // else decode to system memory
    ra->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(in.c_str(), ra.Get(), &reader))) {
        std::printf("TRANSCODE FAIL open\n");
        return 6;
    }
    const DWORD vs = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    reader->SetStreamSelection(vs, TRUE);
    DXGI_FORMAT fmt = DXGI_FORMAT_NV12;
    GUID sub_in = MFVideoFormat_NV12;
    bool typed = false;
    for (const GUID& sub : {MFVideoFormat_P010, MFVideoFormat_NV12}) {
        ComPtr<IMFMediaType> t;
        MFCreateMediaType(&t);
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        t->SetGUID(MF_MT_SUBTYPE, sub);
        if (SUCCEEDED(reader->SetCurrentMediaType(vs, nullptr, t.Get()))) {
            fmt = sub == MFVideoFormat_P010 ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
            sub_in = sub;
            typed = true;
            break;
        }
    }
    if (!typed) {
        std::printf("TRANSCODE FAIL no NV12/P010 decode\n");
        return 6;
    }
    ComPtr<IMFMediaType> cur;
    reader->GetCurrentMediaType(vs, &cur);
    // A software decoder keeps frames per worker thread; PULSE_DEC_THREADS caps them (memory vs speed).
    if (const char* th_env = getenv("PULSE_DEC_THREADS")) {
        ComPtr<IMFSourceReaderEx> rex;
        if (SUCCEEDED(reader.As(&rex)))
            for (DWORD i = 0;; ++i) {
                GUID cat{};
                ComPtr<IMFTransform> t;
                if (FAILED(rex->GetTransformForStream(vs, i, &cat, &t))) break;
                ComPtr<ICodecAPI> api;
                if (cat == MFT_CATEGORY_VIDEO_DECODER && SUCCEEDED(t.As(&api))) {
                    VARIANT v;
                    VariantInit(&v);
                    v.vt = VT_UI4;
                    v.ulVal = static_cast<ULONG>(atoi(th_env));
                    std::printf("decoder worker threads -> %lu: hr 0x%08lX\n", v.ulVal,
                                static_cast<unsigned long>(api->SetValue(&CODECAPI_AVDecNumWorkerThreads, &v)));
                }
            }
    }
    UINT32 cw = 0, ch = 0, num = 30, den = 1;
    MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &cw, &ch);
    MFGetAttributeRatio(cur.Get(), MF_MT_FRAME_RATE, &num, &den);
    UINT vw = cw, vh = ch;
    MFVideoArea area{};
    if (SUCCEEDED(cur->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area), nullptr))) {
        vw = static_cast<UINT>(area.Area.cx);
        vh = static_cast<UINT>(area.Area.cy);
    }
    const double fit = std::min({1.0, static_cast<double>(tw) / vw, static_cast<double>(th) / vh});
    const UINT ow = std::max(2u, static_cast<UINT>(vw * fit + 0.5) & ~1u), oh = std::max(2u, static_cast<UINT>(vh * fit + 0.5) & ~1u);
    if (fit >= 1.0) {
        std::printf("TRANSCODE SKIP %ux%u already fits %ux%u\n", vw, vh, tw, th);
        return 0;
    }
    const double fps = static_cast<double>(num) / std::max(1u, den);

    // Scaler.
    ComPtr<ID3D11VideoDevice> vdev;
    ComPtr<ID3D11VideoContext> vctx;
    c.dev->QueryInterface(IID_PPV_ARGS(&vdev));
    c.ctx->QueryInterface(IID_PPV_ARGS(&vctx));
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputWidth = cw;
    cd.InputHeight = ch;
    cd.OutputWidth = ow;
    cd.OutputHeight = oh;
    cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_QUALITY;
    ComPtr<ID3D11VideoProcessorEnumerator> venum;
    ComPtr<ID3D11VideoProcessor> vp;
    UINT support = 0;
    if (!vdev || FAILED(vdev->CreateVideoProcessorEnumerator(&cd, &venum)) || FAILED(vdev->CreateVideoProcessor(venum.Get(), 0, &vp)) ||
        FAILED(venum->CheckVideoProcessorFormat(fmt, &support)) || !(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
        std::printf("TRANSCODE FAIL video processor (%s out)\n", fmt == DXGI_FORMAT_P010 ? "P010" : "NV12");
        return 6;
    }
    const RECT src{0, 0, static_cast<LONG>(vw), static_cast<LONG>(vh)}, dst{0, 0, static_cast<LONG>(ow), static_cast<LONG>(oh)};
    vctx->VideoProcessorSetStreamSourceRect(vp.Get(), 0, TRUE, &src);
    vctx->VideoProcessorSetStreamDestRect(vp.Get(), 0, TRUE, &dst);
    vctx->VideoProcessorSetOutputTargetRect(vp.Get(), TRUE, &dst);
    vctx->VideoProcessorSetStreamAutoProcessingMode(vp.Get(), 0, FALSE);
    ComPtr<ID3D11VideoContext1> vctx1;
    if (SUCCEEDED(vctx.As(&vctx1))) {
        vctx1->VideoProcessorSetStreamColorSpace1(vp.Get(), 0, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
        vctx1->VideoProcessorSetOutputColorSpace1(vp.Get(), DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
    }

    // Encoder.
    const bool ten = fmt == DXGI_FORMAT_P010;
    ComPtr<IMFAttributes> wa;
    MFCreateAttributes(&wa, 3);
    wa->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, c.mgr);
    wa->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    DeleteFileW(out.c_str());
    ComPtr<IMFSinkWriter> writer;
    DWORD stream = 0;
    const char* codec = "";
    const UINT32 bitrate = static_cast<UINT32>(static_cast<double>(ow) * oh * fps * 0.10);  // ~25 Mbps at 4K30
    auto try_codec = [&](const GUID& out_sub, UINT32 profile) {
        writer.Reset();
        if (FAILED(MFCreateSinkWriterFromURL(out.c_str(), nullptr, wa.Get(), &writer))) return false;
        ComPtr<IMFMediaType> ot, it;
        MFCreateMediaType(&ot);
        ot->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        ot->SetGUID(MF_MT_SUBTYPE, out_sub);
        ot->SetUINT32(MF_MT_AVG_BITRATE, out_sub == MFVideoFormat_H264 ? bitrate * 3 / 2 : bitrate);
        ot->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(ot.Get(), MF_MT_FRAME_SIZE, ow, oh);
        MFSetAttributeRatio(ot.Get(), MF_MT_FRAME_RATE, num, den);
        MFSetAttributeRatio(ot.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (profile) ot->SetUINT32(MF_MT_VIDEO_PROFILE, profile);
        if (FAILED(writer->AddStream(ot.Get(), &stream))) return false;
        MFCreateMediaType(&it);
        it->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        it->SetGUID(MF_MT_SUBTYPE, sub_in);
        it->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(it.Get(), MF_MT_FRAME_SIZE, ow, oh);
        MFSetAttributeRatio(it.Get(), MF_MT_FRAME_RATE, num, den);
        MFSetAttributeRatio(it.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (FAILED(writer->SetInputMediaType(stream, it.Get(), nullptr))) return false;
        return SUCCEEDED(writer->BeginWriting());
    };
    if (ten && try_codec(MFVideoFormat_HEVC, eAVEncH265VProfile_Main_420_10)) codec = "HEVC Main10";
    else if (!ten && try_codec(MFVideoFormat_HEVC, eAVEncH265VProfile_Main_420_8)) codec = "HEVC Main";
    else if (!ten && try_codec(MFVideoFormat_H264, eAVEncH264VProfile_High)) codec = "H.264 High";
    else {
        std::printf("TRANSCODE FAIL no hardware encoder accepted %ux%u %s\n", ow, oh, ten ? "P010" : "NV12");
        return 6;
    }
    // Which encoder MFT the writer picked (hardware or not).
    std::string encoder_name = "?";
    {
        ComPtr<IMFSinkWriterEx> wex;
        GUID cat{};
        ComPtr<IMFTransform> enc;
        if (SUCCEEDED(writer.As(&wex)) && SUCCEEDED(wex->GetTransformForStream(stream, 0, &cat, &enc))) {
            ComPtr<IMFAttributes> ea;
            wchar_t nm[128]{};
            UINT32 len = 0;
            if (SUCCEEDED(enc->GetAttributes(&ea)) && ea && SUCCEEDED(ea->GetString(MFT_FRIENDLY_NAME_Attribute, nm, 128, &len)))
                encoder_name = U8(nm);
        }
    }
    std::printf("transcode: %ux%u -> %ux%u, %s at %.1f Mbps, encoder: %s\n", vw, vh, ow, oh, codec, bitrate / 1e6,
                encoder_name.c_str());

    meter.Take();
    double peak_local = 0, peak_ws = 0, cpu_sum = 0;
    int cpu_n = 0;
    double last_sample = NowMs();
    uint64_t frames = 0;
    LONGLONG first_ts = -1;
    ComPtr<ID3D11Texture2D> upload;
    std::map<std::pair<ID3D11Texture2D*, UINT>, ComPtr<ID3D11VideoProcessorInputView>> views;
    bool sw_decode = false;
    for (;;) {
        DWORD idx = 0, flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> smp;
        if (FAILED(reader->ReadSample(vs, 0, &idx, &flags, &ts, &smp))) {
            std::printf("TRANSCODE FAIL read\n");
            return 6;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (!smp) continue;
        if (first_ts < 0) first_ts = ts;
        ComPtr<IMFMediaBuffer> buf;
        smp->GetBufferByIndex(0, &buf);
        ComPtr<IMFDXGIBuffer> dx;
        ComPtr<ID3D11Texture2D> tex;
        UINT slice = 0;
        if (buf && SUCCEEDED(buf.As(&dx)) && SUCCEEDED(dx->GetResource(IID_PPV_ARGS(&tex)))) {
            dx->GetSubresourceIndex(&slice);
        } else if (buf) {  // software decode: copy into one GPU texture
            sw_decode = true;
            if (!upload) {
                D3D11_TEXTURE2D_DESC td{};
                td.Width = cw;
                td.Height = ch;
                td.MipLevels = td.ArraySize = 1;
                td.Format = fmt;
                td.SampleDesc.Count = 1;
                td.BindFlags = D3D11_BIND_RENDER_TARGET;  // the video processor takes input views on these
                c.dev->CreateTexture2D(&td, nullptr, &upload);
            }
            ComPtr<IMF2DBuffer> b2;
            BYTE* data = nullptr;
            LONG pitch = 0;
            if (!upload || FAILED(buf.As(&b2)) || FAILED(b2->Lock2D(&data, &pitch))) {
                std::printf("TRANSCODE FAIL software frame\n");
                return 6;
            }
            c.ctx->UpdateSubresource(upload.Get(), 0, nullptr, data, static_cast<UINT>(pitch), 0);
            b2->Unlock2D();
            tex = upload;
        }
        auto key = std::make_pair(tex.Get(), slice);
        if (!views.count(key)) {
            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
            ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
            ivd.Texture2D.ArraySlice = slice;
            ComPtr<ID3D11VideoProcessorInputView> iv;
            if (FAILED(vdev->CreateVideoProcessorInputView(tex.Get(), venum.Get(), &ivd, &iv))) {
                std::printf("TRANSCODE FAIL input view\n");
                return 6;
            }
            views[key] = iv;
        }
        // A fresh target per frame: the encoder may hold its input until it has encoded it.
        D3D11_TEXTURE2D_DESC od{};
        od.Width = ow;
        od.Height = oh;
        od.MipLevels = od.ArraySize = 1;
        od.Format = fmt;
        od.SampleDesc.Count = 1;
        od.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> outtex;
        ComPtr<ID3D11VideoProcessorOutputView> ov;
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
        ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        if (FAILED(c.dev->CreateTexture2D(&od, nullptr, &outtex)) ||
            FAILED(vdev->CreateVideoProcessorOutputView(outtex.Get(), venum.Get(), &ovd, &ov))) {
            std::printf("TRANSCODE FAIL output texture\n");
            return 6;
        }
        D3D11_VIDEO_PROCESSOR_STREAM st{};
        st.Enable = TRUE;
        st.pInputSurface = views[key].Get();
        vctx->VideoProcessorBlt(vp.Get(), ov.Get(), 0, 1, &st);
        ComPtr<IMFMediaBuffer> ob;
        ComPtr<IMFSample> os;
        if (FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), outtex.Get(), 0, FALSE, &ob)) || FAILED(MFCreateSample(&os))) {
            std::printf("TRANSCODE FAIL sample\n");
            return 6;
        }
        DWORD len = 0;
        ComPtr<IMF2DBuffer> ob2;
        if (SUCCEEDED(ob.As(&ob2))) ob2->GetContiguousLength(&len);
        ob->SetCurrentLength(len);
        os->AddBuffer(ob.Get());
        os->SetSampleTime(ts - first_ts);
        os->SetSampleDuration(static_cast<LONGLONG>(1e7 / fps));
        if (FAILED(writer->WriteSample(stream, os.Get()))) {
            std::printf("TRANSCODE FAIL write\n");
            return 6;
        }
        ++frames;
        if (NowMs() - last_sample >= 500) {
            const Sample sm = meter.Take();
            peak_local = std::max(peak_local, sm.gpu_local_mb);
            peak_ws = std::max(peak_ws, sm.working_mb);
            cpu_sum += sm.cpu_pct;
            ++cpu_n;
            last_sample = NowMs();
        }
    }
    if (FAILED(writer->Finalize())) {
        std::printf("TRANSCODE FAIL finalize\n");
        return 6;
    }
    const double secs = (NowMs() - t0) / 1000.0;
    const double video_secs = frames / fps;
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    GetFileAttributesExW(out.c_str(), GetFileExInfoStandard, &fa);
    const double out_mb = (static_cast<double>(fa.nFileSizeHigh) * 4294967296.0 + fa.nFileSizeLow) / (1024.0 * 1024.0);
    std::printf("TRANSCODE OK %llu frames in %.1f s (%.1fx real time), %s decode, peak gpu local %.1f MB, peak working set "
                "%.1f MB, cpu %.1f%%, output %.1f MB (%.1f Mbps)\n",
                static_cast<unsigned long long>(frames), secs, video_secs / secs, sw_decode ? "SOFTWARE" : "hardware",
                peak_local, peak_ws, cpu_n ? cpu_sum / cpu_n : 0.0, out_mb, out_mb * 8.0 * 1.048576 / video_secs);
    return 0;
}

Player* MakePlayer(const std::wstring& path) {
    if (path == L"engine-native") return new EnginePlayer(true);
    if (path == L"reader") return new ReaderPlayer();
    if (path == L"dxva") {
        auto* r = new ReaderPlayer();
        r->use_dxva = true;
        return r;
    }
    if (path == L"mft") {
        auto* r = new ReaderPlayer();
        r->use_mft = true;
        return r;
    }
    return new EnginePlayer(false);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %ls <video> [--path engine-screen|engine-native|reader] [--swap nv12|bgra] "
                             "[--seconds N] [--onscreen] [--csv out.csv]\n", argv[0]);
        return 2;
    }
    std::wstring file = argv[1], csv, path_kind = L"engine-screen";
    int seconds = 12;
    bool onscreen = false, force_bgra = false;
    std::wstring dump_png;
    int pool = -1, dump_frame = 45;
    std::wstring transcode_out;
    UINT target_w = 0, target_h = 0;
    bool decode_swap = false;
    for (int i = 2; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--seconds" && i + 1 < argc) seconds = std::max(3, _wtoi(argv[++i]));
        else if (a == L"--onscreen") onscreen = true;
        else if (a == L"--csv" && i + 1 < argc) csv = argv[++i];
        else if (a == L"--path" && i + 1 < argc) path_kind = argv[++i];
        else if (a == L"--swap" && i + 1 < argc) force_bgra = std::wstring(argv[++i]) == L"bgra";
        else if (a == L"--dump" && i + 1 < argc) dump_png = argv[++i];
        else if (a == L"--pool" && i + 1 < argc) pool = _wtoi(argv[++i]);
        else if (a == L"--dump-frame" && i + 1 < argc) dump_frame = _wtoi(argv[++i]);
        else if (a == L"--transcode" && i + 1 < argc) transcode_out = argv[++i];
        else if (a == L"--target" && i + 1 < argc) {
            const std::wstring t = argv[++i];
            target_w = static_cast<UINT>(_wtoi(t.c_str()));
            const size_t x_at = t.find(L'x');
            if (x_at != std::wstring::npos) target_h = static_cast<UINT>(_wtoi(t.c_str() + x_at + 1));
        }
        else if (a == L"--present" && i + 1 < argc) decode_swap = std::wstring(argv[++i]) == L"decode";
    }
    wchar_t full[MAX_PATH * 2]{};
    GetFullPathNameW(file.c_str(), static_cast<DWORD>(std::size(full)), full, nullptr);
    file = full;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);
    Meter meter;
    meter.Start();

    const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PulseVideoWallpaperProbe";
    RegisterClassW(&wc);
    const int x = onscreen ? 0 : GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN) + 64;
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName,
                                L"video wallpaper probe", WS_POPUP, x, 0, sw, sh, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return 3;
    SetWindowPos(hwnd, HWND_BOTTOM, x, 0, sw, sh, SWP_NOACTIVATE | SWP_SHOWWINDOW);

    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> dctx;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels, 2,
                                 D3D11_SDK_VERSION, &dev, nullptr, &dctx)))
        return 4;
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(dev.As(&mt))) mt->SetMultithreadProtected(TRUE);
    ComPtr<IDXGIDevice> dxgi_dev;
    dev.As(&dxgi_dev);
    ComPtr<IDXGIAdapter> adapter;
    dxgi_dev->GetAdapter(&adapter);
    adapter.As(&meter.adapter);
    ComPtr<IDXGIFactory2> factory;
    adapter->GetParent(IID_PPV_ARGS(&factory));
    {  // Can this GPU/output put YUV straight on screen? (NV12 swap chains and decode swap chains need it.)
        ComPtr<IDXGIOutput> out;
        ComPtr<IDXGIOutput3> out3;
        UINT nv12 = 0, bgra = 0;
        if (SUCCEEDED(adapter->EnumOutputs(0, &out)) && SUCCEEDED(out.As(&out3))) {
            out3->CheckOverlaySupport(DXGI_FORMAT_NV12, dev.Get(), &nv12);
            out3->CheckOverlaySupport(DXGI_FORMAT_B8G8R8A8_UNORM, dev.Get(), &bgra);
            std::printf("overlay support: NV12 0x%X, BGRA 0x%X (1 = direct, 2 = scaling)\n", nv12, bgra);
        }
    }
    DXGI_ADAPTER_DESC adesc{};
    adapter->GetDesc(&adesc);
    UINT token = 0;
    ComPtr<IMFDXGIDeviceManager> mgr;
    MFCreateDXGIDeviceManager(&token, &mgr);
    mgr->ResetDevice(dev.Get(), token);
    ComPtr<IDCompositionDevice> dcomp;
    ComPtr<IDCompositionTarget> target;
    if (FAILED(DCompositionCreateDevice(dxgi_dev.Get(), IID_PPV_ARGS(&dcomp))) ||
        FAILED(dcomp->CreateTargetForHwnd(hwnd, TRUE, &target)))
        return 5;
    Ctx c;
    c.dev = dev.Get();
    c.ctx = dctx.Get();
    c.mgr = mgr.Get();
    c.dcomp = dcomp.Get();
    c.target = target.Get();
    c.factory = factory.Get();
    c.screen_w = static_cast<UINT>(sw);
    c.screen_h = static_cast<UINT>(sh);
    c.force_bgra = force_bgra;
    c.dump_png = dump_png;
    c.hwnd = hwnd;
    c.pool = pool;
    c.dump_frame = dump_frame;
    c.decode_swap = decode_swap;
    DwmGpuMeter dwm;
    const bool dwm_ok = dwm.Start();
    Pump(300);
    meter.Take();
    const Sample idle = meter.Take();
    double dwm_idle_ded = -1, dwm_idle_sh = -1;
    dwm.Take(dwm_idle_ded, dwm_idle_sh);

    std::printf("file: %s\npath: %ls  adapter: %ls  screen: %dx%d  window: %s\n", U8(file).c_str(), path_kind.c_str(),
                adesc.Description, sw, sh, onscreen ? "on-screen (bottom)" : "off-screen");
    std::printf("baseline (device + composition, no video): private %.1f MB, gpu local %.1f MB, gpu shared %.1f MB\n",
                idle.private_mb, idle.gpu_local_mb, idle.gpu_shared_mb);

    if (!transcode_out.empty()) {
        wchar_t outfull[MAX_PATH * 2]{};
        GetFullPathNameW(transcode_out.c_str(), static_cast<DWORD>(std::size(outfull)), outfull, nullptr);
        const int rc = Transcode(c, meter, file, outfull, target_w ? target_w : c.screen_w, target_h ? target_h : c.screen_h);
        target.Reset();
        dcomp.Reset();
        DestroyWindow(hwnd);
        MFShutdown();
        CoUninitialize();
        return rc;
    }

    auto write_csv = [&](const std::string& line) {
        if (csv.empty()) return;
        if (FILE* f = _wfopen(csv.c_str(), L"ab")) {
            std::fprintf(f, "%s\n", line.c_str());
            fclose(f);
        }
    };
    std::string name = U8(file);
    name = name.substr(name.find_last_of("\\/") + 1);

    Player* p = MakePlayer(path_kind);
    if (!p->Open(c, file)) {
        std::printf("RESULT FAIL %s\n", p->error.c_str());
        write_csv(name + "," + U8(path_kind) + ",FAIL," + p->error);
        p->Close(c);
        delete p;
        return 6;
    }
    std::printf("video %ux%u -> surface %ux%u, first frame %.0f ms\n", p->video_w, p->video_h, p->surface_w,
                p->surface_h, p->first_frame_ms);

    std::vector<Sample> play;
    double s0 = 0, l0 = 0, s1 = 0, l1 = 0;
    Pump(1000, p);
    meter.Take();
    p->Stats(s0, l0);
    std::vector<double> dwm_ded;
    for (int s = 0; s < seconds; ++s) {
        Pump(1000, p);
        play.push_back(meter.Take());
        double d = -1, d_shared = -1;
        dwm.Take(d, d_shared);
        dwm_ded.push_back(d);
    }
    std::sort(dwm_ded.begin(), dwm_ded.end());
    const double dwm_play = dwm_ded.empty() ? -1 : dwm_ded[dwm_ded.size() / 2];
    p->Stats(s1, l1);
    auto median = [&](double Sample::*m) {
        std::vector<double> v;
        for (size_t i = play.size() / 2; i < play.size(); ++i) v.push_back(play[i].*m);
        std::sort(v.begin(), v.end());
        return v.empty() ? 0.0 : v[v.size() / 2];
    };
    auto peak = [&](double Sample::*m) {
        double best = 0;
        for (const auto& s : play) best = std::max(best, s.*m);
        return best;
    };
    const double fps = (s1 - s0) / seconds;
    std::printf("%s\n", p->detail.c_str());
    std::printf("playing  (%d s): private %.1f MB (peak %.1f), working set %.1f MB, gpu local %.1f MB (peak %.1f), "
                "gpu shared %.1f MB, cpu %.1f%%, %.1f fps shown, %.0f late/dropped\n",
                seconds, median(&Sample::private_mb), peak(&Sample::private_mb), median(&Sample::working_mb),
                median(&Sample::gpu_local_mb), peak(&Sample::gpu_local_mb), median(&Sample::gpu_shared_mb),
                median(&Sample::cpu_pct), fps, l1 - l0);
    if (dwm_ok) std::printf("dwm.exe gpu dedicated: idle %.1f MB, playing %.1f MB (%+.1f)\n", dwm_idle_ded, dwm_play,
                            dwm_play - dwm_idle_ded);

    p->Pause();
    Pump(3000, p);
    meter.Take();
    Pump(1000, p);
    const Sample paused = meter.Take();
    std::printf("paused   : private %.1f MB, gpu local %.1f MB, gpu shared %.1f MB, cpu %.1f%%\n", paused.private_mb,
                paused.gpu_local_mb, paused.gpu_shared_mb, paused.cpu_pct);

    const UINT vw = p->video_w, vh = p->video_h, surf_w = p->surface_w, surf_h = p->surface_h;
    const double first_ms = p->first_frame_ms;
    const std::string detail = p->detail;
    p->Close(c);
    delete p;
    Pump(2000);
    meter.Take();
    Pump(1000);
    const Sample released = meter.Take();
    std::printf("released : private %.1f MB, gpu local %.1f MB, gpu shared %.1f MB\n", released.private_mb,
                released.gpu_local_mb, released.gpu_shared_mb);

    Player* again = MakePlayer(path_kind);
    const double reopen_ms = again->Open(c, file) ? again->first_frame_ms : -1;
    again->Close(c);
    delete again;
    std::printf("reopen   : first frame %.0f ms\n", reopen_ms);

    const double d_private = median(&Sample::private_mb) - idle.private_mb;
    const double d_local = median(&Sample::gpu_local_mb) - idle.gpu_local_mb;
    const double d_shared = median(&Sample::gpu_shared_mb) - idle.gpu_shared_mb;
    std::printf("RESULT OK video cost while playing: private +%.1f MB, gpu local +%.1f MB, gpu shared +%.1f MB\n",
                d_private, d_local, d_shared);
    // file,path,status,video,surface,first_ms,private+,gpu_local+,gpu_shared+,cpu,fps,late,
    // paused_private+,paused_gpu_local+,released_private+,released_gpu_local+,reopen_ms,detail,dwm_gpu+
    char line[700];
    std::snprintf(line, sizeof(line),
                  "%s,%s,OK,%ux%u,%ux%u,%.0f,%.1f,%.1f,%.1f,%.1f,%.1f,%.0f,%.1f,%.1f,%.1f,%.1f,%.0f,\"%s\",%.1f", name.c_str(),
                  U8(path_kind).c_str(), vw, vh, surf_w, surf_h, first_ms, d_private, d_local, d_shared,
                  median(&Sample::cpu_pct), fps, l1 - l0, paused.private_mb - idle.private_mb,
                  paused.gpu_local_mb - idle.gpu_local_mb, released.private_mb - idle.private_mb,
                  released.gpu_local_mb - idle.gpu_local_mb, reopen_ms, detail.c_str(),
                  dwm_ok ? dwm_play - dwm_idle_ded : -1.0);
    write_csv(line);
    target.Reset();
    dcomp.Reset();
    DestroyWindow(hwnd);
    MFShutdown();
    CoUninitialize();
    return 0;
}

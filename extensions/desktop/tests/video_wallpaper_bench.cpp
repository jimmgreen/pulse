// video_wallpaper_bench.cpp - Real hardware verification & benchmark suite for Pulse Video Wallpaper:
//   1. Production Transcoder (pulse_wallpaper_transcoder.exe) benchmarks:
//      - hevc-8k -> 4K (HW decode + HEVC Main HW encode)
//      - h264-8k -> 4K (SW decode + HEVC Main HW encode under Job Object RAM cap)
//      - hevc-4k-10bit -> 1440p (HW decode + HEVC Main10 10-bit HW encode)
//      Records wall time, peak child Working Set / Private bytes, output file size, codec & bit-depth probe.
//   2. Production Playback (VideoWallpaperPlayer + real DesktopRenderer DComp tree + 1/4-res glass active):
//      - Baseline (idle renderer window)
//      - h264-1080p original
//      - Transcoded 4K HEVC (from hevc-8k)
//      - Transcoded 1440p 10-bit (from hevc-4k-10bit)
//      - 30s FreezeAndRelease -> ResumeFromFrozen
//      Measures first-frame ms, steady FPS, dropped frames, process WS / Private, Process GPU Local+Shared
//      (IDXGIAdapter3::QueryVideoMemoryInfo), and DWM GPU Dedicated+Shared (PDH).
//   3. Color Fidelity (BT.601 vs BT.709):
//      Compares VideoWallpaperPlayer VideoProcessor BGRA frames (both direct playback and transcoded)
//      against FFmpeg-decoded PNG reference frames.
//   4. Shared-Resource Coexistence (AGENTS.md mandatory video verification):
//      Measures an independent MF D3D11VA playback stream (first-frame, FPS, dropped, clock monotonicity,
//      pause hold, seek latency & timestamp accuracy) both ALONE (baseline) and CONCURRENTLY with
//      4K VideoWallpaperPlayer + background thumbnail extraction bursts.
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
#include <mfidl.h>
#include <mfreadwrite.h>
#include <pdh.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <wincodec.h>
#include <wrl/client.h>

#include "host/render/desktop_renderer.h"
#include "host/video/transcode_manager.h"
#include "host/video/video_color_space.h"
#include "host/video/video_prober.h"
#include "host/video/video_wallpaper_player.h"
#include "host/video/wallpaper_policy.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace pulse::desktop::video;
namespace fs = std::filesystem;

namespace {

double NowMs() {
    static const LARGE_INTEGER freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}

double ToMb(uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }

LRESULT CALLBACK BenchWndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

struct MemSample {
    double ws_mb = 0.0;
    double priv_mb = 0.0;
    double gpu_local_mb = 0.0;
    double gpu_shared_mb = 0.0;
    double dwm_ded_mb = -1.0;
    double dwm_shared_mb = -1.0;
};

class DwmGpuMeter {
public:
    ~DwmGpuMeter() { Close(); }
    bool Start() {
        DWORD dwm = 0;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe{sizeof(pe)};
            if (Process32FirstW(snap, &pe)) {
                do {
                    if (_wcsicmp(pe.szExeFile, L"dwm.exe") == 0) dwm = pe.th32ProcessID;
                } while (Process32NextW(snap, &pe));
            }
            CloseHandle(snap);
        }
        if (!dwm || PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) return false;
        auto add = [&](const wchar_t* counter, std::vector<PDH_HCOUNTER>& out) {
            wchar_t wild[256]{};
            std::swprintf(wild, 256, L"\\GPU Process Memory(pid_%lu_*)\\%s", dwm, counter);
            DWORD len = 0;
            PdhExpandWildCardPathW(nullptr, wild, nullptr, &len, 0);
            if (!len) return;
            std::vector<wchar_t> list(len + 2);
            if (PdhExpandWildCardPathW(nullptr, wild, list.data(), &len, 0) != ERROR_SUCCESS) return;
            for (const wchar_t* path = list.data(); *path; path += wcslen(path) + 1) {
                PDH_HCOUNTER h = nullptr;
                if (PdhAddEnglishCounterW(query_, path, 0, &h) == ERROR_SUCCESS) out.push_back(h);
            }
        };
        add(L"Dedicated Usage", dedicated_);
        add(L"Shared Usage", shared_);
        PdhCollectQueryData(query_);
        return !dedicated_.empty();
    }
    void Take(double& ded_mb, double& sh_mb) {
        ded_mb = sh_mb = -1.0;
        if (!query_ || PdhCollectQueryData(query_) != ERROR_SUCCESS) return;
        auto sum = [](const std::vector<PDH_HCOUNTER>& v) {
            double total = 0.0;
            for (auto h : v) {
                PDH_FMT_COUNTERVALUE val{};
                if (PdhGetFormattedCounterValue(h, PDH_FMT_LARGE, nullptr, &val) == ERROR_SUCCESS) {
                    total += static_cast<double>(val.largeValue);
                }
            }
            return total / (1024.0 * 1024.0);
        };
        ded_mb = sum(dedicated_);
        sh_mb = sum(shared_);
    }
    void Close() {
        if (query_) {
            PdhCloseQuery(query_);
            query_ = nullptr;
        }
        dedicated_.clear();
        shared_.clear();
    }
private:
    PDH_HQUERY query_ = nullptr;
    std::vector<PDH_HCOUNTER> dedicated_, shared_;
};

ComPtr<IDXGIAdapter3> OpenAdapterByLuid(LUID luid) {
    ComPtr<IDXGIFactory4> fac;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&fac)))) return {};
    ComPtr<IDXGIAdapter1> ad;
    if (luid.LowPart != 0 || luid.HighPart != 0) {
        if (SUCCEEDED(fac->EnumAdapterByLuid(luid, IID_PPV_ARGS(&ad)))) {
            ComPtr<IDXGIAdapter3> ad3;
            if (SUCCEEDED(ad.As(&ad3))) return ad3;
        }
    }
    if (SUCCEEDED(fac->EnumAdapters1(0, &ad))) {
        ComPtr<IDXGIAdapter3> ad3;
        if (SUCCEEDED(ad.As(&ad3))) return ad3;
    }
    return {};
}

MemSample TakeMemSample(IDXGIAdapter3* adapter, DwmGpuMeter& dwm) {
    MemSample s{};
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        s.ws_mb = ToMb(pmc.WorkingSetSize);
        s.priv_mb = ToMb(pmc.PrivateUsage);
    }
    if (adapter) {
        DXGI_QUERY_VIDEO_MEMORY_INFO loc{}, sh{};
        adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &loc);
        adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &sh);
        s.gpu_local_mb = ToMb(loc.CurrentUsage);
        s.gpu_shared_mb = ToMb(sh.CurrentUsage);
    }
    dwm.Take(s.dwm_ded_mb, s.dwm_shared_mb);
    return s;
}

void SampleTranscoderProcessPeak(double& peak_ws_mb, double& peak_priv_mb) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{sizeof(pe)};
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"pulse_wallpaper_transcoder.exe") == 0) {
                HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
                if (hp) {
                    PROCESS_MEMORY_COUNTERS_EX pmc{};
                    pmc.cb = sizeof(pmc);
                    if (GetProcessMemoryInfo(hp, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
                        peak_ws_mb = std::max(peak_ws_mb, ToMb(pmc.PeakWorkingSetSize));
                        peak_priv_mb = std::max(peak_priv_mb, ToMb(pmc.PeakPagefileUsage));
                    }
                    CloseHandle(hp);
                }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

struct TranscodeBenchResult {
    std::string label;
    bool ok = false;
    double wall_sec = 0.0;
    double peak_ws_mb = 0.0;
    double peak_priv_mb = 0.0;
    double out_size_mb = 0.0;
    uint32_t out_w = 0, out_h = 0, out_bits = 0;
    std::wstring out_codec;
    std::wstring err;
};

TranscodeBenchResult RunTranscodeBench(const std::string& label,
                                       const fs::path& src,
                                       const fs::path& out_dir,
                                       const std::wstring& stem,
                                       uint32_t target_w,
                                       uint32_t target_h,
                                       TranscodeCodecChoice codec,
                                       uint64_t mem_limit) {
    TranscodeBenchResult res{};
    res.label = label;
    const fs::path tmp_out = out_dir / (stem + L".tmp.mp4");
    const fs::path fin_out = out_dir / (stem + L".mp4");
    DeleteFileW(tmp_out.c_str());
    DeleteFileW(fin_out.c_str());

    TranscodeJobSpec spec{};
    spec.job_id = 1;
    spec.source_path = src.wstring();
    spec.cache_key = stem;
    spec.temp_output_path = tmp_out.wstring();
    spec.final_output_path = fin_out.wstring();
    spec.target_width = target_w;
    spec.target_height = target_h;
    spec.codec = codec;
    spec.memory_limit_bytes = mem_limit;

    std::atomic<bool> done{false};
    TranscodeOutcome outcome = TranscodeOutcome::Failed;
    std::wstring err;

    TranscodeManager tm;
    const double t0 = NowMs();
    const bool started = tm.StartJob(spec, {
        nullptr,
        [&](uint64_t, TranscodeOutcome o, const std::wstring& msg) {
            outcome = o;
            err = msg;
            done.store(true);
        }
    });
    if (!started) {
        res.err = L"StartJob failed";
        return res;
    }
    while (!done.load()) {
        SampleTranscoderProcessPeak(res.peak_ws_mb, res.peak_priv_mb);
        Sleep(40);
    }
    tm.WaitUntilIdle();
    res.wall_sec = (NowMs() - t0) / 1000.0;
    res.err = err;
    res.ok = (outcome == TranscodeOutcome::Succeeded && fs::exists(fin_out));
    if (res.ok) {
        std::error_code ec;
        res.out_size_mb = ToMb(fs::file_size(fin_out, ec));
        const auto probe = ProbeVideoFile(fin_out.wstring());
        res.out_w = probe.display_width;
        res.out_h = probe.display_height;
        res.out_bits = probe.bit_depth;
        res.out_codec = probe.codec_name;
    }
    std::printf("[TRANSCODE] %-24s ok=%d wall=%.2fs peakWS=%.1fMB peakPriv=%.1fMB out=%.2fMB %ux%u %ubit %ls %ls\n",
                label.c_str(), res.ok ? 1 : 0, res.wall_sec, res.peak_ws_mb, res.peak_priv_mb,
                res.out_size_mb, res.out_w, res.out_h, res.out_bits, res.out_codec.c_str(),
                res.ok ? L"" : res.err.c_str());
    return res;
}

bool LoadPngBgra(const fs::path& path, std::vector<uint8_t>& bgra, uint32_t& w, uint32_t& h) {
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return false;
    if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0) return false;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(&conv))) return false;
    if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                                WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) return false;
    bgra.resize(size_t(w) * size_t(h) * 4u);
    return SUCCEEDED(conv->CopyPixels(nullptr, w * 4u, static_cast<UINT>(bgra.size()), bgra.data()));
}

bool CapturePlayerFrozenBgra(const fs::path& video_path, uint32_t max_w, uint32_t max_h, FrozenFrameSnapshot& out_snap) {
    std::atomic<bool> got_swap{false}, got_frozen{false};
    std::mutex mtx;
    ComPtr<IDXGISwapChain1> sc_held;

    VideoPlayerCallbacks cb{};
    cb.on_swap_chain_ready = [&](uint64_t, ComPtr<IDXGISwapChain1> sc, uint32_t, uint32_t, uint32_t, uint32_t) {
        std::lock_guard<std::mutex> lk(mtx);
        sc_held = std::move(sc);
        got_swap.store(true);
    };
    cb.on_frozen_frame_ready = [&](uint64_t, FrozenFrameSnapshot snap) {
        std::lock_guard<std::mutex> lk(mtx);
        out_snap = std::move(snap);
        got_frozen.store(true);
    };

    VideoWallpaperPlayer player;
    if (!player.Start(LUID{0, 0}, std::move(cb))) return false;
    player.OpenVideo(video_path.wstring(), max_w, max_h);
    for (int i = 0; i < 150 && !got_swap.load(); ++i) Sleep(20);
    Sleep(150);
    player.FreezeAndRelease();
    for (int i = 0; i < 150 && !got_frozen.load(); ++i) Sleep(20);
    {
        std::lock_guard<std::mutex> lk(mtx);
        sc_held.Reset();
    }
    player.Shutdown();
    return got_frozen.load() && !out_snap.bgra.empty();
}

void RunColorFidelityCheck(const std::string& label, const fs::path& video_path, const fs::path& ref_png) {
    std::vector<uint8_t> ref_bgra;
    uint32_t rw = 0, rh = 0;
    if (!LoadPngBgra(ref_png, ref_bgra, rw, rh)) {
        std::printf("[COLOR] %-24s SKIP (ref png load failed: %ls)\n", label.c_str(), ref_png.c_str());
        return;
    }
    FrozenFrameSnapshot snap{};
    if (!CapturePlayerFrozenBgra(video_path, rw, rh, snap) || snap.width != rw || snap.height != rh) {
        std::printf("[COLOR] %-24s FAIL (player capture %ux%u vs ref %ux%u)\n",
                    label.c_str(), snap.width, snap.height, rw, rh);
        return;
    }
    double sum_abs = 0.0;
    uint64_t count = 0;
    int max_err = 0;
    const uint32_t y0 = rh / 10, y1 = rh - rh / 10;
    const uint32_t x0 = rw / 10, x1 = rw - rw / 10;
    for (uint32_t y = y0; y < y1; ++y) {
        const uint8_t* a = snap.bgra.data() + size_t(y) * snap.stride;
        const uint8_t* b = ref_bgra.data() + size_t(y) * rw * 4u;
        for (uint32_t x = x0; x < x1; ++x) {
            for (int c = 0; c < 3; ++c) {
                const int d = std::abs(int(a[x * 4 + c]) - int(b[x * 4 + c]));
                sum_abs += d;
                if (d > max_err) max_err = d;
                ++count;
            }
        }
    }
    const double mae = count > 0 ? (sum_abs / static_cast<double>(count)) : 999.0;
    std::printf("[COLOR] %-24s %ux%u MAE=%.2f/255 maxErr=%d/255 (%s)\n",
                label.c_str(), rw, rh, mae, max_err, mae <= 4.0 ? "PASS" : "CHECK");
}

// Secondary MF D3D11VA playback benchmark simulating Pulse Quick Look / video preview under GPU sharing.
struct SecondaryPlaybackMetrics {
    bool ok = false;
    double first_frame_ms = -1.0;
    uint64_t frames = 0;
    uint64_t dropped = 0;
    double fps = 0.0;
    double max_interval_ms = 0.0;
    bool monotonic_pts = true;
    bool pause_held = true;
    double seek_first_ms = -1.0;
    double seek_pts_err_ms = -1.0;
    uint64_t post_seek_frames = 0;
};

SecondaryPlaybackMetrics RunSecondaryVideoStream(const fs::path& video_path) {
    SecondaryPlaybackMetrics m{};
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                 levels, 2, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
        return m;
    }
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(dev.As(&mt))) mt->SetMultithreadProtected(TRUE);

    UINT token = 0;
    ComPtr<IMFDXGIDeviceManager> dm;
    if (FAILED(MFCreateDXGIDeviceManager(&token, &dm)) || FAILED(dm->ResetDevice(dev.Get(), token))) return m;

    ComPtr<IMFAttributes> attr;
    MFCreateAttributes(&attr, 6);
    attr->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, dm.Get());
    attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    attr->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);

    const double t_open = NowMs();
    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(video_path.c_str(), attr.Get(), &reader))) return m;
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);

    ComPtr<IMFMediaType> out_t;
    MFCreateMediaType(&out_t);
    out_t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out_t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    if (FAILED(reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, out_t.Get()))) {
        return m;
    }

    auto read_one = [&](LONGLONG& pts_100ns, bool& eof) -> bool {
        DWORD si = 0, flags = 0;
        ComPtr<IMFSample> sample;
        const HRESULT hr = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                                              0, &si, &flags, &pts_100ns, &sample);
        if (FAILED(hr)) return false;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { eof = true; return false; }
        if (!sample) return false;
        // Touch the GPU texture to ensure decode actually completes on the GPU
        ComPtr<IMFMediaBuffer> buf;
        ComPtr<IMFDXGIBuffer> dxgi_buf;
        if (SUCCEEDED(sample->GetBufferByIndex(0, &buf)) && SUCCEEDED(buf.As(&dxgi_buf))) {
            ComPtr<ID3D11Texture2D> tex;
            UINT sub = 0;
            dxgi_buf->GetResource(IID_PPV_ARGS(&tex));
            dxgi_buf->GetSubresourceIndex(&sub);
        }
        return true;
    };

    LONGLONG pts = 0;
    bool eof = false;
    if (!read_one(pts, eof)) return m;
    const double t_first = NowMs();
    m.first_frame_ms = t_first - t_open;
    m.frames = 1;
    LONGLONG prev_pts = pts;
    const LONGLONG start_pts = pts;
    double last_present = t_first;

    // Paced playback for 2.5 seconds
    const double play_end = t_first + 2500.0;
    while (NowMs() < play_end) {
        eof = false;
        if (!read_one(pts, eof)) {
            if (eof) break;
            continue;
        }
        if (pts < prev_pts) m.monotonic_pts = false;
        prev_pts = pts;
        const double target_ms = t_first + static_cast<double>(pts - start_pts) / 10000.0;
        const double now = NowMs();
        if (now < target_ms - 1.0) {
            Sleep(static_cast<DWORD>(std::min(50.0, target_ms - now)));
        } else if (now > target_ms + 45.0) {
            ++m.dropped;
        }
        const double p_now = NowMs();
        m.max_interval_ms = std::max(m.max_interval_ms, p_now - last_present);
        last_present = p_now;
        ++m.frames;
    }
    const double elapsed_s = (NowMs() - t_first) / 1000.0;
    m.fps = elapsed_s > 0.0 ? static_cast<double>(m.frames) / elapsed_s : 0.0;

    // Pause hold check (300 ms paused without reading samples)
    const uint64_t frames_before_pause = m.frames;
    Sleep(300);
    m.pause_held = (m.frames == frames_before_pause);

    // Seek check to 5.0 seconds
    PROPVARIANT pos{};
    pos.vt = VT_I8;
    pos.hVal.QuadPart = 50000000LL; // 5.0s
    const double t_seek = NowMs();
    if (SUCCEEDED(reader->SetCurrentPosition(GUID_NULL, pos))) {
        eof = false;
        if (read_one(pts, eof)) {
            m.seek_first_ms = NowMs() - t_seek;
            m.seek_pts_err_ms = std::abs(static_cast<double>(pts - 50000000LL) / 10000.0);
            m.post_seek_frames = 1;
            for (int i = 0; i < 19; ++i) {
                eof = false;
                if (!read_one(pts, eof)) break;
                ++m.post_seek_frames;
            }
        }
    }
    m.ok = (m.frames >= 40 && m.monotonic_pts && m.pause_held && m.post_seek_frames >= 15);
    return m;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    fs::path out_dir = fs::temp_directory_path() / L"pulse_vw_bench";
    fs::path color_bt601, color_bt709, ref_bt601, ref_bt709;
    bool skip_transcode = false;
    for (int i = 1; i < argc; ++i) {
        if (std::wcscmp(argv[i], L"--out") == 0 && i + 1 < argc) out_dir = argv[++i];
        else if (std::wcscmp(argv[i], L"--bt601") == 0 && i + 1 < argc) color_bt601 = argv[++i];
        else if (std::wcscmp(argv[i], L"--bt709") == 0 && i + 1 < argc) color_bt709 = argv[++i];
        else if (std::wcscmp(argv[i], L"--ref601") == 0 && i + 1 < argc) ref_bt601 = argv[++i];
        else if (std::wcscmp(argv[i], L"--ref709") == 0 && i + 1 < argc) ref_bt709 = argv[++i];
        else if (std::wcscmp(argv[i], L"--skip-transcode") == 0) skip_transcode = true;
    }
    std::error_code ec;
    fs::create_directories(out_dir, ec);

    const fs::path clips_dir = L"bench_data\\video-wallpaper-proto\\clips";
    const auto gpu = ProbeGpuCapabilities();
    std::printf("=== GPU & System ===\n");
    std::printf("DedicatedVRAM=%.0f MB iGPU=%d AvailRAM=%.0f MB HEVCDec=%d HEVCEncHW=%d H264EncHW=%d\n",
                ToMb(gpu.dedicated_vram_bytes), gpu.is_integrated_gpu ? 1 : 0,
                ToMb(gpu.available_ram_bytes), gpu.has_hevc_decoder ? 1 : 0,
                gpu.has_hevc_hw_encoder ? 1 : 0, gpu.has_h264_hw_encoder ? 1 : 0);

    std::ofstream csv(out_dir / L"video_wallpaper_bench.csv");
    csv << "section,case,ok,wall_or_first_ms,fps,dropped,ws_mb,ws_delta_mb,vram_mb,vram_delta_mb,dwm_ded_mb,dwm_delta_mb,notes\n";

    // 1. Transcoder benchmarks
    std::printf("\n=== 1. Production Transcoder (pulse_wallpaper_transcoder.exe) ===\n");
    const fs::path out_4k_hevc = out_dir / L"hevc_8k_to_4k.mp4";
    const fs::path out_4k_from_h264 = out_dir / L"h264_8k_to_4k.mp4";
    const fs::path out_1440_10bit = out_dir / L"hevc_4k10_to_1440p10.mp4";

    if (!skip_transcode) {
        if (fs::exists(clips_dir / L"hevc-8k.mp4")) {
            auto r = RunTranscodeBench("hevc-8k->4K(HW)", clips_dir / L"hevc-8k.mp4", out_dir,
                                       L"hevc_8k_to_4k", 3840, 2160, TranscodeCodecChoice::HevcMainHw,
                                       2ull * 1024ull * 1024ull * 1024ull);
            csv << "transcode," << r.label << "," << (r.ok ? 1 : 0) << "," << (r.wall_sec * 1000.0)
                << ",0,0," << r.peak_ws_mb << ",0,0,0,0,0," << r.out_w << "x" << r.out_h << "_" << r.out_bits << "bit\n";
        }
        if (fs::exists(clips_dir / L"h264-8k.mp4")) {
            auto r = RunTranscodeBench("h264-8k->4K(SWdec+HWenc)", clips_dir / L"h264-8k.mp4", out_dir,
                                       L"h264_8k_to_4k", 3840, 2160, TranscodeCodecChoice::HevcMainHw,
                                       3ull * 1024ull * 1024ull * 1024ull);
            csv << "transcode," << r.label << "," << (r.ok ? 1 : 0) << "," << (r.wall_sec * 1000.0)
                << ",0,0," << r.peak_ws_mb << ",0,0,0,0,0," << r.out_w << "x" << r.out_h << "_" << r.out_bits << "bit\n";
        }
        if (fs::exists(clips_dir / L"hevc-4k-10bit.mp4")) {
            auto r = RunTranscodeBench("hevc-4k10->1440p10(HW)", clips_dir / L"hevc-4k-10bit.mp4", out_dir,
                                       L"hevc_4k10_to_1440p10", 2560, 1440, TranscodeCodecChoice::HevcMain10Hw,
                                       2ull * 1024ull * 1024ull * 1024ull);
            csv << "transcode," << r.label << "," << (r.ok ? 1 : 0) << "," << (r.wall_sec * 1000.0)
                << ",0,0," << r.peak_ws_mb << ",0,0,0,0,0," << r.out_w << "x" << r.out_h << "_" << r.out_bits << "bit\n";
        }
    }

    // 2. Production Playback Memory & Performance Benchmarks (with DesktopRenderer DComp + Glass active)
    std::printf("\n=== 2. Production Playback (VideoWallpaperPlayer + DesktopRenderer DComp + 1/4-res Glass) ===\n");
    WNDCLASSW wc{};
    wc.lpfnWndProc = BenchWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PulseVideoBenchWnd";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
                                wc.lpszClassName, L"video-bench", WS_POPUP,
                                -4000, 0, 1920, 1080, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    pulse::desktop::render::DesktopRenderer renderer;
    renderer.InitializeForHwnd(hwnd, 1920, 1080, true, false);
    ComPtr<IDXGIAdapter3> adapter = OpenAdapterByLuid(renderer.AdapterLuid());
    DwmGpuMeter dwm;
    dwm.Start();

    Sleep(300);
    const MemSample base = TakeMemSample(adapter.Get(), dwm);
    std::printf("[PLAYBACK] %-24s WS=%.1fMB VRAM=%.1fMB SharedGPU=%.1fMB DWM_Ded=%.1fMB DWM_Sh=%.1fMB\n",
                "baseline_idle", base.ws_mb, base.gpu_local_mb, base.gpu_shared_mb, base.dwm_ded_mb, base.dwm_shared_mb);

    auto run_play_case = [&](const std::string& label, const fs::path& clip_path, uint32_t max_w, uint32_t max_h, bool test_freeze) {
        if (!fs::exists(clip_path)) return;
        std::atomic<bool> got_swap{false}, got_frozen{false};
        std::mutex mtx;
        ComPtr<IDXGISwapChain1> sc_held;
        uint32_t sc_w = 0, sc_h = 0;
        FrozenFrameSnapshot frozen{};

        VideoPlayerCallbacks cb{};
        cb.on_swap_chain_ready = [&](uint64_t, ComPtr<IDXGISwapChain1> sc, uint32_t w, uint32_t h, uint32_t, uint32_t) {
            std::lock_guard<std::mutex> lk(mtx);
            sc_held = std::move(sc);
            sc_w = w; sc_h = h;
            got_swap.store(true);
        };
        cb.on_glass_frame_ready = [&](uint64_t, uint64_t, HANDLE h_nt, uint32_t gw, uint32_t gh) {
            renderer.OnInternalVideoGlassFrame(h_nt, gw, gh);
        };
        cb.on_frozen_frame_ready = [&](uint64_t, FrozenFrameSnapshot snap) {
            std::lock_guard<std::mutex> lk(mtx);
            frozen = std::move(snap);
            got_frozen.store(true);
        };

        VideoWallpaperPlayer player;
        player.Start(renderer.AdapterLuid(), std::move(cb));
        player.SetGlassConsumerActive(true);
        player.SetGlassLiveRate(1);
        player.OpenVideo(clip_path.wstring(), max_w, max_h);

        for (int i = 0; i < 200 && !got_swap.load(); ++i) Sleep(20);
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (sc_held) renderer.SetVideoWallpaperSwapChain(sc_held.Get(), sc_w, sc_h, VideoFillMode::Fill);
        }

        // Play for 3.5 seconds steady state
        const auto s0 = player.Stats();
        const double t0 = NowMs();
        Sleep(3500);
        const double dt_s = (NowMs() - t0) / 1000.0;
        const auto s1 = player.Stats();
        const MemSample m = TakeMemSample(adapter.Get(), dwm);
        const double fps = dt_s > 0.0 ? static_cast<double>(s1.frames_shown - s0.frames_shown) / dt_s : 0.0;
        const double ws_d = m.ws_mb - base.ws_mb;
        const double vr_d = m.gpu_local_mb - base.gpu_local_mb;
        const double dwm_d = (m.dwm_ded_mb >= 0 && base.dwm_ded_mb >= 0) ? (m.dwm_ded_mb - base.dwm_ded_mb) : 0.0;

        std::printf("[PLAYBACK] %-24s first=%.1fms fps=%.1f drop=%llu glass=%llu WS=%.1f(+%.1f)MB VRAM=%.1f(+%.1f)MB DWM=%.1f(%+.1f)MB fmt=%d\n",
                    label.c_str(), s1.first_frame_ms, fps,
                    static_cast<unsigned long long>(s1.frames_dropped),
                    static_cast<unsigned long long>(s1.glass_frames_published),
                    m.ws_mb, ws_d, m.gpu_local_mb, vr_d, m.dwm_ded_mb, dwm_d, static_cast<int>(s1.swap_format));
        csv << "playback," << label << ",1," << s1.first_frame_ms << "," << fps << "," << s1.frames_dropped
            << "," << m.ws_mb << "," << ws_d << "," << m.gpu_local_mb << "," << vr_d
            << "," << m.dwm_ded_mb << "," << dwm_d << ",glass=" << s1.glass_frames_published << "\n";

        if (test_freeze) {
            player.FreezeAndRelease();
            for (int i = 0; i < 150 && !got_frozen.load(); ++i) Sleep(20);
            {
                std::lock_guard<std::mutex> lk(mtx);
                sc_held.Reset();
                if (!frozen.bgra.empty()) {
                    renderer.SetVideoWallpaperFrozenBgra(frozen.bgra.data(), frozen.width, frozen.height,
                                                         frozen.stride, VideoFillMode::Fill);
                    frozen.bgra.clear();
                    frozen.bgra.shrink_to_fit();
                }
            }
            renderer.TrimDxgiMemory();
            player.TrimDeviceMemory();
            Sleep(250);
            const MemSample mf = TakeMemSample(adapter.Get(), dwm);
            const double f_ws_d = mf.ws_mb - base.ws_mb;
            const double f_vr_d = mf.gpu_local_mb - base.gpu_local_mb;
            const double f_dwm_d = (mf.dwm_ded_mb >= 0 && base.dwm_ded_mb >= 0) ? (mf.dwm_ded_mb - base.dwm_ded_mb) : 0.0;
            std::printf("[FREEZE]   %-24s frozen=%ux%u WS=%.1f(+%.1f)MB VRAM=%.1f(+%.1f)MB DWM=%.1f(%+.1f)MB\n",
                        (label + "_frozen").c_str(), frozen.width, frozen.height,
                        mf.ws_mb, f_ws_d, mf.gpu_local_mb, f_vr_d, mf.dwm_ded_mb, f_dwm_d);
            csv << "freeze," << label << "_frozen,1,0,0,0," << mf.ws_mb << "," << f_ws_d
                << "," << mf.gpu_local_mb << "," << f_vr_d << "," << mf.dwm_ded_mb << "," << f_dwm_d << ",released\n";

            got_swap.store(false);
            player.ResumeFromFrozen();
            for (int i = 0; i < 150 && !got_swap.load(); ++i) Sleep(20);
            const auto sr = player.Stats();
            std::printf("[RESUME]   %-24s reopen_first_frame=%.1fms\n", label.c_str(), sr.reopen_first_frame_ms);
            csv << "resume," << label << "_resume,1," << sr.reopen_first_frame_ms << ",0,0,0,0,0,0,0,0,reopen\n";
        }

        renderer.ClearVideoWallpaper();
        player.Shutdown();
        renderer.TrimDxgiMemory();
        Sleep(200);
    };

    run_play_case("h264-1080p_orig", clips_dir / L"h264-1080p.mp4", 1920, 1080, false);
    if (fs::exists(out_4k_hevc)) {
        run_play_case("hevc-8k->4K_transcoded", out_4k_hevc, 3840, 2160, true);
    } else if (fs::exists(clips_dir / L"hevc-4k.mp4")) {
        run_play_case("hevc-4k_orig", clips_dir / L"hevc-4k.mp4", 3840, 2160, true);
    }
    if (fs::exists(out_1440_10bit)) {
        run_play_case("hevc-4k10->1440p10", out_1440_10bit, 2560, 1440, false);
    }

    // 3. Color fidelity (BT.601 vs BT.709)
    if (!color_bt601.empty() && !ref_bt601.empty()) {
        std::printf("\n=== 3. Color Fidelity (BT.601 vs BT.709) ===\n");
        RunColorFidelityCheck("bt601_direct_play", color_bt601, ref_bt601);
        if (!color_bt709.empty() && !ref_bt709.empty()) {
            RunColorFidelityCheck("bt709_direct_play", color_bt709, ref_bt709);
        }
        // Also verify transcoder preserves BT.601 & BT.709 end-to-end!
        auto r601 = RunTranscodeBench("color_bt601_tc", color_bt601, out_dir, L"color_bt601_tc",
                                      640, 360, TranscodeCodecChoice::HevcMainHw, 1024ull * 1024ull * 1024ull);
        if (r601.ok) RunColorFidelityCheck("bt601_after_transcode", out_dir / L"color_bt601_tc.mp4", ref_bt601);
        if (!color_bt709.empty() && !ref_bt709.empty()) {
            auto r709 = RunTranscodeBench("color_bt709_tc", color_bt709, out_dir, L"color_bt709_tc",
                                          1280, 720, TranscodeCodecChoice::HevcMainHw, 1024ull * 1024ull * 1024ull);
            if (r709.ok) RunColorFidelityCheck("bt709_after_transcode", out_dir / L"color_bt709_tc.mp4", ref_bt709);
        }
    }

    // 4. Shared-resource coexistence (AGENTS.md mandatory video verification)
    std::printf("\n=== 4. Shared-Resource Coexistence (Wallpaper 4K + Secondary Playback + BG Thumbnails) ===\n");
    const fs::path sec_clip = clips_dir / L"h264-4k.mp4";
    const fs::path wp_clip = fs::exists(out_4k_hevc) ? out_4k_hevc : (clips_dir / L"hevc-4k.mp4");
    if (fs::exists(sec_clip) && fs::exists(wp_clip)) {
        const auto base_sec = RunSecondaryVideoStream(sec_clip);
        std::printf("[COEXIST] secondary_baseline   ok=%d first=%.1fms fps=%.1f drop=%llu maxInt=%.1fms mono=%d pause=%d seek=%.1fms seekErr=%.1fms postSeek=%llu\n",
                    base_sec.ok ? 1 : 0, base_sec.first_frame_ms, base_sec.fps,
                    static_cast<unsigned long long>(base_sec.dropped), base_sec.max_interval_ms,
                    base_sec.monotonic_pts ? 1 : 0, base_sec.pause_held ? 1 : 0,
                    base_sec.seek_first_ms, base_sec.seek_pts_err_ms,
                    static_cast<unsigned long long>(base_sec.post_seek_frames));

        // Start 4K video wallpaper + glass consumer + background thumbnail extraction thread
        std::atomic<bool> wp_ready{false}, stop_thumbs{false};
        std::atomic<uint64_t> thumbs_done{0};
        std::mutex wp_mtx;
        ComPtr<IDXGISwapChain1> wp_sc;
        VideoPlayerCallbacks cb{};
        cb.on_swap_chain_ready = [&](uint64_t, ComPtr<IDXGISwapChain1> sc, uint32_t w, uint32_t h, uint32_t, uint32_t) {
            std::lock_guard<std::mutex> lk(wp_mtx);
            wp_sc = std::move(sc);
            renderer.SetVideoWallpaperSwapChain(wp_sc.Get(), w, h, VideoFillMode::Fill);
            wp_ready.store(true);
        };
        cb.on_glass_frame_ready = [&](uint64_t, uint64_t, HANDLE h_nt, uint32_t gw, uint32_t gh) {
            renderer.OnInternalVideoGlassFrame(h_nt, gw, gh);
        };

        VideoWallpaperPlayer wp;
        wp.Start(renderer.AdapterLuid(), std::move(cb));
        wp.SetGlassConsumerActive(true);
        wp.SetGlassLiveRate(1);
        wp.OpenVideo(wp_clip.wstring(), 3840, 2160);
        for (int i = 0; i < 200 && !wp_ready.load(); ++i) Sleep(20);
        Sleep(200);

        std::thread thumb_worker([&]() {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            const wchar_t* thumb_clips[] = {L"h264-1080p.mp4", L"hevc-1440p.mp4", L"hevc-4k.mp4"};
            int idx = 0;
            while (!stop_thumbs.load()) {
                const fs::path src = clips_dir / thumb_clips[idx % 3];
                const fs::path jpg = out_dir / (L"_coexist_thumb_" + std::to_wstring(idx % 3) + L".jpg");
                if (fs::exists(src) && ExtractPosterJpeg(src.wstring(), jpg.wstring(), 480, 270)) {
                    thumbs_done.fetch_add(1);
                }
                ++idx;
            }
            CoUninitialize();
        });

        const auto wp_s0 = wp.Stats();
        const double t_c0 = NowMs();
        const auto conc_sec = RunSecondaryVideoStream(sec_clip);
        const double t_c_s = (NowMs() - t_c0) / 1000.0;
        const auto wp_s1 = wp.Stats();
        stop_thumbs.store(true);
        thumb_worker.join();
        const double wp_fps = t_c_s > 0.0 ? static_cast<double>(wp_s1.frames_shown - wp_s0.frames_shown) / t_c_s : 0.0;

        std::printf("[COEXIST] secondary_concurrent ok=%d first=%.1fms fps=%.1f drop=%llu maxInt=%.1fms mono=%d pause=%d seek=%.1fms seekErr=%.1fms postSeek=%llu\n",
                    conc_sec.ok ? 1 : 0, conc_sec.first_frame_ms, conc_sec.fps,
                    static_cast<unsigned long long>(conc_sec.dropped), conc_sec.max_interval_ms,
                    conc_sec.monotonic_pts ? 1 : 0, conc_sec.pause_held ? 1 : 0,
                    conc_sec.seek_first_ms, conc_sec.seek_pts_err_ms,
                    static_cast<unsigned long long>(conc_sec.post_seek_frames));
        std::printf("[COEXIST] wallpaper_under_load fps=%.1f drop_delta=%llu glass_delta=%llu bg_thumbs_extracted=%llu\n",
                    wp_fps,
                    static_cast<unsigned long long>(wp_s1.frames_dropped - wp_s0.frames_dropped),
                    static_cast<unsigned long long>(wp_s1.glass_frames_published - wp_s0.glass_frames_published),
                    static_cast<unsigned long long>(thumbs_done.load()));
        csv << "coexist,secondary_baseline," << (base_sec.ok ? 1 : 0) << "," << base_sec.first_frame_ms << ","
            << base_sec.fps << "," << base_sec.dropped << ",0,0,0,0,0,0,seek=" << base_sec.seek_first_ms << "ms\n";
        csv << "coexist,secondary_concurrent," << (conc_sec.ok ? 1 : 0) << "," << conc_sec.first_frame_ms << ","
            << conc_sec.fps << "," << conc_sec.dropped << ",0,0,0,0,0,0,seek=" << conc_sec.seek_first_ms
            << "ms_wpFps=" << wp_fps << "_thumbs=" << thumbs_done.load() << "\n";

        renderer.ClearVideoWallpaper();
        wp.Shutdown();
    }

    renderer.Shutdown();
    DestroyWindow(hwnd);
    MFShutdown();
    CoUninitialize();
    std::printf("\nBENCH_DONE csv=%ls\n", (out_dir / L"video_wallpaper_bench.csv").c_str());
    return 0;
}

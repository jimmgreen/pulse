#include "host/video/video_prober.h"
#include "host/video/video_wallpaper_controller.h"

#include <wincodec.h>
#include <psapi.h>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

using namespace pulse::desktop::video;
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace pulse::desktop::video {
struct VideoWallpaperControllerTestPeer {
    struct Outcome {
        size_t event_count = 0;
        size_t probe_count = 0;
        bool generation_matches = false;
        bool request_matches = false;
        bool open_ok = false;
        bool poster_ok = false;
        bool player_started = false;
        std::wstring poster_path;
    };

    static void UseCache(VideoWallpaperController& controller, const std::wstring& path) {
        controller.cache_ = WallpaperCacheManager(path);
    }

    static Outcome JoinAndTake(VideoWallpaperController& controller, const std::wstring& source) {
        controller.JoinProbeThread();
        Outcome result;
        result.player_started = controller.player_started_;
        std::lock_guard<std::mutex> lock(controller.ev_mtx_);
        auto batch = controller.ev_mailbox_.TakeBatch();
        result.event_count = batch.size();
        for (const auto& event : batch) {
            if (event.kind != VideoWallpaperController::UiEvent::Kind::ProbeDone) continue;
            ++result.probe_count;
            result.generation_matches = event.generation == controller.generation_.load() &&
                                        event.probe.generation == event.generation;
            result.request_matches = event.probe.source_path == source &&
                event.probe.target_w == 3840 && event.probe.target_h == 2160 && event.probe.glass_rate == 2;
            result.open_ok = event.probe.probe.open_ok;
            result.poster_ok = event.probe.poster_ok;
            result.poster_path = event.probe.poster_path;
        }
        return result;
    }
};
}

namespace {
int failures = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    failures += !ok;
}

std::vector<char> Read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

bool ReadableJpeg(const fs::path& path, UINT max_width = 320, UINT max_height = 180) {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    UINT width = 0, height = 0;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnLoad, &decoder)) || FAILED(decoder->GetFrame(0, &frame)) ||
        FAILED(frame->GetSize(&width, &height)) || !width || !height || width > max_width || height > max_height) return false;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
    std::vector<BYTE> pixels(static_cast<size_t>(width) * height * 4u);
    return SUCCEEDED(converter->CopyPixels(nullptr, width * 4u, static_cast<UINT>(pixels.size()), pixels.data()));
}

bool HasOwnedTemp(const fs::path& directory, const std::wstring& name) {
    const std::wstring prefix = name + L".{";
    for (const auto& entry : fs::directory_iterator(directory)) {
        const auto filename = entry.path().filename().wstring();
        if (filename.starts_with(prefix) && filename.ends_with(L".tmp.jpg")) return true;
    }
    return false;
}

int ControllerImport(const std::wstring& clip, const fs::path& out) {
    const fs::path cache = out / L"controller-cache";
    Check(fs::create_directory(cache), "controller cache is a new isolated directory");
    const auto attempts_before = VideoProbeDeviceCreationAttempts();
    const auto begin = std::chrono::steady_clock::now();
    VideoWallpaperController controller;
    VideoWallpaperControllerTestPeer::UseCache(controller, cache.wstring());
    Check(controller.Initialize(nullptr, 0, nullptr, {}), "controller initializes with isolated cache and no window/renderer");
    controller.SetVideoWallpaper(clip, true, 3840, 2160, 2);
    // Inspect the actual worker mailbox without executing playback/transcode UI actions.
    const auto result = VideoWallpaperControllerTestPeer::JoinAndTake(controller, clip);
    const double elapsed_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - begin).count();
    const auto attempts = VideoProbeDeviceCreationAttempts() - attempts_before;
    const auto owners = VideoProbeJobsHoldingDevice();
    const bool isolated_poster = !result.poster_path.empty() &&
        fs::path(result.poster_path).parent_path().lexically_normal() == cache.lexically_normal();
    const bool readable = isolated_poster && ReadableJpeg(result.poster_path, 1920, 1080);
    Check(result.event_count == 1 && result.probe_count == 1 && result.generation_matches && result.request_matches,
          "public SetVideoWallpaper posts exactly one current-generation ProbeDone with original request");
    Check(result.open_ok && result.poster_ok && readable, "controller worker produces a readable JPEG inside isolated cache");
    Check(attempts == 1 && owners == 0, "controller capability/probe/poster share one device and release it before ProbeDone");
    Check(!result.player_started && !controller.IsOptimizing(), "mailbox inspection does not start playback or transcoding");
    controller.Shutdown();
    Check(VideoProbeJobsHoldingDevice() == 0, "controller shutdown leaves no probe job device owner");
    std::printf("CONTROLLER_PROBE_RESULT events=%zu probe_done=%zu open_ok=%d poster_ok=%d jpeg_readable=%d device_creation_attempts=%llu owners=%llu total_ms=%.3f failures=%d\n",
        result.event_count, result.probe_count, result.open_ok ? 1 : 0, result.poster_ok ? 1 : 0,
        readable ? 1 : 0, static_cast<unsigned long long>(attempts), static_cast<unsigned long long>(owners),
        elapsed_ms, failures);
    std::wprintf(L"CONTROLLER_PROBE_JPEG %ls\n", result.poster_path.c_str());
    return failures ? 1 : 0;
}

int MeasureImport(const std::wstring& clip, const fs::path& out, bool use_job) {
    PROCESS_MEMORY_COUNTERS_EX before{};
    before.cb = sizeof(before);
    const bool before_ok = !!GetProcessMemoryInfo(GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&before), sizeof(before));
    const auto attempts_before = VideoProbeDeviceCreationAttempts();
    const auto begin = std::chrono::steady_clock::now();
    GpuCapabilities caps;
    VideoProbeInfo probe;
    uint32_t accent = 0;
    bool saved = false;
    const fs::path poster = out / L"poster-1920x1080.jpg";
    if (use_job) {
        VideoProbeJob job;
        caps = job.ProbeCapabilities();
        probe = job.ProbeFile(clip);
        saved = job.ExtractPoster(clip, poster.wstring(), 1920, 1080, &accent);
        job.Reset();
    } else {
        caps = ProbeGpuCapabilities(nullptr);
        probe = ProbeVideoFile(clip, nullptr);
        saved = ExtractPosterJpeg(clip, poster.wstring(), 1920, 1080, &accent, nullptr);
    }
    const double elapsed_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - begin).count();
    PROCESS_MEMORY_COUNTERS_EX after{};
    after.cb = sizeof(after);
    const bool after_ok = !!GetProcessMemoryInfo(GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&after), sizeof(after));
    Sleep(1000);
    PROCESS_MEMORY_COUNTERS_EX settled{};
    settled.cb = sizeof(settled);
    const bool settled_ok = !!GetProcessMemoryInfo(GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&settled), sizeof(settled));
    std::printf("PROBE_SETTLED wait_ms=1000 memory_ok=%d working_set_bytes=%zu private_bytes=%zu\n",
        settled_ok ? 1 : 0, settled.WorkingSetSize, settled.PrivateUsage);
    // Verification decoding is outside both the production-job timer and memory sample.
    const bool caps_ok = caps.system_ram_bytes > 0 && caps.dedicated_vram_bytes > 0;
    const bool probe_ok = probe.open_ok && probe.has_mf_decoder;
    const bool readable = saved && ReadableJpeg(poster, 1920, 1080);
    const auto attempts = VideoProbeDeviceCreationAttempts() - attempts_before;
    std::printf("PROBE_JOB_MEASURE mode=%s device_creation_attempts=%llu caps_ok=%d probe_ok=%d poster_ok=%d jpeg_readable=%d total_ms=%.3f memory_ok=%d start_working_set_bytes=%zu start_private_bytes=%zu peak_working_set_bytes=%zu end_private_bytes=%zu owners=%llu accent=%08X\n",
        use_job ? "job" : "legacy", static_cast<unsigned long long>(attempts), caps_ok ? 1 : 0,
        probe_ok ? 1 : 0, saved ? 1 : 0, readable ? 1 : 0, elapsed_ms, before_ok && after_ok ? 1 : 0,
        before.WorkingSetSize, before.PrivateUsage, after.PeakWorkingSetSize, after.PrivateUsage,
        static_cast<unsigned long long>(VideoProbeJobsHoldingDevice()), accent);
    std::printf("MEMORY_NOTE peak_working_set_bytes is process lifetime peak; end_private_bytes is end PrivateUsage, NOT peak private. JPEG verification occurs after memory sample.\n");
    std::wprintf(L"PROBE_JOB_JPEG %ls\n", poster.c_str());
    const bool ok = caps_ok && probe_ok && readable && before_ok && after_ok &&
                    attempts == (use_job ? 1u : 3u) && VideoProbeJobsHoldingDevice() == 0;
    std::printf("PROBE_JOB_MEASURE_RESULT %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 3 && argc != 4) return 2;
    const bool controller_mode = argc == 4 && std::wstring(argv[1]) == L"--controller";
    const bool measure_job = argc == 4 && std::wstring(argv[3]) == L"--measure-job";
    const bool measure_legacy = argc == 4 && std::wstring(argv[3]) == L"--measure-legacy";
    const int expected_depth = argc == 4 && std::wstring(argv[3]) == L"--expect-bit-depth=8" ? 8 :
        (argc == 4 && std::wstring(argv[3]) == L"--expect-bit-depth=10" ? 10 : 0);
    if (argc == 4 && !controller_mode && !measure_job && !measure_legacy && !expected_depth) return 2;
    const bool cpu_only = std::wstring(argv[1]) == L"--cpu-only";
    if (cpu_only && argc == 4) return 2;
    const wchar_t* source_arg = argv[controller_mode ? 2 : 1];
    if (!cpu_only && !fs::is_regular_file(source_arg)) return 2;
    const fs::path out = fs::absolute(argv[controller_mode ? 3 : 2]) /
        (L"probe-job-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(out.parent_path());
    if (!fs::create_directory(out)) return 2;
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(co)) return 3;
    if (controller_mode) {
        const int result = ControllerImport(source_arg, out);
        CoUninitialize();
        return result;
    }
    if (measure_job || measure_legacy) {
        const int result = MeasureImport(argv[1], out, measure_job);
        CoUninitialize();
        return result;
    }
    const auto attempts_before = VideoProbeDeviceCreationAttempts();
    const auto owners_before = VideoProbeJobsHoldingDevice();
    unsigned factory_calls = 0;
    const auto missing = (out / L"missing.mp4").wstring();
    const auto output = (out / L"unused.jpg").wstring();
    {
        VideoProbeJob cancelled([] { return true; }, [&] {
            ++factory_calls;
            return ComPtr<ID3D11Device>{};
        });
        cancelled.ProbeCapabilities();
        cancelled.ProbeFile(missing);
        Check(!cancelled.ExtractPoster(missing, output, 320, 180) && factory_calls == 0 &&
              cancelled.DeviceCreationAttempts() == 0, "cancelled job performs no device creation or poster write");
    }
    {
        VideoProbeJob failed({}, [&] {
            ++factory_calls;
            return ComPtr<ID3D11Device>{};
        });
        const auto caps = failed.ProbeCapabilities();
        const auto probe = failed.ProbeFile(missing);
        Check(caps.system_ram_bytes > 0 && !probe.open_ok &&
              !failed.ExtractPoster(missing, output, 320, 180), "device failure retains non-GPU capability and file-error paths");
        failed.ProbeCapabilities();
        failed.ProbeFile(missing);
        failed.Reset();
        failed.Reset();
        failed.ProbeCapabilities();
        Check(factory_calls == 1 && failed.DeviceCreationAttempts() == 1 && !failed.HasDevice() &&
              VideoProbeDeviceCreationAttempts() == attempts_before &&
              !failed.ExtractPoster(missing, output, 320, 180), "failed factory never retries or falls back to hidden device creation; Reset closes job");
    }
    Check(VideoProbeJobsHoldingDevice() == owners_before && !fs::exists(output), "CPU error/cancel paths leave no device owner or output");

    if (!cpu_only) {
        const std::wstring clip = argv[1];
        const fs::path poster = out / L"poster.jpg";
        const auto real_start = VideoProbeDeviceCreationAttempts();
        {
            VideoProbeJob job;
            const auto caps = job.ProbeCapabilities();
            const auto probe = job.ProbeFile(clip);
            uint32_t accent = 0;
            const bool saved = job.ExtractPoster(clip, poster.wstring(), 320, 180, &accent);
            Check(job.HasDevice() && caps.system_ram_bytes > 0 && probe.open_ok && probe.has_mf_decoder &&
                  saved && ReadableJpeg(poster), "real production job completes capability, video probe, and readable JPEG extraction");
            std::printf("PROBE_FORMAT codec=%u bit_depth=%u hw_decode_supported=%d expected_bit_depth=%d\n",
                static_cast<unsigned>(probe.codec), static_cast<unsigned>(probe.bit_depth),
                probe.hw_decode_supported ? 1 : 0, expected_depth);
            if (expected_depth) {
                Check(probe.open_ok && probe.bit_depth == expected_depth,
                      "probe preserves explicitly supplied fixture bit depth");
            }
            Check(VideoProbeDeviceCreationAttempts() == real_start + 1 && job.DeviceCreationAttempts() == 1 &&
                  VideoProbeJobsHoldingDevice() == owners_before + 1,
                  "all three production job methods share exactly one real independent device");
            Check(!job.ExtractPoster(missing, (out / L"missing.jpg").wstring(), 320, 180), "missing video returns failure without replacing device");
            const fs::path blocked = out / L"directory-destination";
            fs::create_directory(blocked);
            Check(!job.ExtractPoster(clip, blocked.wstring(), 320, 180) && fs::is_directory(blocked) &&
                  !HasOwnedTemp(out, blocked.filename().wstring()), "failed atomic commit preserves destination and removes only own temporary file");
            Check(job.ExtractPoster(clip, (out / L"recovered.jpg").wstring(), 320, 180) &&
                  ReadableJpeg(out / L"recovered.jpg") && VideoProbeDeviceCreationAttempts() == real_start + 1,
                  "same real device recovers after file and commit errors");
            job.Reset();
            Check(!job.HasDevice() && VideoProbeJobsHoldingDevice() == owners_before &&
                  !job.ExtractPoster(clip, (out / L"closed.jpg").wstring(), 320, 180) &&
                  !job.ProbeFile(clip).open_ok && VideoProbeDeviceCreationAttempts() == real_start + 1,
                  "Reset releases job ownership and permanently prevents new work/device allocation");
        }
        const fs::path cancelled_path = out / L"cancelled.jpg";
        if (!fs::is_regular_file(poster)) {
            Check(false, "readable poster required for real atomic cancellation test");
            CoUninitialize();
            return 1;
        }
        fs::copy_file(poster, cancelled_path);
        const auto before = Read(cancelled_path);
        const fs::path unrelated = out / L"cancelled.jpg.tmp.jpg";
        { std::ofstream keep(unrelated); keep << "unrelated caller file"; }
        bool observed_commit_cancel = false;
        {
            VideoProbeJob job([&] {
                const bool cancel = HasOwnedTemp(out, L"cancelled.jpg");
                observed_commit_cancel |= cancel;
                return cancel;
            });
            uint32_t accent = 0x12345678u;
            Check(!job.ExtractPoster(clip, cancelled_path.wstring(), 320, 180, &accent) &&
                  observed_commit_cancel && accent == 0x12345678u && Read(cancelled_path) == before &&
                  fs::exists(unrelated) && !HasOwnedTemp(out, L"cancelled.jpg"),
                  "cancellation at real JPEG commit preserves prior output/accent and unrelated files");
        }
        Check(VideoProbeJobsHoldingDevice() == owners_before, "job destructor releases final independent device owner");
    }
    std::printf("PROBE_JOB_COUNTS attempts=%llu owners=%llu factory_calls=%u\n",
                static_cast<unsigned long long>(VideoProbeDeviceCreationAttempts() - attempts_before),
                static_cast<unsigned long long>(VideoProbeJobsHoldingDevice() - owners_before), factory_calls);
    std::wprintf(L"PROBE_JOB_ARTIFACTS %ls\n", out.c_str());
    CoUninitialize();
    std::printf("VIDEO_PROBE_JOB failures=%d gpu_cases=%d\n", failures, cpu_only ? 0 : 1);
    return failures ? 1 : 0;
}

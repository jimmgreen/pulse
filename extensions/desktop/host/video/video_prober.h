// video_prober.h - Media Foundation video metadata probe, DXVA/D3D11VA hardware decode
// capability query, accurate BT.601/BT.709 color space mapping, and WIC poster JPEG extraction.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11_4.h>
#include <dxgicommon.h>

#include "host/video/video_color_space.h"
#include "host/video/video_wallpaper_types.h"
#include "host/video/wallpaper_policy.h"

#include <cstdint>
#include <string>
#include <functional>
#include <wrl/client.h>

namespace pulse::desktop::video {

// Queries local GPU memory, iGPU status, and installed Media Foundation HEVC/H.264 codecs.
GpuCapabilities ProbeGpuCapabilities(ID3D11Device* existing_device = nullptr);

// Queries ID3D11VideoDevice (profile + format + config count) to determine whether hardware
// decode supports the given codec, bit depth, and coded resolution.
bool CheckHardwareDecodeSupport(ID3D11Device* d3d_device,
                                VideoCodecKind codec,
                                uint8_t bit_depth,
                                uint32_t width,
                                uint32_t height,
                                bool* out_profile_found = nullptr,
                                uint32_t* out_config_count = nullptr);

// Probes a video file using Media Foundation SourceReader without full playback decoding.
// Detects codec, effective aperture, duration, fps, bit depth, HDR flags, color matrix,
// installed MF decoder availability, and GPU hardware decode support.
VideoProbeInfo ProbeVideoFile(const std::wstring& path, ID3D11Device* d3d_device = nullptr);

// Diagnostic count of real standalone prober-device creation attempts in this process.
uint64_t VideoProbeDeviceCreationAttempts() noexcept;
uint64_t VideoProbeJobsHoldingDevice() noexcept;

// Decodes the first video frame, converts with accurate BT.601/BT.709 color space via
// D3D11 VideoProcessor (with CPU upload fallback for software decode), scales to <= max_w x max_h,
// extracts vibrant accent ARGB (optional), and writes an atomic JPEG poster file.
bool ExtractPosterJpeg(const std::wstring& video_path,
                       const std::wstring& poster_jpg_path,
                       uint32_t max_w,
                       uint32_t max_h,
                       uint32_t* out_accent_argb = nullptr,
                       ID3D11Device* d3d_device = nullptr);

// One background import/probe job owns one independent device. No readers or
// decoded surfaces survive a method call; Reset permanently closes this job.
class VideoProbeJob {
public:
    using Cancelled = std::function<bool()>;
    // A nonempty factory must create an exclusive device, never borrow a renderer device.
    using DeviceFactory = std::function<Microsoft::WRL::ComPtr<ID3D11Device>()>;
    explicit VideoProbeJob(Cancelled cancelled = {}, DeviceFactory factory = {});
    ~VideoProbeJob();
    VideoProbeJob(const VideoProbeJob&) = delete;
    VideoProbeJob& operator=(const VideoProbeJob&) = delete;

    GpuCapabilities ProbeCapabilities();
    VideoProbeInfo ProbeFile(const std::wstring& path);
    bool ExtractPoster(const std::wstring& path, const std::wstring& destination,
                       uint32_t max_w, uint32_t max_h, uint32_t* accent = nullptr);
    void Reset() noexcept;
    bool HasDevice() const noexcept { return !!device_; }
    uint32_t DeviceCreationAttempts() const noexcept { return device_creation_attempts_; }

private:
    bool IsCancelled() const;
    Cancelled cancelled_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    uint32_t device_creation_attempts_ = 0;
    bool closed_ = false;
};

} // namespace pulse::desktop::video

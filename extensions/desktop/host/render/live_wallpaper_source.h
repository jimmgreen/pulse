// live_wallpaper_source.h - Real-time GPU wallpaper backdrop source for Liquid Glass
// over dynamic wallpapers (Wallpaper Engine, Lively, etc.).
// - Windows 11 (Build 22000+): borderless Windows.Graphics.Capture on Explorer's wallpaper
//   host window (zero self-feedback, GPU-resident CopySubresourceRegion + D2D quarter-frost).
// - Windows 10 / WGC-restricted fallback: borderless PrintWindow(PW_RENDERFULLCONTENT) with
//   sampled checksum deduplication so static intervals cost 0 uploads and 0 Presents.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d2d1_1.h>

#include "host/live_wallpaper_detector.h"

#include <cstdint>
#include <memory>
#include <span>

namespace pulse::desktop::render {

enum class LiveCaptureBackend : uint8_t {
    None = 0,
    WgcWindow = 1,
    PrintWindowFallback = 2,
    VideoWallpaperShared = 3,
};

struct LiveGlassSourceStats {
    LiveCaptureBackend backend = LiveCaptureBackend::None;
    bool active = false;
    bool paused = false;
    bool has_frame = false;
    uint64_t frames_imported = 0;
    uint64_t frames_skipped_unchanged = 0;
    size_t gpu_bytes = 0;
};

class LiveWallpaperSource {
public:
    LiveWallpaperSource();
    ~LiveWallpaperSource();
    LiveWallpaperSource(const LiveWallpaperSource&) = delete;
    LiveWallpaperSource& operator=(const LiveWallpaperSource&) = delete;

    // Configures notification HWND + message posted when a new wallpaper frame is ready.
    void ConfigureNotification(HWND notify_hwnd, UINT frame_msg) noexcept;

    // Updates desired live state. Automatically starts, retargets, pauses (retaining the last
    // frame in `SharpBitmap()`), or after 30s of inactivity releases capture buffers.
    void SyncState(ID3D11Device* d3d_device,
                   ID2D1DeviceContext* d2d_dc,
                   const host::LiveWallpaperDecision& decision,
                   RECT surface_screen_px,
                   uint8_t rate_mode, // 0=15fps, 1=30fps, 2=60fps
                   bool glass_visible,
                   bool paused) noexcept;

    // Called on the UI thread when `frame_msg` arrives (or before drawing if pending).
    // Imports the newest frame onto GPU, updates `FrostBitmap()` when `need_frost` is true,
    // and returns true if any pixel under `glass_cards_surface_px` may have changed.
    bool ConsumePendingFrame(ID3D11Device* d3d_device,
                             ID2D1DeviceContext* d2d_dc,
                             bool need_frost,
                             std::span<const RECT> glass_cards_surface_px) noexcept;

    // Direct internal video wallpaper source (shared NT texture via KeyedMutex or CPU poster snapshot),
    // completely bypassing WGC/PrintWindow capture when Pulse's own video wallpaper is active.
    void SetInternalVideoMode(bool active) noexcept;
    bool IsInternalVideoMode() const noexcept;
    bool ImportSharedVideoTexture(ID3D11Device* d3d_device,
                                  ID2D1DeviceContext* d2d_dc,
                                  HANDLE shared_nt_handle,
                                  uint32_t w,
                                  uint32_t h,
                                  uint32_t logical_w,
                                  uint32_t logical_h,
                                  bool need_frost) noexcept;
    bool ImportBgraSnapshot(ID3D11Device* d3d_device,
                            ID2D1DeviceContext* d2d_dc,
                            const uint8_t* bgra,
                            uint32_t w,
                            uint32_t h,
                            uint32_t stride,
                            uint32_t logical_w,
                            uint32_t logical_h,
                            bool need_frost) noexcept;

    void ReleaseDeviceResources() noexcept;
    void ResetDesktopBlackFallback() noexcept;

    bool HasFrame() const noexcept;
    ID2D1Bitmap1* SharpBitmap() const noexcept;
    ID2D1Bitmap1* FrostBitmap() const noexcept;
    uint32_t SharpWidth() const noexcept;
    uint32_t SharpHeight() const noexcept;
    uint32_t FrostWidth() const noexcept;
    uint32_t FrostHeight() const noexcept;
    // `SharpBitmap()` is cropped directly to `surface_screen_px`, so its origin relative to
    // the desktop surface is `(0, 0)` and its logical extent is `(SurfaceWidth, SurfaceHeight)`.
    uint32_t LogicalScreenWidth() const noexcept;
    uint32_t LogicalScreenHeight() const noexcept;
    float AverageLuminance() const noexcept;
    LiveGlassSourceStats Stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulse::desktop::render

// video_color_space.h - Lightweight header-only MF -> DXGI color space mapping shared by
// video_prober, video_wallpaper_player, and pulse_wallpaper_transcoder without D2D/WIC deps.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgicommon.h>
#include <mfobjects.h>

#include "host/video/video_wallpaper_types.h"

namespace pulse::desktop::video {

inline DXGI_COLOR_SPACE_TYPE MapMfToDxgiColorSpace(const VideoColorInfo& color,
                                                   uint32_t width,
                                                   uint32_t height) noexcept {
    const bool full_range = (color.nominal_range == MFNominalRange_0_255);
    uint32_t matrix = color.yuv_matrix;
    if (matrix == MFVideoTransferMatrix_Unknown) {
        // Broadcast default: SD (< 720p) uses BT.601, HD/UHD uses BT.709.
        matrix = (height >= 720u || width >= 1280u) ? MFVideoTransferMatrix_BT709 : MFVideoTransferMatrix_BT601;
    }
    if (matrix == MFVideoTransferMatrix_BT601 || matrix == MFVideoTransferMatrix_SMPTE240M) {
        return full_range ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601
                          : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601;
    }
    if (matrix == MFVideoTransferMatrix_BT2020_10 || matrix == MFVideoTransferMatrix_BT2020_12) {
        return full_range ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020
                          : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P2020;
    }
    return full_range ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709
                      : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
}

} // namespace pulse::desktop::video

#pragma once
#include <windows.h>
#include <d2d1.h>
#include <cstdint>
#include <memory>

namespace pulse::desktop::render {
// One Windows.UI.Composition tree beneath the existing DComp foreground tree,
// attached to the SAME HWND. DWM supplies live pixels; no capture or timer loop.
class HostBackdropLayer {
public:
    HostBackdropLayer();
    ~HostBackdropLayer();
    bool Initialize(HWND hwnd) noexcept;
    // Independent HSL luminosity and color blending over DWM's blurred pixels.
    // Call after Initialize; a failure deactivates the layer for solid fallback.
    bool SetMaterial(bool dark, float tint_opacity, float luminosity_opacity,
                     uint32_t tint_argb) noexcept;
    void Reset() noexcept;
    void BeginFrame() noexcept;
    bool AddCard(const D2D1_RECT_F& rect_dip, float radius_dip, float scale,
                 float opacity = 1.0f) noexcept;
    void EndFrame() noexcept;
    bool Active() const noexcept;
    uint32_t VisualCount() const noexcept;
    uint64_t UpdateCount() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}

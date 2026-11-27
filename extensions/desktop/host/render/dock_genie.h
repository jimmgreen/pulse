// dock_genie.h - Hardware-accelerated D3D11 + DirectComposition Genie & Scale
// window minimize (collapse into Dock icon) and restore (expand from Dock icon) animator.
// Allocates its overlay swap chain strictly to the animation union rect during playback (~0.42s)
// and releases GPU resources immediately when idle (0 MB idle VRAM).
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace pulse::desktop::render {

enum class DockMinimizeEffect : uint8_t {
    Off = 0,
    Genie = 1, // Authentic macOS curved S-funnel warp into/out of the Dock icon
    Scale = 2, // Smooth macOS cubic scale & slide into/out of the Dock icon
};

// CPU-side premultiplied BGRA window snapshot used by Genie/Scale minimize & restore animations.
struct DockWindowSnapshot {
    int width = 0;
    int height = 0;
    RECT screen_rect{};
    std::vector<uint32_t> pixels; // Premultiplied BGRA (width * height)

    bool Valid() const noexcept {
        return width > 4 && height > 4 &&
               pixels.size() == static_cast<size_t>(width) * static_cast<size_t>(height);
    }
};

// Controls whether window capture may send PrintWindow(PW_RENDERFULLCONTENT) to the target window.
// PassiveScreenOnly NEVER sends WM_PRINT / PrintWindow to foreground apps (preventing GDI/MFC/DirectX
// toolbar flicker in apps such as AutoCAD).
enum class WindowCaptureMode : uint8_t {
    PassiveScreenOnly = 0,
    PreferScreenFallbackPrint = 1,
};

// Captures a crisp window frame cropped to DWMWA_EXTENDED_FRAME_BOUNDS.
// Uses passive composed-screen BitBlt first when unobstructed (zero WM_PRINT / zero flicker),
// falling back to PrintWindow(PW_RENDERFULLCONTENT) only when mode == PreferScreenFallbackPrint
// and the window is occluded or screen copy cannot capture it.
bool CaptureWindowSnapshotForDock(HWND hwnd,
                                  DockWindowSnapshot& out_snap,
                                  int max_long_edge = 1400,
                                  WindowCaptureMode mode = WindowCaptureMode::PreferScreenFallbackPrint,
                                  HWND ignore_above_hwnd = nullptr);

// Hardware-accelerated D3D11 + DirectComposition Genie / Scale minimize & restore warp overlay.
class DockGenieAnimator {
public:
    DockGenieAnimator();
    ~DockGenieAnimator();

    DockGenieAnimator(const DockGenieAnimator&) = delete;
    DockGenieAnimator& operator=(const DockGenieAnimator&) = delete;

    bool Initialize(HINSTANCE hinst);
    void Shutdown();

    // Starts a minimize (reverse=false: window -> dock icon) or restore (reverse=true: dock icon -> window) warp.
    // `dock_edge`: 0=Bottom, 1=Left, 2=Right. `on_complete` runs on the UI thread immediately upon completion.
    bool Start(const DockWindowSnapshot& snap,
               RECT window_screen_rect,
               RECT dock_slot_screen_rect,
               bool reverse,
               DockMinimizeEffect style,
               std::function<void()> on_complete = nullptr,
               uint8_t dock_edge = 0);

    // Advances the active warp locked to the host's DwmFlush VSync clock. Returns true while active.
    bool Tick(double now_ms);
    void ForceComplete() noexcept;
    bool IsActive() const noexcept;
    HWND OverlayHandle() const noexcept;

    // Offscreen D3D11 mesh warp preview renderer for headless regression tests & visual snapshots.
    bool RenderPreviewFrame(const DockWindowSnapshot& snap,
                            int canvas_w,
                            int canvas_h,
                            RECT win_rect,
                            RECT dock_rect,
                            float progress,
                            DockMinimizeEffect style,
                            std::vector<uint32_t>& out_bgra,
                            uint8_t dock_edge = 0);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulse::desktop::render

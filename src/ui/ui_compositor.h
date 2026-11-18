// ui_compositor.h — D3D11 + D2D + DWrite device, Mica, screenshot.
#pragma once
#include "FluentTokens.h"
#include <windows.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <dcomp.h>
#include <d2d1_3.h>
#include <dwrite_3.h>
#include <wincodec.h>
#include <memory>
#include <string>
#include <string_view>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace pulse::ui {

class LumaTextRenderer;
struct LumaTextStats;

template <typename T>
struct ComPtr {
    T* p = nullptr;
    ~ComPtr() { if (p) p->Release(); }
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& o) noexcept { p = o.p; o.p = nullptr; }
    ComPtr& operator=(ComPtr&& o) noexcept { if (p) p->Release(); p = o.p; o.p = nullptr; return *this; }
    T* operator->() const { return p; }
    T** operator&() { return &p; }
    T* get() const { return p; }
    void reset() { if (p) p->Release(); p = nullptr; }
};

class Compositor {
    friend struct QuickPreviewAuditTest;
public:
    Compositor();
    ~Compositor();

    bool Init(HWND hwnd);
    void SetCompositionAboveChildren(bool enabled) { composition_above_children_ = enabled; }
    // Starts creating the process's first D3D11 device on a worker thread;
    // the first Init on any thread takes it (waiting if needed) instead of
    // creating its own. Driver loading dominates that call (about 250 ms on
    // NVIDIA), so calling this early takes it off the startup critical path.
    static void PrefetchSharedDevice();
    const std::wstring& InitializationError() const { return initialization_error_; }
    void Shutdown();
    void Resize(int width, int height);
    // dirty: the only client rect this frame drew (partial redraw); null for
    // a whole frame. Ignored unless CanPresentPartial().
    // Reports swap-chain/DirectComposition submission status; S_OK alone means
    // both succeeded. Existing callers may deliberately ignore the status.
    HRESULT Present(const RECT* dirty = nullptr);
    // Partial redraw (plans/Pulse_局部重绘方案.md): the swap chain keeps its
    // buffers (FLIP_SEQUENTIAL) and the last frame reached the screen whole,
    // so the next one may redraw part of it. PULSE_PARTIAL_REDRAW=0 turns it off.
    static bool PartialRedrawEnabled();
    bool CanPresentPartial() const { return partial_swap_ && !full_next_ && !device_lost_; }
    void ForceFullFrame() { full_next_ = true; }
    // A lost device, or a swap chain that could not be resized: the next
    // Recover() rebuilds the window's graphics.
    void NotifyDeviceLost(HRESULT reason);
    bool Recover();
    bool NeedsRecovery() const { return device_lost_; }
    bool SaveSnapshot(const wchar_t* path);

    ID2D1DeviceContext* Dc() const { return dc_.get(); }
    IDWriteFactory2* DwriteFactory() const { return dwriteFactory_.get(); }
    HWND Hwnd() const { return hwnd_; }
    bool AdapterDescription(DXGI_ADAPTER_DESC& description) const {
        ComPtr<IDXGIAdapter> adapter;
        return dxgiDevice_.get() && SUCCEEDED(dxgiDevice_->GetAdapter(&adapter)) &&
            SUCCEEDED(adapter->GetDesc(&description));
    }

    void RecreateTextFormats(float scale);
    bool UpdateTextRenderingParams(HMONITOR monitor);
    IDWriteTextFormat* TextFormat() const { return textFormat_.get(); }
    IDWriteTextFormat* FileNameFormat() const { return fileNameFormat_.get(); }
    IDWriteTextFormat* SmallFormat() const { return smallFormat_.get(); }
    IDWriteTextFormat* HeaderFormat() const { return headerFormat_.get(); }
    IDWriteTextFormat* TabFormat() const { return tabFormat_.get(); }
    IDWriteTextFormat* AddressFormat() const { return addressFormat_.get(); }
    IDWriteTextFormat* IconFormat() const { return iconFormat_.get(); }
    bool DrawLumaText(std::wstring_view text, IDWriteTextFormat* format,
                      const D2D1_RECT_F& bounds, const D2D1_COLOR_F& foreground,
                      const D2D1_COLOR_F& background,
                      DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING);
    // dc->DrawTextLayout, with LumaText glyphs when the UI text uses LumaText
    // (same look as the file list); DirectWrite draws otherwise.
    void DrawTextLayout(D2D1_POINT_2F origin, IDWriteTextLayout* layout,
                        ID2D1SolidColorBrush* brush,
                        D2D1_DRAW_TEXT_OPTIONS options = D2D1_DRAW_TEXT_OPTIONS_NONE);
    bool DrawLumaTextSplit(std::wstring_view text, IDWriteTextFormat* format,
                           const D2D1_RECT_F& bounds, const D2D1_COLOR_F& foreground,
                           std::uint32_t split, const D2D1_COLOR_F& split_foreground,
                           const D2D1_COLOR_F& background);
    bool MeasureLumaText(std::wstring_view text, IDWriteTextFormat* format,
                         float& width, float* height = nullptr);
    bool PaintLumaEdit(HWND hwnd, HDC hdc, IDWriteTextFormat* format,
                       const D2D1_COLOR_F& foreground, const D2D1_COLOR_F& background);
    bool DrawLumaEditOnSurface(HWND hwnd, IDWriteTextFormat* format,
        const D2D1_COLOR_F& foreground, const D2D1_COLOR_F& background, const D2D1_RECT_F& bounds);
    bool PresentLumaEdit(HWND hwnd, IDWriteTextFormat* format,
                         const D2D1_COLOR_F& foreground, const D2D1_COLOR_F& background,
                         bool painted_plain_child = false);
    LRESULT CallLumaEditMouse(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                               IDWriteTextFormat* format);
    void SynchronizeEditSelection(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    bool LumaTextAvailable() const noexcept;
    bool LumaTextEnabled() const noexcept; // Selected UI backend, including accessibility policy.
    bool CustomEditEnabled() const noexcept; // DirectWrite input surfaces in all text modes.
    const LumaTextStats* GetLumaTextStats() const noexcept;

    int Width() const { return width_; }
    int Height() const { return height_; }
    bool UsesTransparentComposition() const { return transparentComposition_; }

    // Menu acrylic samples what the window last showed. When enabled (process
    // wide), Present() first copies the finished frame into a GPU bitmap on the
    // same device; FrameCopy() is null until a frame has been presented.
    static void SetFrameCopyEnabled(bool enabled) noexcept;
    static bool FrameCopyEnabled() noexcept;
    ID2D1Bitmap1* FrameCopy() const { return frameCopy_.get(); }
    // Counts the copies, so a reader can tell the frame has changed.
    unsigned long long FrameCopySerial() const { return frameCopySerial_; }

    // Dialog acrylic (window_helpers BeginSurface): the Pulse window right
    // under a dialog, blurred like menu acrylic. window_material registers
    // the builder; targets built without it keep the plain dialog surface.
    using AcrylicPlateBuilder = bool (*)(ID2D1DeviceContext* dc, ID2D1Bitmap1* frame, POINT frame_origin,
                                         const RECT& card, float scale, bool dark, ComPtr<ID2D1Bitmap1>& plate);
    static void SetAcrylicPlateBuilder(AcrylicPlateBuilder builder) noexcept;
    static AcrylicPlateBuilder GetAcrylicPlateBuilder() noexcept;
    // The window of this thread whose frame copy shows right under `dialog`
    // (screen rect `area`); null when another program's window is in between
    // or nothing of ours is there.
    static Compositor* FrameSourceUnder(HWND dialog, const RECT& area);

    // Theme reveal: the next Present() also keeps that frame in HeldFrame()
    // until ReleaseHeldFrame() (or a resize / device loss drops it).
    void CaptureNextFrame() { captureNext_ = true; }
    void CancelCaptureNextFrame() { captureNext_ = false; }
    ID2D1Bitmap1* HeldFrame() const { return held_.get(); }
    void ReleaseHeldFrame() { held_.reset(); }

    // Whole-window zoom on the DirectComposition root visual (runs on the
    // compositor thread, so no frames are pumped). origin is in client pixels.
    // HideContent() makes the visual transparent until PlayZoom starts, so a
    // re-shown window never flashes its previous frame. Return false when
    // composition is unavailable; callers then show/hide without motion.
    bool HideContent();
    bool PlayZoom(float origin_x, float origin_y, bool opening, double duration_s);
    void ClearZoom();

private:
    bool CheckGraphics(HRESULT hr, const wchar_t* stage);
    std::wstring initialization_error_;
    bool InitD3D();
    bool CreateSwapChain();
    void CopyFrame();
    void ResizeSwapChain();
    static bool IsDeviceLost(HRESULT hr);

    HWND hwnd_ = nullptr;
    bool composition_above_children_ = false;
    int width_ = 0;
    int height_ = 0;

    ComPtr<ID3D11Device> d3dDevice_;
    ComPtr<IDXGIDevice1> dxgiDevice_;
    ComPtr<IDXGISwapChain1> swapChain_;
    ComPtr<IDCompositionDevice> compositionDevice_;
    ComPtr<IDCompositionTarget> compositionTarget_;
    ComPtr<IDCompositionVisual> compositionVisual_;
    ComPtr<ID2D1Factory1> d2dFactory_;
    ComPtr<ID2D1Device> d2dDevice_;
    ComPtr<ID2D1DeviceContext> dc_;
    ComPtr<ID2D1Bitmap1> targetBitmap_;
    ComPtr<ID2D1Bitmap1> frameCopy_;
    unsigned long long frameCopySerial_ = 0;
    ComPtr<ID2D1Bitmap1> held_;
    bool captureNext_ = false;
    ComPtr<IDWriteFactory2> dwriteFactory_;
    ComPtr<IDWriteRenderingParams2> textRenderingParams_;
    ComPtr<IDWriteTextFormat> textFormat_;
    ComPtr<IDWriteTextFormat> fileNameFormat_;
    ComPtr<IDWriteTextFormat> smallFormat_;
    ComPtr<IDWriteTextFormat> headerFormat_;
    ComPtr<IDWriteTextFormat> tabFormat_;
    ComPtr<IDWriteTextFormat> addressFormat_;
    ComPtr<IDWriteTextFormat> iconFormat_;
    std::unique_ptr<LumaTextRenderer> lumaText_;
    HMONITOR text_params_monitor_ = nullptr;
    int text_params_mode_ = 0;
    bool transparentComposition_ = false;
    bool device_lost_ = false;
    bool partial_swap_ = false;   // FLIP_SEQUENTIAL: buffers keep their pixels
    bool full_next_ = true;       // new buffers, a skipped or occluded present
};

} // namespace pulse::ui

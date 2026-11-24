#pragma once
#include "ui_compositor.h"
#include <functional>

namespace pulse::ui {
class NativeMenuBackdrop {
public:
    virtual ~NativeMenuBackdrop() = default;
    virtual bool Update(UINT width, UINT height, float radius) noexcept = 0;
    virtual bool PaintForeground(ID2D1DeviceContext* dc, const D2D1_RECT_F& card,
                                 float radius, bool dark) noexcept = 0;
};
// A same-device DirectComposition surface on the menu HWND itself.
class NativeMenuSurface {
public:
    using BackdropFactory = std::function<std::unique_ptr<NativeMenuBackdrop>(HWND)>;
    ~NativeMenuSurface();
    HRESULT Initialize(HWND hwnd, ID2D1DeviceContext* dc, bool dark, const BackdropFactory& factory);
    HRESULT Present(ID2D1DeviceContext* dc, ID2D1Bitmap1* bitmap, POINT origin, float radius);
    bool PaintForeground(ID2D1DeviceContext* dc, const D2D1_RECT_F& card, float radius, bool dark) noexcept;
    void Reset() noexcept;
    bool Ready() const noexcept { return swap_.get() != nullptr; }
private:
    std::unique_ptr<NativeMenuBackdrop> backdrop_;
    bool fail_present_for_testing_ = false;
    HWND hwnd_ = nullptr;
    LONG_PTR original_ex_style_ = 0;
    UINT width_ = 1, height_ = 1;
    ComPtr<IDXGISwapChain1> swap_;
    ComPtr<IDCompositionDevice> composition_;
    ComPtr<IDCompositionTarget> target_;
    ComPtr<IDCompositionVisual> visual_;
    ComPtr<ID2D1Bitmap1> buffer_;
};
}

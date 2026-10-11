// d2d_device_binding.h - Binds a render cache to the Direct2D device behind a
// window's context (multi-window M3d).
//
// Bitmaps belong to the D2D device, not to the context that created them, and
// every Pulse window on the UI thread draws through one shared device
// (Compositor::InitD3D). A cache keyed on a window's context dropped all its
// bitmaps whenever another window painted, and kept a raw pointer to a context
// that goes away with its window. The binding holds the device and a private
// context on it for creating bitmaps, so only a different device (device loss)
// invalidates the cache.
#pragma once
#include "ui_compositor.h"

#include <utility>

namespace pulse::ui {

class D2DDeviceBinding {
public:
    // Binds to the device behind `window_dc` (nullptr unbinds). Returns true
    // when the device changed: bitmaps created before must be dropped.
    bool Bind(ID2D1DeviceContext* window_dc) {
        ComPtr<ID2D1Device> device;
        if (window_dc) window_dc->GetDevice(&device);
        // A context that failed to create is retried on the next bind.
        if (device.get() == device_.get() && (dc_.get() || !device.get())) return false;
        dc_.reset();
        if (device.get()) device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_);
        device_ = std::move(device);
        return true;
    }
    // Creates bitmaps that draw into any context of the bound device.
    ID2D1DeviceContext* Dc() const { return dc_.get(); }
    ID2D1Device* Device() const { return device_.get(); }

private:
    ComPtr<ID2D1Device> device_;
    ComPtr<ID2D1DeviceContext> dc_;
};

}  // namespace pulse::ui

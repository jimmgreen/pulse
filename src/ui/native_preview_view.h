#pragma once
#include "thumbnail_cache.h"
#include <optional>

namespace pulse::ui {
class ExtensionPanel;
// One independent consumer per Quick Look or embedded details preview.
class NativePreviewView {
    friend struct NativePreviewProbe;
public:
    NativePreviewView();
    ~NativePreviewView();
    NativePreviewView(const NativePreviewView&) = delete;
    NativePreviewView& operator=(const NativePreviewView&) = delete;
    bool Available(HWND owner, const std::wstring& path);
    void Reset();
    bool HasResult() const noexcept { return result_ != nullptr; }
    std::optional<PreviewDrawResult> Draw(HWND owner, HWND notify, ID2D1DeviceContext* dc,
        const D2D1_RECT_F& bounds, const std::wstring& path, uint64_t modified,
        uint64_t size, uint32_t pixels, std::shared_ptr<const std::wstring>& text,
        bool& truncated, preview::Integrity& integrity, uint32_t& width, uint32_t& height,
        bool dark, D2D1_COLOR_F accent, float scale);
    bool CaptureModel(const wchar_t* path);
private:
    const uint64_t surface_;
    HWND owner_ = nullptr;
    std::shared_ptr<const void> result_;
    ComPtr<ID2D1Bitmap> bitmap_;
    ID2D1DeviceContext* device_ = nullptr;
    // shared_ptr binds its deleter where the panel is created, so builds
    // without the extension runtime need no ExtensionPanel definition.
    std::shared_ptr<ExtensionPanel> model_;
};
}

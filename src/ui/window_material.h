// window_material.h — DWM backdrop + custom-image sampling for Mica / Acrylic.
#pragma once
#include "ui_compositor.h"
#include <string>
#include <string_view>

namespace pulse::ui {

enum class WindowEffect {
    None = 0,
    Acrylic,
    Mica,
    MicaAlt
};

inline constexpr int kWindowEffectCount = 4;

const wchar_t* WindowEffectId(WindowEffect effect) noexcept;
const wchar_t* WindowEffectLabel(WindowEffect effect) noexcept;
WindowEffect WindowEffectFromId(std::wstring_view id) noexcept;
bool WindowEffectUsesBackdrop(WindowEffect effect) noexcept;
bool ApplyWindowEffect(HWND hwnd, WindowEffect effect, bool dark) noexcept;

int RunMaterialSelfTest();
void LogWindowMaterial(const char* fmt, ...);

// Samples a user image with the WinUI homemade-Mica D2D graph (blur, then
// luminosity blend, then color blend). DWM cannot retarget Mica at a bitmap.
class WindowMaterial {
public:
    void SetCompositor(Compositor* compositor);
    void Invalidate();

    ID2D1Bitmap* SourceBitmap(const std::wstring& path);
    bool DrawSourceCover(ID2D1DeviceContext* dc, const D2D1_RECT_F& dest,
                         const std::wstring& path);

    // Paints the sampled material. Returns false when there is nothing to sample
    // (no path, None, high contrast, or decode failure) so the caller can use DWM.
    bool DrawBackdrop(ID2D1DeviceContext* dc, const D2D1_RECT_F& dest,
                      WindowEffect effect, bool dark, const std::wstring& path);

private:
    struct Recipe {
        float blur_std = 80.0f;
        float tint_opacity = 0.5f;
        float luminosity_opacity = 1.0f;
        D2D1_COLOR_F tint{};
    };

    static Recipe RecipeFor(WindowEffect effect, bool dark) noexcept;
    ID2D1Bitmap* EnsureSampled(ID2D1DeviceContext* dc, const D2D1_RECT_F& dest,
                               WindowEffect effect, bool dark, const std::wstring& path);

    Compositor* compositor_ = nullptr;
    ComPtr<ID2D1Bitmap> source_;
    std::wstring source_path_;
    std::wstring source_failed_;
    ID2D1DeviceContext* source_dc_ = nullptr;
    ComPtr<ID2D1Bitmap> sampled_;
    std::wstring sampled_path_;
    WindowEffect sampled_effect_ = WindowEffect::None;
    bool sampled_dark_ = false;
    int sampled_w_ = 0;
    int sampled_h_ = 0;
    ID2D1DeviceContext* sampled_dc_ = nullptr;
};

} // namespace pulse::ui

// window_material.h — DWM backdrop + custom-image sampling for Mica / Acrylic.
#pragma once
#include "background_image_layout.h"
#include "ui_compositor.h"
#include "window_effect_support.h"
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::ui {

inline constexpr int kWindowEffectCount = 4;

const wchar_t* WindowEffectId(WindowEffect effect) noexcept;
const wchar_t* WindowEffectLabel(WindowEffect effect) noexcept;
bool WindowEffectUsesBackdrop(WindowEffect effect) noexcept;
bool ApplyWindowEffect(HWND hwnd, WindowEffect effect, bool dark) noexcept;

// In-window acrylic (multi-window M7i capsules). SampleTarget copies what the
// current frame already holds under `rect` (plus `margin` for the blur) out of
// the device context's target bitmap; it fails on targets that are not
// bitmaps or under a scaling transform, and the caller then paints a solid
// plate. DrawAcrylicSample blurs that copy through the same graph as the
// window material with the WinUI in-app acrylic recipe and paints it clipped
// to the rounded rect.
struct TargetSample {
    ComPtr<ID2D1Bitmap1> bitmap;
    D2D1_POINT_2F origin{};
};
bool SampleTarget(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, float margin, TargetSample& sample) noexcept;
bool DrawAcrylicSample(ID2D1DeviceContext* dc, const TargetSample& sample, const D2D1_RECT_F& rect, float radius,
                       bool dark, float blur_std) noexcept;
// Same graph with explicit opacities (menus use a clearer plate than capsules).
bool DrawAcrylicSample(ID2D1DeviceContext* dc, const TargetSample& sample, const D2D1_RECT_F& rect, float radius,
                       bool dark, float blur_std, float luminosity_opacity, float tint_opacity) noexcept;

// Popup acrylic (menus) follows the system: off under high contrast, when
// Windows "Transparency effects" is off, and while battery saver is on.
bool SystemAllowsPopupAcrylic() noexcept;

// A popup's acrylic plate (menus, dialogs): the part of a window's frame
// copy under `card` (screen pixels; the frame's pixel 0,0 sits at
// `frame_origin`), blurred and tinted, card-sized. Where the card hangs past
// the window the frame's edge pixels are stretched over the rest. The device
// context's target is restored afterwards.
bool BuildAcrylicPlate(ID2D1DeviceContext* dc, ID2D1Bitmap1* frame, POINT frame_origin, const RECT& card,
                       float blur, bool dark, float luminosity, float tint, ComPtr<ID2D1Bitmap1>& plate) noexcept;

int RunMaterialSelfTest();
void LogWindowMaterial(const char* fmt, ...);

// Layout, opacity and identity of a user image painted by DrawSourceCover.
// The ints mirror the persisted background_* preferences; `version` changes
// with every published image so a new one always rebakes.
struct SourceDrawParams {
    int position = 4;
    int ratio_percent = 50;
    int fit = 1;
    float opacity = 1.0f;
    uint64_t version = 0;
};

// Identity of a baked bitmap. The content fields must match to reuse a bake;
// the size fields may drift within the rebake tolerance while the user drags
// the window edge.
struct SourceBakeKey {
    std::wstring path;
    uint64_t version = 0;
    int position = 4;
    int ratio_percent = 50;
    int fit = 1;
    float blur_px = 0.0f;
    int width = 0;
    int height = 0;

    // path, version, position, ratio and fit equal and the blur radius within
    // 0.5 px (the radius arrives as a scaled DIP value).
    bool SameContent(const SourceBakeKey& other) const noexcept;
    bool SizeWithin(const SourceBakeKey& other, int tolerance_w, int tolerance_h) const noexcept;
};

// Samples a user image with the WinUI homemade-Mica D2D graph (blur, then
// luminosity blend, then color blend). DWM cannot retarget Mica at a bitmap.
class WindowMaterial {
public:
    WindowMaterial() = default;
    ~WindowMaterial();
    WindowMaterial(const WindowMaterial&) = delete;
    WindowMaterial& operator=(const WindowMaterial&) = delete;

    // Main-window wallpaper-sampling graph, not native DWM. Blur uses source pixels.
    static HRESULT BuildMicaImage(ID2D1DeviceContext* dc, ID2D1Image* input,
        bool alternate, bool dark, float blur_std, ID2D1Image** output) noexcept;
    static HRESULT BuildMicaImageWithProfile(ID2D1DeviceContext* dc, ID2D1Image* input,
        bool alternate, bool dark, float blur_std, float luminosity_opacity,
        float tint_opacity, ID2D1Image** output) noexcept;
    void SetCompositor(Compositor* compositor);
    void Invalidate();

    ID2D1Bitmap* SourceBitmap(const std::wstring& path);
    // Defaults to a full-window cover (position 4, fit cover, opaque), which is
    // what every caller drew before the placement options existed.
    // blur_px > 0 paints a Gaussian-blurred cover baked once per path/size/radius.
    bool DrawSourceCover(ID2D1DeviceContext* dc, const D2D1_RECT_F& dest,
                         const std::wstring& path, float blur_px = 0.0f);
    // Same, with the persisted placement/fit/opacity. The image is clipped to
    // the placement region and the bake is cached per params.version.
    bool DrawSourceCover(ID2D1DeviceContext* dc, const D2D1_RECT_F& dest,
                         const std::wstring& path, const SourceDrawParams& params, float blur_px);

    // Paints the sampled material. Returns false when there is nothing to sample
    // (no path, None, high contrast, or decode failure) so the caller can use DWM.
    bool DrawBackdrop(ID2D1DeviceContext* dc, const D2D1_RECT_F& dest,
                      WindowEffect effect, bool dark, const std::wstring& path);
    // Same material, still over the whole window: only params.version selects
    // the bake (a published image forces a rebake). position, ratio_percent
    // and fit are ignored on purpose, because the material never follows the
    // image placement; sampling a sub-rect would leave the rest of the bake
    // at its cleared colour and show up as a black block. params.opacity fades
    // the sample toward the material's base tint instead of drawing it
    // translucent, so the window never opens onto the desktop.
    bool DrawBackdrop(ID2D1DeviceContext* dc, const D2D1_RECT_F& dest,
                      WindowEffect effect, bool dark, const std::wstring& path,
                      const SourceDrawParams& params);

#ifdef PULSE_WINDOW_MATERIAL_TESTING
    void SetDecodeDelayForTesting(DWORD delay_ms);
    uint64_t DecodeAttemptsForTesting() const;
    bool DecodeFailedForTesting(const std::wstring& path);
    D2D1_SIZE_U SourceSizeForTesting() const { return {source_width_, source_height_}; }
#endif

private:
    struct DecodeWorker;

    struct Recipe {
        float blur_std = 80.0f;
        float tint_opacity = 0.5f;
        float luminosity_opacity = 1.0f;
        D2D1_COLOR_F tint{};
    };

    static Recipe RecipeFor(WindowEffect effect, bool dark) noexcept;
    void EnsureDecodeWorker();
    ID2D1Bitmap* SourceBitmapAt(const std::wstring& path, UINT max_edge);
    void QueueDecode(const std::wstring& path, UINT max_edge);
    void TakeDecodeResult();
    void ResetGpuResources();
    static DWORD WINAPI DecodeWorkerMain(void* parameter);
    ID2D1Bitmap* EnsureBlurred(ID2D1DeviceContext* dc, const D2D1_RECT_F& dest,
                               const std::wstring& path, const SourceDrawParams& params,
                               float blur_px);
    ID2D1Bitmap* EnsureSampled(ID2D1DeviceContext* dc, const D2D1_RECT_F& dest,
                               WindowEffect effect, bool dark, const std::wstring& path,
                               const SourceDrawParams& params);

    Compositor* compositor_ = nullptr;
    std::shared_ptr<DecodeWorker> decode_worker_;
    std::wstring requested_path_;
    UINT requested_edge_ = 0;
    uint64_t requested_generation_ = 0;
    std::vector<uint8_t> source_pixels_;
    std::wstring source_pixels_path_;
    UINT source_width_ = 0;
    UINT source_height_ = 0;
    UINT source_stride_ = 0;
    ComPtr<ID2D1Bitmap1> source_;
    std::wstring source_path_;
    std::wstring source_failed_;
    ID2D1DeviceContext* source_dc_ = nullptr;
    ComPtr<ID2D1Bitmap> sampled_;
    SourceBakeKey sampled_key_;
    WindowEffect sampled_effect_ = WindowEffect::None;
    bool sampled_dark_ = false;
    ID2D1DeviceContext* sampled_dc_ = nullptr;
    ComPtr<ID2D1Bitmap> blurred_;
    SourceBakeKey blurred_key_;
    ID2D1DeviceContext* blurred_dc_ = nullptr;
};

} // namespace pulse::ui

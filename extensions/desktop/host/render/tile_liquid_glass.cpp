// tile_liquid_glass.cpp - macOS Tahoe-style Liquid Glass single-pass HLSL renderer for desktop tiles.
#include "host/render/tile_liquid_glass.h"

#include <d2d1effectauthor.h>
#include <d3dcommon.h>

// Generated at build time by fxc.exe from host/render/shaders/tile_liquid_glass.hlsl:
#include "tile_liquid_glass_ps.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <new>

namespace pulse::desktop::render {

using Microsoft::WRL::ComPtr;

namespace {

// {6A9D2F14-7C81-4B5E-9213-4F8EA23B7109}
constexpr GUID CLSID_PulseTileLiquidGlass = {
    0x6a9d2f14, 0x7c81, 0x4b5e, {0x92, 0x13, 0x4f, 0x8e, 0xa2, 0x3b, 0x71, 0x09}};
// {8C4E1B72-3D29-4F0A-A567-1D9C4452E831}
constexpr GUID CLSID_PulseTileLiquidGlassShader = {
    0x8c4e1b72, 0x3d29, 0x4f0a, {0xa5, 0x67, 0x1d, 0x9c, 0x44, 0x52, 0xe8, 0x31}};

struct alignas(16) TileGlassShaderConstants {
    float rect_x, rect_y, rect_w, rect_h;
    float radius_px, bezel_px, ior, bezel_slope;
    float dmax_px, inv_snell_max, chromatic_spread, dpi_scale;
    float sigma_px, frost_amount, vibrancy, overall_opacity;
    float tint_r, tint_g, tint_b, tint_a;
    float white_veil_alpha, dither_alpha, rim_gain, has_frost_tex;
    float bounds_min_x, bounds_min_y, bounds_max_x, bounds_max_y;
    float sharp_scale_x, sharp_scale_y, frost_scale_x, frost_scale_y;
    float sharp_max_x, sharp_max_y, frost_max_x, frost_max_y;
    float light_norm_x, light_norm_y, rim_stroke_strength, rim_flags;
};
static_assert(sizeof(TileGlassShaderConstants) == 160);

struct TileGlassEffectPayload {
    TileGlassShaderConstants constants{};
    GlassIntRect output_rect_px{};
    GlassIntRect input_bounds_px{};
    int32_t sharp_w = 1;
    int32_t sharp_h = 1;
    int32_t frost_w = 1;
    int32_t frost_h = 1;
    int32_t sample_pad_px = 16;
};

class TileLiquidGlassDrawTransform final : public ID2D1DrawTransform {
public:
    TileLiquidGlassDrawTransform() = default;

    IFACEMETHODIMP_(ULONG) AddRef() override {
        return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG count = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (count == 0) delete this;
        return count;
    }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(ID2D1TransformNode) ||
            riid == __uuidof(ID2D1Transform) ||
            riid == __uuidof(ID2D1DrawTransform)) {
            *ppv = static_cast<ID2D1DrawTransform*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    IFACEMETHODIMP_(UINT32) GetInputCount() const override { return 2; }

    IFACEMETHODIMP SetDrawInfo(ID2D1DrawInfo* draw_info) override {
        if (!draw_info) return E_INVALIDARG;
        draw_info_ = draw_info;
        D2D1_INPUT_DESCRIPTION desc{};
        desc.filter = D2D1_FILTER_MIN_MAG_MIP_LINEAR;
        desc.levelOfDetailCount = 0;
        HRESULT hr = draw_info_->SetInputDescription(0, desc);
        if (FAILED(hr)) return hr;
        hr = draw_info_->SetInputDescription(1, desc);
        if (FAILED(hr)) return hr;
        draw_info_->SetInstructionCountHint(128);
        hr = draw_info_->SetPixelShader(CLSID_PulseTileLiquidGlassShader, D2D1_PIXEL_OPTIONS_NONE);
        if (FAILED(hr)) return hr;
        return PushConstants();
    }

    IFACEMETHODIMP MapOutputRectToInputRects(const D2D1_RECT_L* output_rect,
                                             D2D1_RECT_L* input_rects,
                                             UINT32 input_rects_count) const override {
        if (!output_rect || !input_rects || input_rects_count != 2) return E_INVALIDARG;
        const int32_t pad = std::clamp(payload_.sample_pad_px, 4, kGlassMaxInputHalo);
        const auto bounded = BoundedGlassInputRect(
            {output_rect->left, output_rect->top, output_rect->right, output_rect->bottom},
            payload_.input_bounds_px, pad);

        auto map_to_native = [](const GlassIntRect& scene_r,
                                float bx, float by,
                                float sx, float sy,
                                int32_t max_w, int32_t max_h) noexcept -> D2D1_RECT_L {
            const int32_t safe_w = std::max(1, max_w);
            const int32_t safe_h = std::max(1, max_h);
            int32_t l = static_cast<int32_t>(std::floor((static_cast<float>(scene_r.left) - bx) * sx)) - 1;
            int32_t t = static_cast<int32_t>(std::floor((static_cast<float>(scene_r.top) - by) * sy)) - 1;
            int32_t r = static_cast<int32_t>(std::ceil((static_cast<float>(scene_r.right) - bx) * sx)) + 1;
            int32_t b = static_cast<int32_t>(std::ceil((static_cast<float>(scene_r.bottom) - by) * sy)) + 1;
            l = std::clamp(l, 0, safe_w - 1);
            t = std::clamp(t, 0, safe_h - 1);
            r = std::clamp(r, l + 1, safe_w);
            b = std::clamp(b, t + 1, safe_h);
            return D2D1_RECT_L{l, t, r, b};
        };

        const auto& c = payload_.constants;
        input_rects[0] = map_to_native(bounded, c.bounds_min_x, c.bounds_min_y,
                                       c.sharp_scale_x, c.sharp_scale_y,
                                       payload_.sharp_w, payload_.sharp_h);
        input_rects[1] = map_to_native(bounded, c.bounds_min_x, c.bounds_min_y,
                                       c.frost_scale_x, c.frost_scale_y,
                                       payload_.frost_w, payload_.frost_h);
        return S_OK;
    }

    IFACEMETHODIMP MapInputRectsToOutputRect(const D2D1_RECT_L* input_rects,
                                             const D2D1_RECT_L* /*input_opaque_sub_rects*/,
                                             UINT32 input_rect_count,
                                             D2D1_RECT_L* output_rect,
                                             D2D1_RECT_L* output_opaque_sub_rect) override {
        if (!input_rects || !output_rect || !output_opaque_sub_rect || input_rect_count != 2) {
            return E_INVALIDARG;
        }
        const auto& r = payload_.output_rect_px;
        *output_rect = D2D1_RECT_L{r.left, r.top, r.right, r.bottom};
        *output_opaque_sub_rect = D2D1_RECT_L{0, 0, 0, 0};
        return S_OK;
    }

    IFACEMETHODIMP MapInvalidRect(UINT32 input_index,
                                  D2D1_RECT_L invalid_input_rect,
                                  D2D1_RECT_L* invalid_output_rect) const override {
        if (!invalid_output_rect || input_index >= 2) return E_INVALIDARG;
        const auto& c = payload_.constants;
        const float inv_sx = 1.0f / std::max(1e-5f, input_index == 0 ? c.sharp_scale_x : c.frost_scale_x);
        const float inv_sy = 1.0f / std::max(1e-5f, input_index == 0 ? c.sharp_scale_y : c.frost_scale_y);
        const LONG pad = static_cast<LONG>(std::clamp(payload_.sample_pad_px, 4, kGlassMaxInputHalo));
        *invalid_output_rect = D2D1_RECT_L{
            static_cast<LONG>(std::floor(c.bounds_min_x + static_cast<float>(invalid_input_rect.left) * inv_sx)) - pad,
            static_cast<LONG>(std::floor(c.bounds_min_y + static_cast<float>(invalid_input_rect.top) * inv_sy)) - pad,
            static_cast<LONG>(std::ceil(c.bounds_min_x + static_cast<float>(invalid_input_rect.right) * inv_sx)) + pad,
            static_cast<LONG>(std::ceil(c.bounds_min_y + static_cast<float>(invalid_input_rect.bottom) * inv_sy)) + pad,
        };
        return S_OK;
    }

    void SetPayload(const TileGlassEffectPayload& payload) noexcept {
        payload_ = payload;
        dirty_ = true;
    }

    const TileGlassEffectPayload& Payload() const noexcept { return payload_; }

    HRESULT PushConstants() noexcept {
        if (!draw_info_) return S_OK;
        dirty_ = false;
        return draw_info_->SetPixelShaderConstantBuffer(
            reinterpret_cast<const BYTE*>(&payload_.constants),
            static_cast<UINT32>(sizeof(payload_.constants)));
    }

    bool Dirty() const noexcept { return dirty_; }

private:
    ~TileLiquidGlassDrawTransform() = default;

    std::atomic<ULONG> ref_count_{1};
    ComPtr<ID2D1DrawInfo> draw_info_;
    TileGlassEffectPayload payload_{};
    bool dirty_ = true;
};

class TileLiquidGlassEffectImpl final : public ID2D1EffectImpl {
public:
    static HRESULT CALLBACK CreateEffect(IUnknown** effect_impl) noexcept {
        if (!effect_impl) return E_POINTER;
        auto* inst = new (std::nothrow) TileLiquidGlassEffectImpl();
        if (!inst) return E_OUTOFMEMORY;
        *effect_impl = static_cast<ID2D1EffectImpl*>(inst);
        return S_OK;
    }

    IFACEMETHODIMP_(ULONG) AddRef() override {
        return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG count = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (count == 0) delete this;
        return count;
    }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID2D1EffectImpl)) {
            *ppv = static_cast<ID2D1EffectImpl*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    IFACEMETHODIMP Initialize(ID2D1EffectContext* ctx, ID2D1TransformGraph* graph) override {
        if (!ctx || !graph) return E_INVALIDARG;
        if (!ctx->IsShaderLoaded(CLSID_PulseTileLiquidGlassShader)) {
            const HRESULT hr = ctx->LoadPixelShader(
                CLSID_PulseTileLiquidGlassShader,
                g_tile_liquid_glass_ps,
                static_cast<UINT32>(sizeof(g_tile_liquid_glass_ps)));
            if (FAILED(hr)) return hr;
        }
        draw_transform_.Attach(new (std::nothrow) TileLiquidGlassDrawTransform());
        if (!draw_transform_) return E_OUTOFMEMORY;
        return graph->SetSingleTransformNode(draw_transform_.Get());
    }

    IFACEMETHODIMP PrepareForRender(D2D1_CHANGE_TYPE /*change_type*/) override {
        if (draw_transform_ && draw_transform_->Dirty()) {
            return draw_transform_->PushConstants();
        }
        return S_OK;
    }

    IFACEMETHODIMP SetGraph(ID2D1TransformGraph* /*graph*/) override { return E_NOTIMPL; }

    static HRESULT CALLBACK SetParamsThunk(IUnknown* effect, const BYTE* data, UINT32 size) noexcept {
        if (!effect) return E_INVALIDARG;
        return static_cast<TileLiquidGlassEffectImpl*>(static_cast<ID2D1EffectImpl*>(effect))->SetParams(data, size);
    }

    static HRESULT CALLBACK GetParamsThunk(const IUnknown* effect, BYTE* data, UINT32 size, UINT32* actual) noexcept {
        if (!effect) return E_INVALIDARG;
        return static_cast<const TileLiquidGlassEffectImpl*>(static_cast<const ID2D1EffectImpl*>(effect))
            ->GetParams(data, size, actual);
    }

    HRESULT SetParams(const BYTE* data, UINT32 size) noexcept {
        if (!data || size != sizeof(TileGlassEffectPayload) || !draw_transform_) return E_INVALIDARG;
        TileGlassEffectPayload payload{};
        std::memcpy(&payload, data, sizeof(payload));
        draw_transform_->SetPayload(payload);
        return S_OK;
    }

    HRESULT GetParams(BYTE* data, UINT32 size, UINT32* actual) const noexcept {
        if (actual) *actual = static_cast<UINT32>(sizeof(TileGlassEffectPayload));
        if (!data) return S_OK;
        if (size < sizeof(TileGlassEffectPayload) || !draw_transform_) return E_INVALIDARG;
        std::memcpy(data, &draw_transform_->Payload(), sizeof(TileGlassEffectPayload));
        return S_OK;
    }

private:
    TileLiquidGlassEffectImpl() = default;
    ~TileLiquidGlassEffectImpl() = default;

    std::atomic<ULONG> ref_count_{1};
    ComPtr<TileLiquidGlassDrawTransform> draw_transform_;
};

std::mutex g_tile_reg_mtx;
std::vector<ID2D1Factory1*> g_tile_registered_factories;

bool EnsureTileLiquidGlassEffectRegistered(ID2D1Factory1* factory) noexcept {
    if (!factory) return false;
    std::lock_guard<std::mutex> lock(g_tile_reg_mtx);
    for (ID2D1Factory1* existing : g_tile_registered_factories) {
        if (existing == factory) return true;
    }
    static constexpr PCWSTR kEffectXml =
        L"<?xml version='1.0'?>"
        L"<Effect>"
        L"  <Property name='DisplayName' type='string' value='PulseTileLiquidGlass'/>"
        L"  <Property name='Author' type='string' value='Pulse'/>"
        L"  <Property name='Category' type='string' value='Material'/>"
        L"  <Property name='Description' type='string' value='Tahoe Tile Liquid Glass'/>"
        L"  <Inputs>"
        L"    <Input name='Sharp'/>"
        L"    <Input name='Frost'/>"
        L"  </Inputs>"
        L"  <Property name='Params' type='blob'>"
        L"    <Property name='DisplayName' type='string' value='Params'/>"
        L"  </Property>"
        L"</Effect>";
    const D2D1_PROPERTY_BINDING bindings[] = {
        {L"Params", &TileLiquidGlassEffectImpl::SetParamsThunk, &TileLiquidGlassEffectImpl::GetParamsThunk},
    };
    if (FAILED(factory->RegisterEffectFromString(
            CLSID_PulseTileLiquidGlass, kEffectXml, bindings, ARRAYSIZE(bindings),
            &TileLiquidGlassEffectImpl::CreateEffect))) {
        return false;
    }
    try {
        g_tile_registered_factories.push_back(factory);
    } catch (...) {}
    return true;
}

inline float ComputeSnellPeakValue(float ior, float slope) noexcept {
    // Superellipse bezel derivative dh(x) is strictly decreasing in x -> peak occurs at x = 1e-4.
    const float safe_ior = std::max(1.01f, ior);
    const float safe_slope = std::max(0.1f, slope);
    constexpr float x = 1e-4f;
    const float u = 1.0f - x;
    const float u3 = u * u * u;
    const float u4 = u3 * u;
    const float dh = u3 / std::pow(std::max(1e-6f, 1.0f - u4), 0.75f) * safe_slope;
    const float theta_in = std::atan(dh);
    const float sin_out = std::clamp(std::sin(theta_in) / safe_ior, -0.9999f, 0.9999f);
    const float theta_out = std::asin(sin_out);
    return std::max(1e-5f, std::tan(theta_in - theta_out));
}

// Exterior pads of the erf ambient+contact shadow, in DIPs. The shadow is computed per card size: a 9-slice of
// one 140 DIP card stretched its edge strips (still ramping up from the corners at 3 sigma = 60 DIP) over longer
// cards and crushed them on short ones, so every corner showed a pale notch with a seam where the strip began.
constexpr int kShadowPadSideDip = 52;
constexpr int kShadowPadTopDip = 44;
constexpr int kShadowPadBottomDip = 66;
constexpr float kShadowTexelDip = 2.0f;  // the blur is 20 DIP, the contact shadow 6 DIP
constexpr size_t kShadowCacheMax = 32;

inline float RoundedRectSdf(float px, float py, float hx, float hy, float r) noexcept {
    const float qx = std::fabs(px) - (hx - r);
    const float qy = std::fabs(py) - (hy - r);
    const float ox = std::max(qx, 0.0f);
    const float oy = std::max(qy, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - r;
}

TileGlassConfig MakeBaseTahoeConfig(bool dark_mode) noexcept {
    TileGlassConfig cfg{};
    if (!dark_mode) {
        // Reference TahoeClearLight preset (gentle milky veil so dark ink reads cleanly over warm/vivid backdrops)
        cfg.refraction_strength = 0.74f;
        cfg.bezel_max_dip = 26.0f;
        cfg.max_displacement_dip = 64.0f;
        cfg.enable_chromatic_aberration = true;
        cfg.chromatic_spread = 0.13f;
        cfg.frost_amount = 0.08f;
        cfg.min_blur_dip = 0.0f;
        cfg.vibrancy = 1.80f;
        cfg.tint = D2D1::ColorF(0.98f, 0.985f, 1.00f, 0.16f);
        cfg.white_veil_alpha = 0.05f;
        cfg.dark_appearance = false;
        cfg.rim_intensity = 0.95f;
        cfg.enable_drop_shadow = true;
        cfg.shadow_blur_dip = 20.0f;
        cfg.shadow_offset_y_dip = 14.0f;
        cfg.shadow_opacity = 0.45f;
    } else {
        // Reference TahoeClearDark preset (WacOS smoked body tint so white ink stays crisp across bright scenes)
        cfg.refraction_strength = 0.72f;
        cfg.bezel_max_dip = 26.0f;
        cfg.max_displacement_dip = 64.0f;
        cfg.enable_chromatic_aberration = true;
        cfg.chromatic_spread = 0.13f;
        cfg.frost_amount = 0.16f;
        cfg.min_blur_dip = 0.0f;
        cfg.vibrancy = 1.76f;
        cfg.tint = D2D1::ColorF(0.06f, 0.07f, 0.10f, 0.28f);
        cfg.white_veil_alpha = 0.03f;
        cfg.dark_appearance = true;
        cfg.rim_intensity = 0.90f;
        cfg.enable_drop_shadow = true;
        cfg.shadow_blur_dip = 22.0f;
        cfg.shadow_offset_y_dip = 15.0f;
        cfg.shadow_opacity = 0.54f;
    }
    return cfg;
}

} // namespace

TileGlassConfig MakeTileGlassConfig(const model::BeautifySettings& settings,
                                    float corner_radius_dip,
                                    bool elevated_hover,
                                    bool energy_saver) noexcept {
    TileGlassConfig cfg = MakeBaseTahoeConfig(settings.dark_mode);
    cfg.corner_radius_dip = std::clamp(corner_radius_dip, 4.0f, 48.0f);

    const float base_refract = cfg.refraction_strength;
    const float base_frost = cfg.frost_amount;
    const float base_rim = cfg.rim_intensity;

    if (settings.glass_reference) {
        const auto& opt = settings.glass_options;
        const float thickness = static_cast<float>(std::clamp(opt.thickness, 1.0, 80.0));
        const float ior = static_cast<float>(std::clamp(opt.ior, 1.0, 4.0));
        const float disp = static_cast<float>(std::clamp(opt.dispersion, 0.0, 50.0));
        const float blur_r = static_cast<float>(std::clamp(opt.blur_radius, 1.0, 200.0));
        const float glare = static_cast<float>(std::clamp(opt.glare_factor, 0.0, 120.0));
        const float tint_a = static_cast<float>(std::clamp(opt.tint_a, 0.0, 100.0));

        // At Studio defaults (thickness=20, ior=1.4, dispersion=7, blur=1, glare=90, tint_a=0),
        // every parameter maps 1:1 to the reference Tahoe preset while sliders stay continuously responsive.
        cfg.bezel_max_dip = std::clamp(thickness * 1.3f, 6.0f, 48.0f);
        cfg.refraction_strength = std::clamp(base_refract * (thickness / 20.0f), 0.10f, 1.0f);
        cfg.index_of_refraction = std::clamp(1.50f + (ior - 1.4f) * 0.85f, 1.05f, 2.60f);
        cfg.chromatic_spread = (disp <= 0.05f) ? 0.0f : std::clamp(0.13f * (disp / 7.0f), 0.0f, 0.35f);
        cfg.enable_chromatic_aberration = (cfg.chromatic_spread > 0.001f);

        if (blur_r <= 1.05f) {
            cfg.frost_amount = base_frost;
            cfg.min_blur_dip = 0.0f;
        } else {
            cfg.frost_amount = std::clamp(base_frost + (blur_r - 1.0f) / 45.0f, 0.0f, 1.0f);
            cfg.min_blur_dip = (blur_r > 20.0f) ? std::min(22.0f, (blur_r - 20.0f) * 0.22f) : 0.0f;
        }
        cfg.rim_intensity = std::clamp(base_rim * (glare / 90.0f), 0.0f, 1.30f);

        if (tint_a > 0.5f) {
            const float tr = static_cast<float>(std::clamp(opt.tint_r, 0.0, 255.0)) / 255.0f;
            const float tg = static_cast<float>(std::clamp(opt.tint_g, 0.0, 255.0)) / 255.0f;
            const float tb = static_cast<float>(std::clamp(opt.tint_b, 0.0, 255.0)) / 255.0f;
            const float ta = std::clamp(tint_a / 100.0f, 0.0f, 0.78f);
            cfg.tint = D2D1::ColorF(tr, tg, tb, ta);
        }
    } else {
        // Curated Liquid Glass modes (5/0=清透水滴, 1=标准液态, 3=柔雾凝脂, 2=流光棱镜, 4=Custom legacy)
        switch (settings.glass_preset) {
        case 1: // 标准液态: the reference TintedAccent recipe with the theme's neutral tint (a touch more frost
                // and body than 清透水滴, so text over busy wallpapers stays readable).
            cfg.refraction_strength = 0.72f;
            cfg.bezel_max_dip = 26.0f;
            cfg.max_displacement_dip = 64.0f;
            cfg.chromatic_spread = 0.12f;
            cfg.frost_amount = 0.18f;
            cfg.min_blur_dip = 0.0f;
            cfg.vibrancy = 1.88f;
            cfg.tint = settings.dark_mode
                ? D2D1::ColorF(0.05f, 0.06f, 0.09f, 0.24f)
                : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.24f);
            cfg.white_veil_alpha = 0.04f;
            cfg.rim_intensity = 0.95f;
            cfg.shadow_blur_dip = 20.0f;
            cfg.shadow_offset_y_dip = 14.0f;
            cfg.shadow_opacity = 0.48f;
            break;
        case 2: // 流光棱镜 (Deep Prism — vivid thick crystal bevel + rainbow dispersion)
            cfg.refraction_strength = 0.94f;
            cfg.bezel_max_dip = 36.0f;
            cfg.max_displacement_dip = 72.0f;
            cfg.index_of_refraction = 1.78f;
            cfg.chromatic_spread = 0.24f;
            cfg.frost_amount = 0.04f;
            cfg.min_blur_dip = 0.0f;
            cfg.vibrancy = 2.04f;
            cfg.rim_intensity = 1.14f;
            break;
        case 3: // 柔雾凝脂: the reference GoldenGateFrost preset (its tint is the dark one; light mode keeps the
                // same body as a pale veil).
            cfg.refraction_strength = 0.62f;
            cfg.bezel_max_dip = 24.0f;
            cfg.max_displacement_dip = 60.0f;
            cfg.chromatic_spread = 0.10f;
            cfg.frost_amount = 0.56f;
            cfg.min_blur_dip = 4.5f;
            cfg.vibrancy = 1.80f;
            cfg.tint = settings.dark_mode
                ? D2D1::ColorF(0.10f, 0.11f, 0.15f, 0.28f)
                : D2D1::ColorF(0.96f, 0.97f, 1.00f, 0.28f);
            cfg.white_veil_alpha = 0.05f;
            cfg.rim_intensity = 0.90f;
            cfg.shadow_blur_dip = 20.0f;
            cfg.shadow_offset_y_dip = 14.0f;
            cfg.shadow_opacity = 0.50f;
            break;
        case 0: // 清透水滴 (Clear Droplet -> Exact reference Tahoe Clear)
            break;
        default: { // Custom legacy sliders
            const float th = static_cast<float>(std::clamp<uint8_t>(settings.glass_thickness, 1u, 80u));
            const float ior = static_cast<float>(std::clamp<uint16_t>(settings.glass_ior_x100, 100u, 400u)) * 0.01f;
            const float disp = static_cast<float>(std::min<uint8_t>(settings.glass_dispersion, 50u));
            const float glare = static_cast<float>(std::min<uint8_t>(settings.glass_glare, 120u));
            const float frost = static_cast<float>(std::min<uint8_t>(settings.glass_frost, 100u)) * 0.01f;
            cfg.bezel_max_dip = std::clamp(th * 1.15f, 6.0f, 48.0f);
            cfg.refraction_strength = std::clamp(base_refract * (th / 24.0f), 0.10f, 1.0f);
            cfg.index_of_refraction = std::clamp(ior, 1.05f, 2.60f);
            cfg.chromatic_spread = (disp <= 0.1f) ? 0.0f : std::clamp(0.13f * (disp / 9.0f), 0.0f, 0.35f);
            cfg.enable_chromatic_aberration = (cfg.chromatic_spread > 0.001f);
            cfg.frost_amount = std::clamp(0.06f + frost * 0.85f, 0.0f, 1.0f);
            cfg.rim_intensity = std::clamp(base_rim * (glare / 90.0f), 0.0f, 1.30f);
            break;
        }
        }
    }

    // Scale body tint subtly with user's card opacity (60% is the reference 1.0x sweet spot).
    const float op_norm = std::clamp(static_cast<float>(settings.card_opacity_percent) / 60.0f, 0.45f, 1.50f);
    cfg.tint.a = std::clamp(cfg.tint.a * op_norm, 0.0f, 0.82f);

    if (elevated_hover) {
        cfg.rim_intensity = std::min(1.25f, cfg.rim_intensity + 0.08f);
        cfg.shadow_opacity = std::min(0.70f, cfg.shadow_opacity + 0.07f);
    }
    if (energy_saver) {
        cfg.enable_chromatic_aberration = false;
    }
    return cfg;
}

void TileRoundedRectSdfAndNormal(float px, float py, float hx, float hy, float r,
                                 float& out_sdf, float& out_nx, float& out_ny) noexcept {
    const float ax = std::fabs(px);
    const float ay = std::fabs(py);
    const float qx = ax - (hx - r);
    const float qy = ay - (hy - r);
    float sdf = 0.0f, nx = 0.0f, ny = 0.0f;
    if (qx > 0.0f && qy > 0.0f) {
        const float len = std::sqrt(qx * qx + qy * qy);
        sdf = len - r;
        nx = len > 1e-6f ? (qx / len) : 0.70710678f;
        ny = len > 1e-6f ? (qy / len) : 0.70710678f;
    } else if (qx > qy) {
        sdf = qx - r;
        nx = 1.0f;
        ny = 0.0f;
    } else {
        sdf = qy - r;
        nx = 0.0f;
        ny = 1.0f;
    }
    if (px < 0.0f) nx = -nx;
    if (py < 0.0f) ny = -ny;
    out_sdf = sdf;
    out_nx = nx;
    out_ny = ny;
}

float EvaluateTileSnellProfile(float t, float ior, float slope) noexcept {
    struct CachedLut {
        float ior = -1.0f;
        float slope = -1.0f;
        float values[257]{};
    };
    thread_local CachedLut lut;

    const float safe_ior = std::max(1.01f, ior);
    const float safe_slope = std::max(0.1f, slope);

    if (std::fabs(lut.ior - safe_ior) > 1e-4f || std::fabs(lut.slope - safe_slope) > 1e-4f) {
        float max_val = 0.0f;
        for (int i = 0; i <= 256; ++i) {
            const float x = std::max(static_cast<float>(i) / 256.0f, 1e-4f);
            const float u = 1.0f - x;
            const float u3 = u * u * u;
            const float u4 = u3 * u;
            // Derivative of convex superellipse height h(x) = (1 - (1 - x)^4)^(1/4)
            const float dh = u3 / std::pow(std::max(1e-6f, 1.0f - u4), 0.75f) * safe_slope;
            const float theta_in = std::atan(dh);
            const float sin_out = std::clamp(std::sin(theta_in) / safe_ior, -0.9999f, 0.9999f);
            const float theta_out = std::asin(sin_out);
            lut.values[i] = std::tan(theta_in - theta_out);
            max_val = std::max(max_val, lut.values[i]);
        }
        if (max_val > 1e-6f) {
            for (float& v : lut.values) v /= max_val;
        }
        lut.ior = safe_ior;
        lut.slope = safe_slope;
    }

    const float f = std::clamp(t, 0.0f, 1.0f) * 256.0f;
    const int idx = std::min(255, static_cast<int>(f));
    const float frac = f - static_cast<float>(idx);
    return lut.values[idx] + (lut.values[idx + 1] - lut.values[idx]) * frac;
}

int ComputeTileGlassPaddingMargin(const TileGlassConfig& cfg, int w_px, int h_px, float dpi_scale) noexcept {
    const float H = static_cast<float>(std::min(w_px, h_px));
    const float k = std::clamp(cfg.refraction_strength, 0.0f, 1.0f);
    const float dmax = std::min(H * 0.8f, cfg.max_displacement_dip * dpi_scale) * (0.4f + 1.0f * k);
    const float sigma = std::max(0.6f + 11.0f * std::clamp(cfg.frost_amount, 0.0f, 1.0f), cfg.min_blur_dip) * dpi_scale;
    const int max_cap = static_cast<int>(std::ceil(64.0f * std::max(1.0f, dpi_scale)));
    return std::clamp(static_cast<int>(std::ceil(3.0f * sigma + dmax + 4.0f)), 8, max_cap);
}

void DrawTileSpecularRim(ID2D1RenderTarget* rt,
                         const D2D1_RECT_F& r,
                         float rad,
                         float px,
                         bool dark,
                         float strength,
                         bool clear_style,
                         float spec,
                         float lx,
                         float ly) {
    if (!rt || strength <= 0.001f || (r.right - r.left) < 4.0f || (r.bottom - r.top) < 4.0f) return;

    if (!clear_style) {
        ComPtr<ID2D1SolidColorBrush> dark_line;
        const float dark_alpha = (dark ? 0.30f : 0.10f) * strength;
        if (SUCCEEDED(rt->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, dark_alpha), &dark_line)) && dark_line) {
            const float inset = 0.5f * px;
            const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
                D2D1::RectF(r.left + inset, r.top + inset, r.right - inset, r.bottom - inset),
                std::max(0.0f, rad - inset), std::max(0.0f, rad - inset));
            rt->DrawRoundedRectangle(rr, dark_line.Get(), px);
        }
    }

    const float w = r.right - r.left;
    const float h = r.bottom - r.top;
    const float span = std::min(w, h) * 0.50f + std::max(w, h) * 0.18f;
    const float gain = (dark ? 0.75f : 0.95f) * strength;
    const float diag = span * 0.70710678f;

    auto make_corner_gradient = [&](float x0, float y0, float x1, float y1, float alpha) -> ComPtr<ID2D1LinearGradientBrush> {
        const D2D1_GRADIENT_STOP stops[3] = {
            {0.00f, D2D1::ColorF(1.0f, 1.0f, 1.0f, alpha)},
            {0.45f, D2D1::ColorF(1.0f, 1.0f, 1.0f, alpha * 0.28f)},
            {1.00f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f)},
        };
        ComPtr<ID2D1GradientStopCollection> sc;
        if (FAILED(rt->CreateGradientStopCollection(stops, 3, &sc)) || !sc) return nullptr;
        ComPtr<ID2D1LinearGradientBrush> brush;
        rt->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1)),
            sc.Get(), &brush);
        return brush;
    };

    float kx = r.left;
    float ky = r.top;
    float ux = 0.70710678f;
    float uy = 0.70710678f;
    if (lx >= 0.0f && ly >= 0.0f) {
        const float cx = (r.left + r.right) * 0.5f;
        const float cy = (r.top + r.bottom) * 0.5f;
        const float vx = (lx - 0.5f) * w;
        const float vy = (ly - 0.5f) * h;
        const float len = std::sqrt(vx * vx + vy * vy);
        if (len > 0.5f) {
            ux = -vx / len;
            uy = -vy / len;
        }
        kx = cx - ux * (w * 0.5f);
        ky = cy - uy * (h * 0.5f);
    }

    ComPtr<ID2D1LinearGradientBrush> key_light;
    ComPtr<ID2D1LinearGradientBrush> fill_light;
    if (spec > 0.001f) {
        key_light = make_corner_gradient(kx, ky, kx + ux * diag, ky + uy * diag, 0.42f * gain * spec);
        const float bx = (r.left + r.right) - kx;
        const float by = (r.top + r.bottom) - ky;
        fill_light = make_corner_gradient(bx, by, bx - ux * diag, by - uy * diag, 0.20f * gain * spec);
    }

    for (int ring = 0; ring < 2; ++ring) {
        const float inset = (1.0f + static_cast<float>(ring)) * px;
        const float ring_opacity = (ring == 0) ? 0.85f : 0.10f;
        const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
            D2D1::RectF(r.left + inset, r.top + inset, r.right - inset, r.bottom - inset),
            std::max(0.0f, rad - inset), std::max(0.0f, rad - inset));
        if (key_light) {
            key_light->SetOpacity(ring_opacity);
            rt->DrawRoundedRectangle(rr, key_light.Get(), px);
        }
        if (fill_light) {
            fill_light->SetOpacity(ring_opacity);
            rt->DrawRoundedRectangle(rr, fill_light.Get(), px);
        }
    }

    ComPtr<ID2D1SolidColorBrush> ambient;
    const float amb_alpha = (clear_style ? (dark ? 0.24f : 0.42f) : (dark ? 0.16f : 0.22f)) * strength;
    if (SUCCEEDED(rt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, amb_alpha), &ambient)) && ambient) {
        const float inset = 1.0f * px;
        const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
            D2D1::RectF(r.left + inset, r.top + inset, r.right - inset, r.bottom - inset),
            std::max(0.0f, rad - inset), std::max(0.0f, rad - inset));
        rt->DrawRoundedRectangle(rr, ambient.Get(), px);
    }
}

void TileLiquidGlassRenderer::BeginFrame() noexcept {
    next_slot_ = 0;
}

void TileLiquidGlassRenderer::EndFrame() noexcept {
    next_slot_ = 0;
}

void TileLiquidGlassRenderer::Reset() noexcept {
    slots_.clear();
    next_slot_ = 0;
    shadow_cache_.clear();
    bound_dc_ = nullptr;
    unavailable_ = false;
}

void TileLiquidGlassRenderer::TrimCaches() noexcept {
    slots_.clear();
    next_slot_ = 0;
    shadow_cache_.clear();
}

void TileLiquidGlassRenderer::BindContext(ID2D1DeviceContext* dc) noexcept {
    if (bound_dc_ == dc) return;
    Reset();
    bound_dc_ = dc;
}

TileLiquidGlassRenderer::EffectSlot* TileLiquidGlassRenderer::AcquireSlot(
    ID2D1Factory1* factory, ID2D1DeviceContext* dc) noexcept {
    if (!factory || !dc) return nullptr;
    BindContext(dc);
    if (unavailable_) return nullptr;
    if (!EnsureTileLiquidGlassEffectRegistered(factory)) {
        unavailable_ = true;
        return nullptr;
    }

    if (next_slot_ < slots_.size()) {
        return &slots_[next_slot_++];
    }

    EffectSlot s{};
    const HRESULT hr = dc->CreateEffect(CLSID_PulseTileLiquidGlass, &s.tile_glass);
    if (FAILED(hr) || !s.tile_glass) {
        unavailable_ = true;
        return nullptr;
    }

    try {
        slots_.push_back(std::move(s));
        return &slots_[next_slot_++];
    } catch (...) {
        return nullptr;
    }
}

ID2D1Bitmap1* TileLiquidGlassRenderer::EnsureAmbientShadow(ID2D1DeviceContext* dc, int w_dip, int h_dip,
                                                          float radius_dip, float blur_dip, float offset_y_dip) {
    if (!dc || w_dip <= 0 || h_dip <= 0) return nullptr;
    const float clamped_r = std::min({std::max(0.0f, radius_dip),
                                      static_cast<float>(w_dip) * 0.5f,
                                      static_cast<float>(h_dip) * 0.5f});
    const int radius_q = static_cast<int>(std::lround(clamped_r * 2.0f));
    const int blur_q = static_cast<int>(std::lround(std::clamp(blur_dip, 4.0f, 30.0f) * 4.0f));
    const int offset_q = static_cast<int>(std::lround(std::clamp(offset_y_dip, 0.0f, 24.0f) * 4.0f));
    ++shadow_clock_;
    for (auto& e : shadow_cache_) {
        if (e.w == w_dip && e.h == h_dip && e.radius_q == radius_q &&
            e.blur_q == blur_q && e.offset_q == offset_q) {
            e.last_use = shadow_clock_;
            return e.bitmap.Get();
        }
    }

    // Separable erf Gaussian ambient + tight contact shadow with the rounded-rect interior baked
    // directly to alpha = 0 (eliminates per-frame D2D PushLayer + 3 COM geometries per card!).
    const float kSigma = static_cast<float>(blur_q) * 0.25f;
    const float kOffsetY = static_cast<float>(offset_q) * 0.25f;
    const float kRad = static_cast<float>(radius_q) * 0.5f;
    const float w = static_cast<float>(w_dip), h = static_cast<float>(h_dip);
    const float hw = w * 0.5f, hh = h * 0.5f;
    const float ext_w = w + 2.0f * kShadowPadSideDip;
    const float ext_h = h + static_cast<float>(kShadowPadTopDip + kShadowPadBottomDip);
    const int tw = static_cast<int>(std::ceil(ext_w / kShadowTexelDip));
    const int th = static_cast<int>(std::ceil(ext_h / kShadowTexelDip));
    if (tw <= 0 || th <= 0 || tw > 8192 || th > 8192) return nullptr;
    const float step_x = ext_w / static_cast<float>(tw), step_y = ext_h / static_cast<float>(th);
    const float aa_span = std::max(step_x, step_y);

    const float inv_sig1 = 1.0f / (kSigma * 1.41421356f);
    const float inv_sig2 = 1.0f / ((kSigma * 0.30f) * 1.41421356f);
    std::vector<float> ax1(static_cast<size_t>(tw)), ax2(static_cast<size_t>(tw)), fade_x(static_cast<size_t>(tw));
    std::vector<float> ay1(static_cast<size_t>(th)), ay2(static_cast<size_t>(th)), fade_y(static_cast<size_t>(th));
    std::vector<float> local_x(static_cast<size_t>(tw)), local_y(static_cast<size_t>(th));
    for (int x = 0; x < tw; ++x) {
        const float ex = (static_cast<float>(x) + 0.5f) * step_x;
        const float fx = ex - kShadowPadSideDip;
        local_x[static_cast<size_t>(x)] = fx - hw;
        ax1[static_cast<size_t>(x)] = 0.5f * (std::erf(fx * inv_sig1) - std::erf((fx - w) * inv_sig1));
        ax2[static_cast<size_t>(x)] = 0.5f * (std::erf(fx * inv_sig2) - std::erf((fx - w) * inv_sig2));
        fade_x[static_cast<size_t>(x)] = std::clamp(std::min(ex, ext_w - ex) / 4.0f, 0.0f, 1.0f);
    }
    for (int y = 0; y < th; ++y) {
        const float ey = (static_cast<float>(y) + 0.5f) * step_y;
        const float fy = ey - kShadowPadTopDip;
        local_y[static_cast<size_t>(y)] = fy - hh;
        ay1[static_cast<size_t>(y)] = 0.5f * (std::erf((fy - kOffsetY) * inv_sig1) - std::erf((fy - h - kOffsetY) * inv_sig1));
        ay2[static_cast<size_t>(y)] = 0.5f * (std::erf((fy - kOffsetY * 0.25f) * inv_sig2) -
                                              std::erf((fy - h - kOffsetY * 0.25f) * inv_sig2));
        fade_y[static_cast<size_t>(y)] = std::clamp(std::min(ey, ext_h - ey) / 4.0f, 0.0f, 1.0f);
    }
    std::vector<uint32_t> px(static_cast<size_t>(tw) * static_cast<size_t>(th), 0u);
    for (int y = 0; y < th; ++y) {
        const size_t yi = static_cast<size_t>(y);
        const float ly = local_y[yi];
        for (int x = 0; x < tw; ++x) {
            const size_t xi = static_cast<size_t>(x);
            const float sdf = RoundedRectSdf(local_x[xi], ly, hw, hh, kRad);
            if (sdf <= -0.5f * aa_span) continue;
            const float outside = std::clamp((sdf + 0.5f * aa_span) / aa_span, 0.0f, 1.0f);
            const float a = std::clamp((ax1[xi] * ay1[yi] + 0.45f * ax2[xi] * ay2[yi]) *
                                       fade_x[xi] * fade_y[yi] * outside, 0.0f, 1.0f);
            px[yi * static_cast<size_t>(tw) + xi] = static_cast<uint32_t>(std::lround(a * 255.0f)) << 24;
        }
    }
    const D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap;
    if (FAILED(dc->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(tw), static_cast<UINT32>(th)), px.data(),
                                static_cast<UINT32>(tw) * 4u, &bp, &bitmap)) || !bitmap) {
        return nullptr;
    }
    if (shadow_cache_.size() >= kShadowCacheMax) {
        auto oldest = std::min_element(shadow_cache_.begin(), shadow_cache_.end(),
                                       [](const ShadowEntry& l, const ShadowEntry& r) { return l.last_use < r.last_use; });
        shadow_cache_.erase(oldest);
    }
    shadow_cache_.push_back(ShadowEntry{w_dip, h_dip, radius_q, blur_q, offset_q, shadow_clock_, bitmap});
    return bitmap.Get();
}

void TileLiquidGlassRenderer::DrawHollowedAmbientShadow(ID2D1DeviceContext* dc,
                                                        const D2D1_RECT_F& rect_dip,
                                                        float radius_dip,
                                                        float opacity,
                                                        float blur_dip,
                                                        float offset_y_dip) {
    if (!dc || opacity <= 0.004f) return;
    const float w = rect_dip.right - rect_dip.left;
    const float h = rect_dip.bottom - rect_dip.top;
    if (w <= 4.0f || h <= 4.0f) return;
    ID2D1Bitmap1* shadow = EnsureAmbientShadow(dc, static_cast<int>(std::lround(w)), static_cast<int>(std::lround(h)),
                                                radius_dip, blur_dip, offset_y_dip);
    if (!shadow) return;

    const D2D1_RECT_F outer = D2D1::RectF(rect_dip.left - kShadowPadSideDip, rect_dip.top - kShadowPadTopDip,
                                          rect_dip.right + kShadowPadSideDip, rect_dip.bottom + kShadowPadBottomDip);
    dc->DrawBitmap(shadow, outer, std::clamp(opacity, 0.0f, 0.85f), D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

bool TileLiquidGlassRenderer::DrawTile(ID2D1Factory1* factory,
                                       ID2D1DeviceContext* dc,
                                       ID2D1Bitmap1* source_bmp,
                                       uint32_t src_w, uint32_t src_h,
                                       uint32_t logical_w, uint32_t logical_h,
                                       float origin_px_x, float origin_px_y,
                                       float dip_scale,
                                       const D2D1_RECT_F& rect_dip,
                                       const TileGlassConfig& cfg,
                                       float overall_opacity,
                                       bool draw_rim,
                                       float light_norm_x,
                                       float light_norm_y,
                                       ID2D1Bitmap1* frost_bmp,
                                       uint32_t frost_w,
                                       uint32_t frost_h) {
    if (!factory || !dc || !source_bmp || src_w == 0 || src_h == 0 || logical_w == 0 || logical_h == 0) {
        return false;
    }
    const float w_dip = rect_dip.right - rect_dip.left;
    const float h_dip = rect_dip.bottom - rect_dip.top;
    if (w_dip < 4.0f || h_dip < 4.0f) return true;
    const float op = std::clamp(overall_opacity, 0.0f, 1.0f);
    if (op <= 1e-4f) return true;

    EffectSlot* slot = AcquireSlot(factory, dc);
    if (!slot) return false;

    const float safe_scale = std::max(0.5f, dip_scale);
    const float rad_dip = std::min({cfg.corner_radius_dip, w_dip * 0.5f, h_dip * 0.5f});

    // 1. Hollowed ambient + contact drop shadow strictly outside rounded rect interior (in scene DIPs).
    if (cfg.enable_drop_shadow && cfg.shadow_opacity > 0.004f) {
        DrawHollowedAmbientShadow(dc, rect_dip, rad_dip, cfg.shadow_opacity * op, cfg.shadow_blur_dip, cfg.shadow_offset_y_dip);
    }

    // 2. Map card bounds through dc's active transform (VirtualSurface atlas shift, jiggle rotation,
    //    press scale, offscreen capture). Decompose into center-preserving scale, rotation around the
    //    transformed card center, and pure translation atlas shift so rotating/scaling a tile never
    //    pollutes the shader's rect or wallpaper sampling origin with ±cx*sin(theta) drift.
    D2D1_MATRIX_3X2_F prev_xf{};
    dc->GetTransform(&prev_xf);
    const D2D1_UNIT_MODE prev_unit = dc->GetUnitMode();
    float prev_dpi_x = 96.0f, prev_dpi_y = 96.0f;
    dc->GetDpi(&prev_dpi_x, &prev_dpi_y);

    const float raw_sx = std::hypot(prev_xf.m11, prev_xf.m12);
    const float raw_sy = std::hypot(prev_xf.m21, prev_xf.m22);
    const float sx = (raw_sx > 1e-4f) ? raw_sx : 1.0f;
    const float sy = (raw_sy > 1e-4f) ? raw_sy : 1.0f;

    const float cx_dip = (rect_dip.left + rect_dip.right) * 0.5f;
    const float cy_dip = (rect_dip.top + rect_dip.bottom) * 0.5f;
    const float dst_cx_dip = cx_dip * prev_xf.m11 + cy_dip * prev_xf.m21 + prev_xf.dx;
    const float dst_cy_dip = cx_dip * prev_xf.m12 + cy_dip * prev_xf.m22 + prev_xf.dy;

    const float atlas_shift_x = (dst_cx_dip - cx_dip) * safe_scale;
    const float atlas_shift_y = (dst_cy_dip - cy_dip) * safe_scale;

    const float cx_px = dst_cx_dip * safe_scale;
    const float cy_px = dst_cy_dip * safe_scale;
    const float half_w_px = std::max(1.0f, (w_dip * sx * safe_scale) * 0.5f);
    const float half_h_px = std::max(1.0f, (h_dip * sy * safe_scale) * 0.5f);
    const float left_px = cx_px - half_w_px;
    const float top_px = cy_px - half_h_px;
    const float right_px = cx_px + half_w_px;
    const float bottom_px = cy_px + half_h_px;
    const float w_f = std::max(2.0f, right_px - left_px);
    const float h_f = std::max(2.0f, bottom_px - top_px);
    const float rad_px = rad_dip * std::min(sx, sy) * safe_scale;

    // 3. Optical parameters aligned 1:1 to reference Tahoe Liquid Glass.
    const float H = std::min(w_f, h_f);
    const float k = std::clamp(cfg.refraction_strength, 0.0f, 1.0f);
    const float bezel = std::clamp(H * cfg.bezel_ratio,
                                   cfg.bezel_min_dip * safe_scale,
                                   cfg.bezel_max_dip * safe_scale);
    const float dmax = std::min(H * 0.8f, cfg.max_displacement_dip * safe_scale) * (0.4f + 1.0f * k);
    const float sigma = std::max(0.6f + 11.0f * std::clamp(cfg.frost_amount, 0.0f, 1.0f),
                                 cfg.min_blur_dip) * safe_scale;
    const float spread = (cfg.enable_chromatic_aberration && cfg.chromatic_spread > 0.001f)
        ? std::clamp(cfg.chromatic_spread, 0.0f, 0.5f) : 0.0f;

    const bool has_frost = (frost_bmp != nullptr && frost_w > 0 && frost_h > 0);
    ID2D1Bitmap1* eff_frost = has_frost ? frost_bmp : source_bmp;
    const uint32_t eff_fw = has_frost ? frost_w : src_w;
    const uint32_t eff_fh = has_frost ? frost_h : src_h;

    const auto m_sharp = ComputeGlassSourceMatrix(
        src_w, src_h, logical_w, logical_h, origin_px_x, origin_px_y, atlas_shift_x, atlas_shift_y);

    // Bind native sharp & frost bitmaps directly to the custom pixel shader (ZERO D2D12DAffineTransform
    // intermediate 4K upscale surfaces!).
    slot->tile_glass->SetInput(0, source_bmp);
    slot->tile_glass->SetInput(1, eff_frost);

    TileGlassEffectPayload payload{};
    auto& c = payload.constants;
    c.rect_x = left_px;
    c.rect_y = top_px;
    c.rect_w = w_f;
    c.rect_h = h_f;
    c.radius_px = rad_px;
    c.bezel_px = bezel;
    c.ior = std::max(1.01f, cfg.index_of_refraction);
    c.bezel_slope = std::max(0.1f, cfg.bezel_slope);
    c.dmax_px = dmax;
    c.inv_snell_max = 1.0f / ComputeSnellPeakValue(c.ior, c.bezel_slope);
    c.chromatic_spread = spread;
    c.dpi_scale = safe_scale;
    c.sigma_px = sigma;
    c.frost_amount = std::clamp(cfg.frost_amount, 0.0f, 1.0f);
    c.vibrancy = std::clamp(cfg.vibrancy, 0.0f, 3.0f);
    c.overall_opacity = op;
    c.tint_r = cfg.tint.r;
    c.tint_g = cfg.tint.g;
    c.tint_b = cfg.tint.b;
    c.tint_a = std::clamp(cfg.tint.a, 0.0f, 1.0f);
    c.white_veil_alpha = std::clamp(cfg.white_veil_alpha, 0.0f, 0.5f);
    c.dither_alpha = std::clamp(cfg.dither_noise_alpha, 0.0f, 0.25f);
    c.rim_gain = (cfg.enable_surface_rim && draw_rim && cfg.rim_intensity > 0.001f)
        ? ((cfg.dark_appearance ? 0.88f : 0.95f) * cfg.rim_intensity) : 0.0f;
    c.has_frost_tex = has_frost ? 1.0f : 0.0f;

    c.bounds_min_x = m_sharp.dx;
    c.bounds_min_y = m_sharp.dy;
    c.bounds_max_x = m_sharp.dx + static_cast<float>(logical_w);
    c.bounds_max_y = m_sharp.dy + static_cast<float>(logical_h);

    c.sharp_scale_x = static_cast<float>(src_w) / static_cast<float>(logical_w);
    c.sharp_scale_y = static_cast<float>(src_h) / static_cast<float>(logical_h);
    c.frost_scale_x = static_cast<float>(eff_fw) / static_cast<float>(logical_w);
    c.frost_scale_y = static_cast<float>(eff_fh) / static_cast<float>(logical_h);
    c.sharp_max_x = std::max(0.5f, static_cast<float>(src_w) - 0.5f);
    c.sharp_max_y = std::max(0.5f, static_cast<float>(src_h) - 0.5f);
    c.frost_max_x = std::max(0.5f, static_cast<float>(eff_fw) - 0.5f);
    c.frost_max_y = std::max(0.5f, static_cast<float>(eff_fh) - 0.5f);

    const bool clear_style = (cfg.frost_amount < 0.30f);
    c.light_norm_x = light_norm_x;
    c.light_norm_y = light_norm_y;
    c.rim_stroke_strength = (draw_rim && cfg.rim_intensity > 0.001f) ? (cfg.rim_intensity * op) : 0.0f;
    c.rim_flags = static_cast<float>((cfg.dark_appearance ? 1 : 0) | (clear_style ? 2 : 0));

    payload.output_rect_px = {
        static_cast<int32_t>(std::floor(left_px)) - 1,
        static_cast<int32_t>(std::floor(top_px)) - 1,
        static_cast<int32_t>(std::ceil(right_px)) + 1,
        static_cast<int32_t>(std::ceil(bottom_px)) + 1,
    };
    payload.input_bounds_px = {
        static_cast<int32_t>(std::floor(c.bounds_min_x)),
        static_cast<int32_t>(std::floor(c.bounds_min_y)),
        static_cast<int32_t>(std::ceil(c.bounds_max_x)),
        static_cast<int32_t>(std::ceil(c.bounds_max_y)),
    };
    payload.sharp_w = static_cast<int32_t>(src_w);
    payload.sharp_h = static_cast<int32_t>(src_h);
    payload.frost_w = static_cast<int32_t>(eff_fw);
    payload.frost_h = static_cast<int32_t>(eff_fh);
    payload.sample_pad_px = std::clamp(
        static_cast<int32_t>(std::ceil(dmax * (1.0f + spread) + std::min(sigma, 4.0f) * 2.0f + 4.0f)),
        4, kGlassMaxInputHalo);

    const auto requested = BoundedGlassInputRect(payload.output_rect_px, payload.input_bounds_px,
                                                 payload.sample_pad_px + 2);
    if (!GlassInputWithinBudget(requested)) {
        return false;
    }

    if (FAILED(slot->tile_glass->SetValue(0u, reinterpret_cast<const BYTE*>(&payload), sizeof(payload)))) {
        return false;
    }

    // 4. Single-pass GPU pixel shader draw in target physical pixel space (includes refraction,
    //    dispersion, frost, vibrancy, tint, dither, and interactive cursor specular rim).
    const bool has_rot = (std::fabs(prev_xf.m12) > 1e-5f || std::fabs(prev_xf.m21) > 1e-5f);
    D2D1_MATRIX_3X2_F draw_xf = D2D1::Matrix3x2F::Identity();
    if (has_rot) {
        const float cos_a = prev_xf.m11 / sx;
        const float sin_a = prev_xf.m12 / sx;
        draw_xf._11 = cos_a;
        draw_xf._12 = sin_a;
        draw_xf._21 = -sin_a;
        draw_xf._22 = cos_a;
        draw_xf._31 = cx_px - (cx_px * cos_a - cy_px * sin_a);
        draw_xf._32 = cy_px - (cx_px * sin_a + cy_px * cos_a);
    }
    dc->SetDpi(96.0f, 96.0f);
    dc->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    dc->SetTransform(draw_xf);
    dc->DrawImage(slot->tile_glass.Get(), D2D1_INTERPOLATION_MODE_LINEAR, D2D1_COMPOSITE_MODE_SOURCE_OVER);
    dc->SetTransform(prev_xf);
    dc->SetUnitMode(prev_unit);
    dc->SetDpi(prev_dpi_x, prev_dpi_y);

    return true;
}

} // namespace pulse::desktop::render

// liquid_glass_effect.cpp - Direct2D 1.1 custom GPU pixel-shader effect (ps_4_0)
// and pooled pass runner for physical Liquid Glass surfaces.
#include "host/render/liquid_glass_effect.h"
#include "../../../../src/ui/window_material.h"

#include <d2d1effectauthor.h>
#include <d3dcommon.h>

// Generated at build time by fxc.exe from host/render/shaders/liquid_glass.hlsl:
#include "liquid_glass_ps.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <new>

namespace pulse::desktop::render {
namespace {

using Microsoft::WRL::ComPtr;

// {3C91E5A4-28F7-44D2-A715-6B09F351C28D}
constexpr GUID CLSID_PulseLiquidGlassShader = {
    0x3c91e5a4, 0x28f7, 0x44d2, {0xa7, 0x15, 0x6b, 0x09, 0xf3, 0x51, 0xc2, 0x8d}};

// Acyclic GPU draw transform node owned by LiquidGlassEffectImpl + ID2D1TransformGraph.
class LiquidGlassDrawTransform final : public ID2D1DrawTransform {
public:
    LiquidGlassDrawTransform() = default;

    // IUnknown
    IFACEMETHODIMP_(ULONG) AddRef() override {
        return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG count = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (count == 0) {
            delete this;
        }
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

    // ID2D1TransformNode
    IFACEMETHODIMP_(UINT32) GetInputCount() const override { return 2; }

    // ID2D1DrawTransform
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

        draw_info_->SetInstructionCountHint(96);
        hr = draw_info_->SetPixelShader(CLSID_PulseLiquidGlassShader, D2D1_PIXEL_OPTIONS_NONE);
        if (FAILED(hr)) return hr;

        return PushConstants();
    }

    // ID2D1Transform
    IFACEMETHODIMP MapOutputRectToInputRects(const D2D1_RECT_L* output_rect,
                                             D2D1_RECT_L* input_rects,
                                             UINT32 input_rects_count) const override {
        if (!output_rect || !input_rects || input_rects_count != 2) return E_INVALIDARG;
        const LONG pad = std::clamp<LONG>(
            static_cast<LONG>(std::ceil(std::fabs(payload_.constants.refraction_pad))) + 2L, 4L, 384L);
        const auto bounded=BoundedGlassInputRect({output_rect->left,output_rect->top,output_rect->right,output_rect->bottom},payload_.input_bounds_px,pad);
        input_rects[0] = D2D1_RECT_L{bounded.left,bounded.top,bounded.right,bounded.bottom};
        input_rects[1] = input_rects[0];
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
        // Glass edges are anti-aliased and interior may pass through -> zero guaranteed opaque sub-rect.
        *output_opaque_sub_rect = D2D1_RECT_L{0, 0, 0, 0};
        return S_OK;
    }

    IFACEMETHODIMP MapInvalidRect(UINT32 input_index,
                                  D2D1_RECT_L invalid_input_rect,
                                  D2D1_RECT_L* invalid_output_rect) const override {
        if (!invalid_output_rect || input_index >= 2) return E_INVALIDARG;
        const LONG pad = std::clamp<LONG>(
            static_cast<LONG>(std::ceil(std::fabs(payload_.constants.refraction_pad))) + 2L, 4L, 384L);
        *invalid_output_rect = D2D1_RECT_L{
            invalid_input_rect.left - pad,
            invalid_input_rect.top - pad,
            invalid_input_rect.right + pad,
            invalid_input_rect.bottom + pad,
        };
        return S_OK;
    }

    void SetPayload(const LiquidGlassEffectPayload& payload) noexcept {
        payload_ = payload;
        dirty_ = true;
    }

    const LiquidGlassEffectPayload& Payload() const noexcept { return payload_; }

    HRESULT PushConstants() noexcept {
        if (!draw_info_) return S_OK;
        dirty_ = false;
        return draw_info_->SetPixelShaderConstantBuffer(
            reinterpret_cast<const BYTE*>(&payload_.constants),
            static_cast<UINT32>(sizeof(payload_.constants)));
    }

    bool Dirty() const noexcept { return dirty_; }

private:
    ~LiquidGlassDrawTransform() = default;

    std::atomic<ULONG> ref_count_{1};
    ComPtr<ID2D1DrawInfo> draw_info_;
    LiquidGlassEffectPayload payload_{};
    bool dirty_ = true;
};

class LiquidGlassEffectImpl final : public ID2D1EffectImpl {
public:
    static HRESULT CALLBACK CreateEffect(IUnknown** effect_impl) noexcept {
        if (!effect_impl) return E_POINTER;
        auto* inst = new (std::nothrow) LiquidGlassEffectImpl();
        if (!inst) return E_OUTOFMEMORY;
        *effect_impl = static_cast<ID2D1EffectImpl*>(inst);
        return S_OK;
    }

    // IUnknown
    IFACEMETHODIMP_(ULONG) AddRef() override {
        return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG count = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (count == 0) {
            delete this;
        }
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

    // ID2D1EffectImpl
    IFACEMETHODIMP Initialize(ID2D1EffectContext* effect_context,
                              ID2D1TransformGraph* transform_graph) override {
        if (!effect_context || !transform_graph) return E_INVALIDARG;

        const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
        };
        D3D_FEATURE_LEVEL max_fl = D3D_FEATURE_LEVEL_9_1;
        HRESULT hr = effect_context->GetMaximumSupportedFeatureLevel(levels, ARRAYSIZE(levels), &max_fl);
        if (FAILED(hr) || max_fl < D3D_FEATURE_LEVEL_10_0) {
            return D2DERR_INSUFFICIENT_DEVICE_CAPABILITIES;
        }

        if (!effect_context->IsShaderLoaded(CLSID_PulseLiquidGlassShader)) {
            hr = effect_context->LoadPixelShader(
                CLSID_PulseLiquidGlassShader,
                g_liquid_glass_ps,
                static_cast<UINT32>(sizeof(g_liquid_glass_ps)));
            if (FAILED(hr)) return hr;
        }

        auto* draw_raw = new (std::nothrow) LiquidGlassDrawTransform();
        if (!draw_raw) return E_OUTOFMEMORY;
        draw_transform_.Attach(draw_raw);

        // Clamp both inputs to infinite bounds so cards near monitor edges refract clean edge pixels.
        ComPtr<ID2D1BorderTransform> border_sharp;
        ComPtr<ID2D1BorderTransform> border_frost;
        hr = effect_context->CreateBorderTransform(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, &border_sharp);
        if (FAILED(hr)) return hr;
        hr = effect_context->CreateBorderTransform(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, &border_frost);
        if (FAILED(hr)) return hr;

        hr = transform_graph->AddNode(border_sharp.Get());
        if (FAILED(hr)) return hr;
        hr = transform_graph->AddNode(border_frost.Get());
        if (FAILED(hr)) return hr;
        hr = transform_graph->AddNode(draw_transform_.Get());
        if (FAILED(hr)) return hr;

        hr = transform_graph->ConnectToEffectInput(0, border_sharp.Get(), 0);
        if (FAILED(hr)) return hr;
        hr = transform_graph->ConnectToEffectInput(1, border_frost.Get(), 0);
        if (FAILED(hr)) return hr;
        hr = transform_graph->ConnectNode(border_sharp.Get(), draw_transform_.Get(), 0);
        if (FAILED(hr)) return hr;
        hr = transform_graph->ConnectNode(border_frost.Get(), draw_transform_.Get(), 1);
        if (FAILED(hr)) return hr;
        return transform_graph->SetOutputNode(draw_transform_.Get());
    }

    IFACEMETHODIMP PrepareForRender(D2D1_CHANGE_TYPE change_type) override {
        if (!draw_transform_) return E_UNEXPECTED;
        if (change_type == D2D1_CHANGE_TYPE_NONE && !draw_transform_->Dirty()) {
            return S_OK;
        }
        return draw_transform_->PushConstants();
    }

    IFACEMETHODIMP SetGraph(ID2D1TransformGraph* /*transform_graph*/) override {
        return E_NOTIMPL;
    }

    static HRESULT CALLBACK SetParamsThunk(IUnknown* effect, const BYTE* data, UINT32 data_size) noexcept {
        if (!effect) return E_INVALIDARG;
        return static_cast<LiquidGlassEffectImpl*>(static_cast<ID2D1EffectImpl*>(effect))->SetParams(data, data_size);
    }

    static HRESULT CALLBACK GetParamsThunk(const IUnknown* effect, BYTE* data, UINT32 data_size, UINT32* actual_size) noexcept {
        if (!effect) return E_INVALIDARG;
        return static_cast<const LiquidGlassEffectImpl*>(static_cast<const ID2D1EffectImpl*>(effect))->GetParams(data, data_size, actual_size);
    }

    HRESULT SetParams(const BYTE* data, UINT32 data_size) noexcept {
        if (!data || data_size != sizeof(LiquidGlassEffectPayload) || !draw_transform_) {
            return E_INVALIDARG;
        }
        LiquidGlassEffectPayload payload{};
        std::memcpy(&payload, data, sizeof(payload));
        draw_transform_->SetPayload(payload);
        return S_OK;
    }

    HRESULT GetParams(BYTE* data, UINT32 data_size, UINT32* actual_size) const noexcept {
        if (actual_size) *actual_size = static_cast<UINT32>(sizeof(LiquidGlassEffectPayload));
        if (!data) return S_OK;
        if (data_size < sizeof(LiquidGlassEffectPayload) || !draw_transform_) {
            return E_INVALIDARG;
        }
        std::memcpy(data, &draw_transform_->Payload(), sizeof(LiquidGlassEffectPayload));
        return S_OK;
    }

private:
    LiquidGlassEffectImpl() = default;
    ~LiquidGlassEffectImpl() = default;

    std::atomic<ULONG> ref_count_{1};
    ComPtr<LiquidGlassDrawTransform> draw_transform_;
};

std::mutex g_reg_mtx;
std::vector<ID2D1Factory1*> g_registered_factories;

} // namespace

bool EnsureLiquidGlassEffectRegistered(ID2D1Factory1* factory) noexcept {
    if (!factory) return false;
    std::lock_guard<std::mutex> lock(g_reg_mtx);
    for (ID2D1Factory1* existing : g_registered_factories) {
        if (existing == factory) return true;
    }

    static constexpr PCWSTR kEffectXml =
        L"<?xml version='1.0'?>"
        L"<Effect>"
        L"  <Property name='DisplayName' type='string' value='PulseLiquidGlass'/>"
        L"  <Property name='Author' type='string' value='Pulse'/>"
        L"  <Property name='Category' type='string' value='Material'/>"
        L"  <Property name='Description' type='string' value='Physical Liquid Glass'/>"
        L"  <Inputs>"
        L"    <Input name='Sharp'/>"
        L"    <Input name='Frost'/>"
        L"  </Inputs>"
        L"  <Property name='Params' type='blob'>"
        L"    <Property name='DisplayName' type='string' value='Params'/>"
        L"  </Property>"
        L"</Effect>";

    const D2D1_PROPERTY_BINDING bindings[] = {
        {L"Params", &LiquidGlassEffectImpl::SetParamsThunk, &LiquidGlassEffectImpl::GetParamsThunk},
    };

    const HRESULT hr = factory->RegisterEffectFromString(
        CLSID_PulseLiquidGlass,
        kEffectXml,
        bindings,
        ARRAYSIZE(bindings),
        &LiquidGlassEffectImpl::CreateEffect);
    if (FAILED(hr)) {
        ui::LogWindowMaterial("liquid glass effect registration failed hr=0x%08lX",
                              static_cast<unsigned long>(hr));
        return false;
    }
    try {
        g_registered_factories.push_back(factory);
    } catch (...) {}
    return true;
}

LiquidGlassPassPool::Slot* LiquidGlassPassPool::AcquireSlot(
    ID2D1Factory1* factory, ID2D1DeviceContext* dc) noexcept {
    if (!factory || !dc) return nullptr;
    if (owner_dc_.Get() != dc) {
        Reset();
        owner_dc_ = dc;
    }
    if (unavailable_) return nullptr;
    if (!EnsureLiquidGlassEffectRegistered(factory)) {
        unavailable_ = true;
        return nullptr;
    }

    if (cursor_ < slots_.size()) {
        return &slots_[cursor_++];
    }

    Slot s{};
    HRESULT hr = dc->CreateEffect(CLSID_D2D12DAffineTransform, &s.affine_sharp);
    if (SUCCEEDED(hr)) hr = dc->CreateEffect(CLSID_D2D12DAffineTransform, &s.affine_frost);
    if (SUCCEEDED(hr)) hr = dc->CreateEffect(CLSID_PulseLiquidGlass, &s.glass);
    if (FAILED(hr) || !s.affine_sharp || !s.affine_frost || !s.glass) {
        ui::LogWindowMaterial("liquid glass effect creation failed hr=0x%08lX; falling back",
                              static_cast<unsigned long>(hr));
        unavailable_ = true;
        return nullptr;
    }

    s.affine_sharp->SetValue(D2D1_2DAFFINETRANSFORM_PROP_INTERPOLATION_MODE,
                             D2D1_2DAFFINETRANSFORM_INTERPOLATION_MODE_LINEAR);
    s.affine_sharp->SetValue(D2D1_2DAFFINETRANSFORM_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
    s.affine_frost->SetValue(D2D1_2DAFFINETRANSFORM_PROP_INTERPOLATION_MODE,
                             D2D1_2DAFFINETRANSFORM_INTERPOLATION_MODE_LINEAR);
    s.affine_frost->SetValue(D2D1_2DAFFINETRANSFORM_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);

    ComPtr<ID2D1Image> sharp_img;
    ComPtr<ID2D1Image> frost_img;
    s.affine_sharp->GetOutput(&sharp_img);
    s.affine_frost->GetOutput(&frost_img);
    s.glass->SetInput(0, sharp_img.Get());
    s.glass->SetInput(1, frost_img.Get());

    try {
        slots_.push_back(std::move(s));
        return &slots_[cursor_++];
    } catch (...) {
        return nullptr;
    }
}

bool LiquidGlassPassPool::DrawCard(ID2D1Factory1* factory,
                                   ID2D1DeviceContext* dc,
                                   ID2D1Image* sharp_source,
                                   uint32_t sharp_w,
                                   uint32_t sharp_h,
                                   ID2D1Image* frost_source,
                                   uint32_t frost_w,
                                   uint32_t frost_h,
                                   uint32_t screen_w,
                                   uint32_t screen_h,
                                   float origin_x_px,
                                   float origin_y_px,
                                   float atlas_shift_x,
                                   float atlas_shift_y,
                                   const GlassDrawPlan& plan) noexcept {
    if (!plan.visible || !dc || !sharp_source || sharp_w == 0 || sharp_h == 0 ||
        screen_w == 0 || screen_h == 0) {
        return false;
    }
    if (!frost_source || frost_w == 0 || frost_h == 0) {
        frost_source = sharp_source;
        frost_w = sharp_w;
        frost_h = sharp_h;
    }

    Slot* slot = AcquireSlot(factory, dc);
    if (!slot) return false;

    const auto m_sharp = ComputeGlassSourceMatrix(
        sharp_w, sharp_h, screen_w, screen_h, origin_x_px, origin_y_px, atlas_shift_x, atlas_shift_y);
    const auto m_frost = ComputeGlassSourceMatrix(
        frost_w, frost_h, screen_w, screen_h, origin_x_px, origin_y_px, atlas_shift_x, atlas_shift_y);

    const D2D1_MATRIX_3X2_F d2d_sharp = D2D1::Matrix3x2F(
        m_sharp.m11, m_sharp.m12, m_sharp.m21, m_sharp.m22, m_sharp.dx, m_sharp.dy);
    const D2D1_MATRIX_3X2_F d2d_frost = D2D1::Matrix3x2F(
        m_frost.m11, m_frost.m12, m_frost.m21, m_frost.m22, m_frost.dx, m_frost.dy);

    slot->affine_sharp->SetInput(0, sharp_source);
    slot->affine_sharp->SetValue(D2D1_2DAFFINETRANSFORM_PROP_TRANSFORM_MATRIX, d2d_sharp);
    slot->affine_frost->SetInput(0, frost_source);
    slot->affine_frost->SetValue(D2D1_2DAFFINETRANSFORM_PROP_TRANSFORM_MATRIX, d2d_frost);

    LiquidGlassEffectPayload payload{};
    payload.constants = plan.constants;
    if (payload.constants.reference_mode > 0.5f) {
        payload.constants.reference_height = static_cast<float>(screen_h);
        payload.constants.lens_scale = 0.05f * screen_h * payload.constants.reference_dpi;
        const float max_snell = std::sqrt(std::max(0.0f,
            payload.constants.ref_ior * payload.constants.ref_ior - 1.0f));
        payload.constants.refraction_pad = std::min(382.0f, std::ceil(
            payload.constants.lens_scale * max_snell * (1.0f + 0.02f * payload.constants.dispersion_gain) +
            payload.constants.blur_radius + 2.0f));
        if (payload.constants.blur_radius > 1) {
            if (!slot->studio_blur && FAILED(dc->CreateEffect(CLSID_D2D1GaussianBlur, &slot->studio_blur))) return false;
            ComPtr<ID2D1Image> sharp_aligned;
            slot->affine_sharp->GetOutput(&sharp_aligned);
            slot->studio_blur->SetInput(0, sharp_aligned.Get());
            slot->studio_blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, payload.constants.blur_radius / 3.0f);
            slot->studio_blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
            ComPtr<ID2D1Image> blurred;
            slot->studio_blur->GetOutput(&blurred);
            slot->glass->SetInput(1, blurred.Get());
        }
    } else {
        ComPtr<ID2D1Image> frosted;
        slot->affine_frost->GetOutput(&frosted);
        slot->glass->SetInput(1, frosted.Get());
    }

    payload.output_rect_px = plan.output_rect_px;
    payload.input_bounds_px = {static_cast<int32_t>(std::floor(m_sharp.dx)),static_cast<int32_t>(std::floor(m_sharp.dy)),
        static_cast<int32_t>(std::ceil(m_sharp.dx+screen_w)),static_cast<int32_t>(std::ceil(m_sharp.dy+screen_h))};
    const auto requested=BoundedGlassInputRect(payload.output_rect_px,payload.input_bounds_px,
        static_cast<int32_t>(std::ceil(payload.constants.refraction_pad))+2);
    if (!GlassInputWithinBudget(requested)) return false; // Existing caller material fallback; no oversized intermediate.

    if (FAILED(slot->glass->SetValue(0u, reinterpret_cast<const BYTE*>(&payload), sizeof(payload)))) {
        return false;
    }

    const D2D1_UNIT_MODE prev_unit = dc->GetUnitMode();
    float prev_dpi_x = 96.0f;
    float prev_dpi_y = 96.0f;
    dc->GetDpi(&prev_dpi_x, &prev_dpi_y);
    D2D1_MATRIX_3X2_F prev_xf{};
    dc->GetTransform(&prev_xf);

    // Source sharp/frost bitmaps are 96 DPI; force 96 DPI alongside PIXELS mode so
    // Direct2D does not inject DpiCompensation zoom or edge dipole shifts on >100% DPI screens.
    dc->SetDpi(96.0f, 96.0f);
    dc->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    dc->DrawImage(slot->glass.Get(), D2D1_INTERPOLATION_MODE_LINEAR, D2D1_COMPOSITE_MODE_SOURCE_OVER);
    dc->SetTransform(prev_xf);
    dc->SetUnitMode(prev_unit);
    dc->SetDpi(prev_dpi_x, prev_dpi_y);
    return true;
}

} // namespace pulse::desktop::render

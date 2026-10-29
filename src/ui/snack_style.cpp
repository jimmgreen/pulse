// snack_style.cpp - the shared plate of the M7i capsules (see snack_style.h).
#include "snack_style.h"

#include <d2d1effects.h>

#include <algorithm>

#include "window_material.h"

namespace pulse::ui {
namespace {
// CLSID_D2D1Shadow (d2d1.lib does not export effect CLSIDs; same as the menus).
constexpr GUID kShadowClsid = {0xC67EA361, 0x1863, 0x4e69, {0x89, 0xdb, 0x69, 0x5d, 0x3e, 0x9a, 0x5b, 0x6b}};
}  // namespace

void DrawElevationShadow(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, float radius, float deviation, float drop,
                         const D2D1_COLOR_F& color, float opacity) {
    const float alpha = color.a * (std::min)(opacity, 1.0f);
    if (!dc || !(rect.right > rect.left) || !(rect.bottom > rect.top) || !(alpha > 0.0f)) return;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<ID2D1CommandList> silhouette;
    if (FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(0x000000), &brush)) ||
        FAILED(dc->CreateCommandList(&silhouette)) || !silhouette.get())
        return;
    // Record untransformed: DrawImage applies the caller's transform once.
    D2D1_MATRIX_3X2_F transform{};
    dc->GetTransform(&transform);
    ComPtr<ID2D1Image> previous;
    dc->GetTarget(&previous);
    dc->SetTarget(silhouette.get());
    dc->SetTransform(D2D1::IdentityMatrix());
    dc->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.get());
    dc->SetTransform(transform);
    dc->SetTarget(previous.get());
    ComPtr<ID2D1Effect> shadow;
    if (FAILED(silhouette->Close()) || FAILED(dc->CreateEffect(kShadowClsid, &shadow)) || !shadow.get()) return;
    shadow->SetInput(0, silhouette.get());
    shadow->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, deviation);
    shadow->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(color.r, color.g, color.b, alpha));
    dc->DrawImage(shadow.get(), D2D1::Point2F(0.0f, drop), D2D1_INTERPOLATION_MODE_LINEAR);
}

void DrawSnackPlate(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, float radius, const SnackPalette& palette,
                    float scale, bool high_contrast, float opacity) {
    if (!dc || !(rect.right > rect.left) || !(rect.bottom > rect.top) || !(opacity > 0.0f)) return;
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(palette.plate, &brush))) return;
    const D2D1_ROUNDED_RECT shape = D2D1::RoundedRect(rect, radius, radius);
    const D2D1_ROUNDED_RECT edge = D2D1::RoundedRect(
        D2D1::RectF(rect.left + 0.5f, rect.top + 0.5f, rect.right - 0.5f, rect.bottom - 0.5f), radius - 0.5f,
        radius - 0.5f);
    if (high_contrast) {
        dc->FillRoundedRectangle(shape, brush.get());
        brush->SetColor(palette.stroke);
        dc->DrawRoundedRectangle(edge, brush.get(), 1.0f);
        return;
    }

    // Sample first so the shadow does not darken the glass.
    const float blur = 16.0f * scale;
    TargetSample sample;
    const bool sampled = SampleTarget(dc, rect, 2.0f * blur, sample);
    const float fade = (std::min)(opacity, 1.0f);

    // Fluent flyout elevation: a wide soft shadow below and a tight one around.
    DrawElevationShadow(dc, rect, radius, 8.0f * scale, 6.0f * scale, palette.shadow_key, fade);
    DrawElevationShadow(dc, rect, radius, 1.0f * scale, 0.5f * scale, palette.shadow_ambient, fade);

    // The shadow fades with its alpha; the glass in a layer (opened only now:
    // the silhouette above retargets the context, which a layer forbids).
    const bool fading = fade < 0.999f;
    if (fading)
        dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                             D2D1::IdentityMatrix(), fade),
                      nullptr);
    if (sampled && DrawAcrylicSample(dc, sample, rect, radius, palette.dark, blur)) {
        brush->SetColor(palette.veil);
    } else {
        brush->SetColor(palette.plate);
    }
    dc->FillRoundedRectangle(shape, brush.get());
    brush->SetColor(palette.stroke);
    dc->DrawRoundedRectangle(edge, brush.get(), 1.0f);
    if (fading) dc->PopLayer();
}
}  // namespace pulse::ui

// liquid_glass_effect.h - Direct2D 1.1 custom GPU pixel-shader effect (ps_4_0) and
// pooled per-frame pass runner for physical Liquid Glass desktop cards & menus.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <wrl/client.h>

#include "host/render/liquid_glass_params.h"

#include <cstdint>
#include <vector>

namespace pulse::desktop::render {

// {8A7F41C2-5D3E-4B91-9C62-1E44B8307A19}
inline constexpr GUID CLSID_PulseLiquidGlass = {
    0x8a7f41c2, 0x5d3e, 0x4b91, {0x9c, 0x62, 0x1e, 0x44, 0xb8, 0x30, 0x7a, 0x19}};

struct alignas(16) LiquidGlassEffectPayload {
    LiquidGlassConstants constants{};
    GlassIntRect output_rect_px{0, 0, 1, 1};
    GlassIntRect input_bounds_px{-16384,-16384,16384,16384};
};
static_assert(sizeof(LiquidGlassEffectPayload) == sizeof(LiquidGlassConstants) + 32, "Payload must remain 16-byte aligned");

// Registers `CLSID_PulseLiquidGlass` on `factory` (idempotent across calls on the same factory).
bool EnsureLiquidGlassEffectRegistered(ID2D1Factory1* factory) noexcept;

// Per-device-context pool of `(AffineSharp, AffineFrost, LiquidGlassEffect)` triples so multiple
// cards drawn inside a single BeginDraw/EndDraw batch never clobber each other's constant buffers,
// and steady-state frames perform zero COM allocations.
class LiquidGlassPassPool {
public:
    static constexpr size_t kMaxRetainedSlots = 48;

    LiquidGlassPassPool() = default;
    ~LiquidGlassPassPool() = default;
    LiquidGlassPassPool(const LiquidGlassPassPool&) = delete;
    LiquidGlassPassPool& operator=(const LiquidGlassPassPool&) = delete;

    void BeginFrame() noexcept { cursor_ = 0; }
    void EndFrame() noexcept {
        if (slots_.size() > kMaxRetainedSlots && cursor_ < slots_.size()) {
            slots_.resize(std::max(cursor_, kMaxRetainedSlots));
        }
    }
    void Reset() noexcept {
        slots_.clear();
        owner_dc_.Reset();
        cursor_ = 0;
        unavailable_ = false;
    }

    bool Unavailable() const noexcept { return unavailable_; }
    size_t SlotCount() const noexcept { return slots_.size(); }

    // Draws a single Liquid Glass card onto `dc` in physical target pixel coordinates,
    // restoring `dc`'s previous unit mode and world transform before returning.
    bool DrawCard(ID2D1Factory1* factory,
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
                  const GlassDrawPlan& plan) noexcept;

private:
    struct Slot {
        Microsoft::WRL::ComPtr<ID2D1Effect> affine_sharp;
        Microsoft::WRL::ComPtr<ID2D1Effect> affine_frost;
        Microsoft::WRL::ComPtr<ID2D1Effect> glass;
        Microsoft::WRL::ComPtr<ID2D1Effect> studio_blur;
    };

    Slot* AcquireSlot(ID2D1Factory1* factory, ID2D1DeviceContext* dc) noexcept;

    Microsoft::WRL::ComPtr<ID2D1DeviceContext> owner_dc_;
    std::vector<Slot> slots_;
    size_t cursor_ = 0;
    bool unavailable_ = false;
};

} // namespace pulse::desktop::render


// bloom_accent_picker.h — Motion circular color picker, Direct2D port.
// Layout, hue formula, and spring numbers match
// https://motion.dev/examples/react-color-picker (CSS mix-blend approximated).
#pragma once
#include "FluentTokens.h"
#include <array>
#include <cstdint>

namespace pulse::ui {

inline constexpr int kBloomDotCount = 19;
inline constexpr int kBloomGlowCount = 6;
inline constexpr int kBloomStopCount = 13; // 0deg .. 360deg step 30
inline constexpr float kBloomExampleSize = 140.0f;
inline constexpr float kBloomPickerDip = 72.0f;

struct BloomDotGeom {
    int ring = 0;
    int index = 0;
    int total = 1;
    float angle = 0.0f;
    float hue = 0.0f;
    float base_x = 0.0f;
    float base_y = 0.0f;
};

float BloomDotHue(int ring, int index, int total_in_ring) noexcept;
BloomDotGeom BloomDotAt(int dot_index) noexcept;
D2D1_COLOR_F BloomHsl(float hue, float sat, float light) noexcept;
uint32_t BloomDotRgb(int dot_index) noexcept;
D2D1_COLOR_F BloomDotColor(int dot_index) noexcept;
bool BloomStepSpring(float& value, float& velocity, float target,
                     float stiffness, float damping, float dt) noexcept;

class BloomAccentPicker {
public:
    BloomAccentPicker();

    void SetDisk(const D2D1_RECT_F& disk) noexcept;
    void SetPointer(float x, float y, bool valid) noexcept;
    void SetPressed(int dot_index) noexcept; // -1 = none
    int Pressed() const noexcept { return pressed_index_; }

    // follow=true → rainbow ring (Windows accent). snap skips the morph.
    void SetSelection(bool follow, uint32_t rgb, bool snap);

    bool Tick(float dt);
    int HitDot(float x, float y) const;
    bool HasDisk() const noexcept { return disk_w_ > 1.0f; }

    void Draw(ID2D1DeviceContext* dc, const Theme& theme) const;

private:
    struct Spring {
        float value = 0.0f;
        float velocity = 0.0f;
        float target = 0.0f;
        float stiffness = 100.0f;
        float damping = 30.0f;
        bool Step(float dt) noexcept;
        void Snap(float v) noexcept {
            value = target = v;
            velocity = 0.0f;
        }
    };

    void RebuildStops(bool follow, uint32_t rgb, bool snap);
    void LocalFromPx(float px, float py, float& lx, float& ly) const noexcept;
    float UnitToPx() const noexcept;
    D2D1_POINT_2F Center() const noexcept;
    D2D1_COLOR_F SampleConic(float css_deg) const noexcept;

    D2D1_RECT_F disk_{};
    float disk_w_ = 0.0f;
    float pointer_x_ = 0.0f;
    float pointer_y_ = 0.0f;
    bool pointer_valid_ = false;
    int hover_index_ = -1;
    int pressed_index_ = -1;
    bool follow_ = true;
    uint32_t selected_rgb_ = 0;

    std::array<Spring, kBloomDotCount> push_x_{};
    std::array<Spring, kBloomDotCount> push_y_{};
    std::array<Spring, kBloomDotCount> scale_{};
    std::array<Spring, kBloomDotCount> ring_op_{};
    std::array<Spring, kBloomGlowCount> glow_op_{};
    std::array<Spring, kBloomGlowCount> glow_sc_{};
    Spring ring_scale_{};
    Spring solid_scale_{};
    std::array<D2D1_COLOR_F, kBloomStopCount> stop_from_{};
    std::array<D2D1_COLOR_F, kBloomStopCount> stop_to_{};
    std::array<D2D1_COLOR_F, kBloomStopCount> stop_now_{};
    float stop_t_ = 1.0f;
};

} // namespace pulse::ui

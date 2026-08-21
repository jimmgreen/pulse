// bloom_accent_picker.cpp — Motion circular picker springs + D2D draw.
#include "bloom_accent_picker.h"
#include "ui_compositor.h"
#include <d2d1_3.h>
#include <algorithm>
#include <cmath>

namespace pulse::ui {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;
constexpr float kDotSize = 32.0f;
constexpr float kStopDuration = 0.2f;
constexpr float kRingOpDuration = 0.13f;
constexpr float kPushMagnitude = 5.0f;
constexpr float kMinDistance = 80.0f;
constexpr float kHoverScale = 1.5f;
constexpr float kTapScale = 1.2f;

float Clamp01(float v) noexcept {
    return std::clamp(v, 0.0f, 1.0f);
}

float EaseOutCubic(float t) noexcept {
    t = Clamp01(t);
    const float inv = 1.0f - t;
    return 1.0f - inv * inv * inv;
}

D2D1_COLOR_F LerpColor(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) noexcept {
    t = Clamp01(t);
    return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                        a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}

uint32_t RgbFromColor(const D2D1_COLOR_F& c) noexcept {
    auto ch = [](float v) {
        return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
    };
    return (ch(c.r) << 16) | (ch(c.g) << 8) | ch(c.b);
}

void SpringFromVisual(float visual_duration, float bounce, float& stiffness, float& damping) {
    const float omega = kTwoPi / visual_duration;
    stiffness = omega * omega;
    damping = 2.0f * (1.0f - bounce) * omega;
}

D2D1_COLOR_F StopColor(int i) noexcept {
    return BloomHsl(static_cast<float>(i * 30), 0.90f, 0.60f);
}

} // namespace

float BloomDotHue(int ring, int index, int total_in_ring) noexcept {
    if (ring <= 0 || total_in_ring <= 0) return 0.0f;
    const float angle = (static_cast<float>(index) / static_cast<float>(total_in_ring)) * kTwoPi;
    const float hue_degrees = angle * 180.0f / kPi - 90.0f - 180.0f;
    float hue = std::fmod(hue_degrees, 360.0f);
    if (hue < 0.0f) hue += 360.0f;
    return hue;
}

BloomDotGeom BloomDotAt(int dot_index) noexcept {
    BloomDotGeom g;
    if (dot_index <= 0) {
        g.ring = 0;
        g.index = 0;
        g.total = 1;
        return g;
    }
    if (dot_index <= 6) {
        g.ring = 1;
        g.index = dot_index - 1;
        g.total = 6;
    } else {
        g.ring = 2;
        g.index = std::clamp(dot_index - 7, 0, 11);
        g.total = 12;
    }
    g.angle = (static_cast<float>(g.index) / static_cast<float>(g.total)) * kTwoPi;
    g.hue = BloomDotHue(g.ring, g.index, g.total);
    const float radius = static_cast<float>(g.ring) * 20.0f;
    g.base_x = std::cos(g.angle) * radius;
    g.base_y = std::sin(g.angle) * radius;
    return g;
}

D2D1_COLOR_F BloomHsl(float hue, float sat, float light) noexcept {
    hue = std::fmod(hue, 360.0f);
    if (hue < 0.0f) hue += 360.0f;
    sat = Clamp01(sat);
    light = Clamp01(light);
    const float c = (1.0f - std::fabs(2.0f * light - 1.0f)) * sat;
    const float hp = hue / 60.0f;
    const float x = c * (1.0f - std::fabs(std::fmod(hp, 2.0f) - 1.0f));
    float r = 0.0f, g = 0.0f, b = 0.0f;
    if (hp < 1.0f)      { r = c; g = x; }
    else if (hp < 2.0f) { r = x; g = c; }
    else if (hp < 3.0f) { g = c; b = x; }
    else if (hp < 4.0f) { g = x; b = c; }
    else if (hp < 5.0f) { r = x; b = c; }
    else                { r = c; b = x; }
    const float m = light - c * 0.5f;
    return D2D1::ColorF(r + m, g + m, b + m, 1.0f);
}

uint32_t BloomDotRgb(int dot_index) noexcept {
    return RgbFromColor(BloomDotColor(dot_index));
}

D2D1_COLOR_F BloomDotColor(int dot_index) noexcept {
    const BloomDotGeom g = BloomDotAt(dot_index);
    if (g.ring == 0) return D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
    if (g.ring == 1) return BloomHsl(g.hue, 0.60f, 0.85f);
    return BloomHsl(g.hue, 0.90f, 0.60f);
}

bool BloomStepSpring(float& value, float& velocity, float target,
                     float stiffness, float damping, float dt) noexcept {
    if (dt <= 0.0f) return false;
    if (!std::isfinite(value) || !std::isfinite(velocity) || !std::isfinite(target) ||
        !std::isfinite(stiffness) || !std::isfinite(damping)) {
        value = std::isfinite(target) ? target : 0.0f;
        velocity = 0.0f;
        return false;
    }
    const float x0 = value - target;
    const float v0 = velocity;
    if (std::fabs(x0) < 0.01f && std::fabs(v0) < 2.0f) {
        value = target;
        velocity = 0.0f;
        return false;
    }

    auto critical = [&]() {
        const float omega0 = std::sqrt((std::max)(stiffness, 0.0001f));
        const float a = x0;
        const float b = v0 + omega0 * x0;
        const float e = std::exp(-omega0 * dt);
        const float x = e * (a + b * dt);
        const float v = e * (b - omega0 * (a + b * dt));
        value = x + target;
        velocity = v;
    };

    const float omega0 = std::sqrt((std::max)(stiffness, 0.0001f));
    const float zeta = damping / (2.0f * omega0);
    if (zeta < 0.9999f) {
        const float omega = omega0 * std::sqrt((std::max)(0.0f, 1.0f - zeta * zeta));
        const float a = x0;
        const float b = (v0 + zeta * omega0 * x0) / (std::max)(omega, 0.0001f);
        const float e = std::exp(-zeta * omega0 * dt);
        const float cth = std::cos(omega * dt);
        const float sth = std::sin(omega * dt);
        const float x = e * (a * cth + b * sth);
        velocity = -zeta * omega0 * x + e * (-a * omega * sth + b * omega * cth);
        value = x + target;
    } else if (zeta > 1.0001f) {
        const float disc = std::sqrt((std::max)(0.0f, zeta * zeta - 1.0f));
        const float r1 = -omega0 * (zeta - disc);
        const float r2 = -omega0 * (zeta + disc);
        const float denom = r2 - r1;
        if (std::fabs(denom) < 1.0e-5f || r1 > 0.0f || r2 > 0.0f) {
            critical();
        } else {
            const float c2 = (v0 - r1 * x0) / denom;
            const float c1 = x0 - c2;
            const float e1 = std::exp(r1 * dt);
            const float e2 = std::exp(r2 * dt);
            const float x = c1 * e1 + c2 * e2;
            velocity = c1 * r1 * e1 + c2 * r2 * e2;
            value = x + target;
        }
    } else {
        critical();
    }

    if (!std::isfinite(value) || !std::isfinite(velocity)) {
        value = target;
        velocity = 0.0f;
        return false;
    }
    if (std::fabs(value - target) < 0.01f && std::fabs(velocity) < 2.0f) {
        value = target;
        velocity = 0.0f;
        return false;
    }
    return true;
}

bool BloomAccentPicker::Spring::Step(float dt) noexcept {
    return BloomStepSpring(value, velocity, target, stiffness, damping, dt);
}

BloomAccentPicker::BloomAccentPicker() {
    for (int i = 0; i < kBloomDotCount; ++i) {
        push_x_[static_cast<size_t>(i)].stiffness = 100.0f;
        push_x_[static_cast<size_t>(i)].damping = 30.0f;
        push_y_[static_cast<size_t>(i)].stiffness = 100.0f;
        push_y_[static_cast<size_t>(i)].damping = 30.0f;
        scale_[static_cast<size_t>(i)].stiffness = 200.0f;
        scale_[static_cast<size_t>(i)].damping = 30.0f;
        scale_[static_cast<size_t>(i)].Snap(1.0f);
        ring_op_[static_cast<size_t>(i)].stiffness = 200.0f;
        ring_op_[static_cast<size_t>(i)].damping = 30.0f;
    }
    for (int i = 0; i < kBloomGlowCount; ++i) {
        glow_op_[static_cast<size_t>(i)].stiffness = 100.0f;
        glow_op_[static_cast<size_t>(i)].damping = 30.0f;
        glow_op_[static_cast<size_t>(i)].Snap(0.15f);
        glow_sc_[static_cast<size_t>(i)].stiffness = 100.0f;
        glow_sc_[static_cast<size_t>(i)].damping = 30.0f;
        glow_sc_[static_cast<size_t>(i)].Snap(1.0f);
    }
    float k = 0.0f, d = 0.0f;
    SpringFromVisual(0.2f, 0.0f, k, d);
    ring_scale_.stiffness = k;
    ring_scale_.damping = d;
    ring_scale_.Snap(1.0f);
    SpringFromVisual(0.2f, 0.2f, k, d);
    solid_scale_.stiffness = k;
    solid_scale_.damping = d;
    solid_scale_.Snap(0.98f);
    for (int i = 0; i < kBloomStopCount; ++i) {
        stop_now_[static_cast<size_t>(i)] = StopColor(i);
        stop_from_[static_cast<size_t>(i)] = stop_now_[static_cast<size_t>(i)];
        stop_to_[static_cast<size_t>(i)] = stop_now_[static_cast<size_t>(i)];
    }
    stop_t_ = 1.0f;
}

void BloomAccentPicker::SetDisk(const D2D1_RECT_F& disk) noexcept {
    disk_ = disk;
    disk_w_ = disk.right - disk.left;
}

void BloomAccentPicker::SetPointer(float x, float y, bool valid) noexcept {
    pointer_x_ = x;
    pointer_y_ = y;
    pointer_valid_ = valid;
}

void BloomAccentPicker::SetPressed(int dot_index) noexcept {
    pressed_index_ = (dot_index >= 0 && dot_index < kBloomDotCount) ? dot_index : -1;
}

void BloomAccentPicker::RebuildStops(bool follow, uint32_t rgb, bool snap) {
    D2D1_COLOR_F selected = HexColor(rgb);
    for (int i = 0; i < kBloomStopCount; ++i) {
        stop_from_[static_cast<size_t>(i)] = stop_now_[static_cast<size_t>(i)];
        stop_to_[static_cast<size_t>(i)] = follow ? StopColor(i) : selected;
        if (snap) stop_now_[static_cast<size_t>(i)] = stop_to_[static_cast<size_t>(i)];
    }
    stop_t_ = snap ? 1.0f : 0.0f;
}

void BloomAccentPicker::SetSelection(bool follow, uint32_t rgb, bool snap) {
    const bool changed = follow_ != follow || (!follow && selected_rgb_ != rgb);
    follow_ = follow;
    selected_rgb_ = rgb;
    if (!changed && !snap) return;
    RebuildStops(follow, rgb, snap);
    float k = 0.0f, d = 0.0f;
    if (follow) {
        SpringFromVisual(0.2f, 0.0f, k, d);
        ring_scale_.stiffness = k;
        ring_scale_.damping = d;
        ring_scale_.target = 1.0f;
        if (snap) ring_scale_.Snap(1.0f);
        solid_scale_.target = 0.98f;
        if (snap) solid_scale_.Snap(0.98f);
    } else {
        SpringFromVisual(0.2f, 0.8f, k, d);
        ring_scale_.stiffness = k;
        ring_scale_.damping = d;
        ring_scale_.target = 1.1f;
        if (snap) {
            ring_scale_.Snap(1.1f);
        } else {
            ring_scale_.velocity = 2.0f;
        }
        solid_scale_.target = 0.9f;
        if (snap) solid_scale_.Snap(0.9f);
    }
}

D2D1_POINT_2F BloomAccentPicker::Center() const noexcept {
    return D2D1::Point2F((disk_.left + disk_.right) * 0.5f,
                         (disk_.top + disk_.bottom) * 0.5f);
}

float BloomAccentPicker::UnitToPx() const noexcept {
    return disk_w_ > 1.0f ? disk_w_ / kBloomExampleSize : 1.0f;
}

void BloomAccentPicker::LocalFromPx(float px, float py, float& lx, float& ly) const noexcept {
    const D2D1_POINT_2F c = Center();
    const float u = UnitToPx();
    lx = (px - c.x) / u;
    ly = (py - c.y) / u;
}

bool BloomAccentPicker::Tick(float dt) {
    dt = std::clamp(dt, 0.001f, 0.05f);
    bool live = stop_t_ < 1.0f;
    if (stop_t_ < 1.0f) {
        stop_t_ = std::min(1.0f, stop_t_ + dt / kStopDuration);
        const float e = EaseOutCubic(stop_t_);
        for (int i = 0; i < kBloomStopCount; ++i)
            stop_now_[static_cast<size_t>(i)] =
                LerpColor(stop_from_[static_cast<size_t>(i)],
                          stop_to_[static_cast<size_t>(i)], e);
        if (stop_t_ < 1.0f) live = true;
    }

    float lx = 0.0f, ly = 0.0f;
    if (pointer_valid_) LocalFromPx(pointer_x_, pointer_y_, lx, ly);

    hover_index_ = -1;
    float best = 1.0e9f;
    const float hit_r = kDotSize * 0.5f;
    const bool in_disk = pointer_valid_ &&
        (lx * lx + ly * ly) <= (kBloomExampleSize * 0.5f) * (kBloomExampleSize * 0.5f);
    for (int i = 0; i < kBloomDotCount; ++i) {
        const BloomDotGeom g = BloomDotAt(i);
        const float px = g.base_x + push_x_[static_cast<size_t>(i)].value;
        const float py = g.base_y + push_y_[static_cast<size_t>(i)].value;
        const float dist = std::sqrt((lx - px) * (lx - px) + (ly - py) * (ly - py));
        const float r = hit_r * std::clamp(scale_[static_cast<size_t>(i)].value, 0.85f, kHoverScale);
        if (pointer_valid_ && dist <= r && dist < best) {
            best = dist;
            hover_index_ = i;
        }
    }

    for (int i = 0; i < kBloomDotCount; ++i) {
        float tx = 0.0f, ty = 0.0f;
        if (in_disk) {
            const BloomDotGeom g = BloomDotAt(i);
            const float dx = g.base_x - lx;
            const float dy = g.base_y - ly;
            const float dist = std::sqrt(dx * dx + dy * dy);
            if (dist < kMinDistance) {
                const float mag = (1.0f - dist / kMinDistance) * kPushMagnitude;
                const float ang = std::atan2(dy, dx);
                tx = std::cos(ang) * mag;
                ty = std::sin(ang) * mag;
            }
        }
        push_x_[static_cast<size_t>(i)].target = tx;
        push_y_[static_cast<size_t>(i)].target = ty;
        live = push_x_[static_cast<size_t>(i)].Step(dt) || live;
        live = push_y_[static_cast<size_t>(i)].Step(dt) || live;

        float scale_to = 1.0f;
        if (pressed_index_ == i) scale_to = kTapScale;
        else if (hover_index_ == i) scale_to = kHoverScale;
        scale_[static_cast<size_t>(i)].target = scale_to;
        live = scale_[static_cast<size_t>(i)].Step(dt) || live;
        if (!std::isfinite(scale_[static_cast<size_t>(i)].value) ||
            scale_[static_cast<size_t>(i)].value < 0.5f ||
            scale_[static_cast<size_t>(i)].value > 1.8f) {
            scale_[static_cast<size_t>(i)].Snap(scale_to);
        }

        const float ring_to = (hover_index_ == i) ? 0.4f : 0.0f;
        float& ring_v = ring_op_[static_cast<size_t>(i)].value;
        const float ring_delta = ring_to - ring_v;
        const float ring_max = 0.4f * dt / kRingOpDuration;
        if (std::fabs(ring_delta) <= ring_max) {
            ring_v = ring_to;
        } else {
            ring_v += (ring_delta > 0.0f ? ring_max : -ring_max);
            live = true;
        }
    }

    live = ring_scale_.Step(dt) || live;
    live = solid_scale_.Step(dt) || live;
    return live;
}

int BloomAccentPicker::HitDot(float x, float y) const {
    if (disk_w_ <= 1.0f) return -1;
    float lx = 0.0f, ly = 0.0f;
    LocalFromPx(x, y, lx, ly);
    int hit = -1;
    float best = 1.0e9f;
    const float hit_r = kDotSize * 0.5f;
    for (int i = 0; i < kBloomDotCount; ++i) {
        const BloomDotGeom g = BloomDotAt(i);
        const float px = g.base_x + push_x_[static_cast<size_t>(i)].value;
        const float py = g.base_y + push_y_[static_cast<size_t>(i)].value;
        const float dist = std::sqrt((lx - px) * (lx - px) + (ly - py) * (ly - py));
        const float r = hit_r * std::clamp(scale_[static_cast<size_t>(i)].value, 0.85f, kHoverScale);
        if (dist <= r && dist < best) {
            best = dist;
            hit = i;
        }
    }
    return hit;
}

D2D1_COLOR_F BloomAccentPicker::SampleConic(float css_deg) const noexcept {
    css_deg = std::fmod(css_deg, 360.0f);
    if (css_deg < 0.0f) css_deg += 360.0f;
    const float span = 360.0f / static_cast<float>(kBloomStopCount - 1);
    const float slot = css_deg / span;
    const int i0 = std::clamp(static_cast<int>(slot), 0, kBloomStopCount - 2);
    const float local = slot - static_cast<float>(i0);
    return LerpColor(stop_now_[static_cast<size_t>(i0)],
                     stop_now_[static_cast<size_t>(i0 + 1)], local);
}

void BloomAccentPicker::Draw(ID2D1DeviceContext* dc, const Theme& theme) const {
    if (!dc || disk_w_ <= 1.0f) return;
    (void)theme;
    const D2D1_POINT_2F c = Center();
    const float u = UnitToPx();
    const float radius = disk_w_ * 0.5f;
    const float max_dot_r = radius * 0.42f;

    auto fill_ellipse = [&](D2D1_POINT_2F center, float r, D2D1_COLOR_F color) {
        if (!std::isfinite(r) || r < 0.5f) return;
        r = (std::min)(r, max_dot_r);
        ComPtr<ID2D1SolidColorBrush> br;
        dc->CreateSolidColorBrush(color, &br.p);
        if (br.get())
            dc->FillEllipse(D2D1::Ellipse(center, r, r), br.get());
    };
    auto stroke_ellipse = [&](D2D1_POINT_2F center, float r, D2D1_COLOR_F color, float width) {
        if (!std::isfinite(r) || r < 0.5f) return;
        r = (std::min)(r, max_dot_r);
        ComPtr<ID2D1SolidColorBrush> br;
        dc->CreateSolidColorBrush(color, &br.p);
        if (br.get())
            dc->DrawEllipse(D2D1::Ellipse(center, r, r), br.get(), width);
    };

    // Follow Windows: hairline rainbow (solid 0.98, ring 1.0). After a
    // color pick the ring springs to 1.1 and the hole to 0.9 — a thicker band.
    const float ring_sc = std::clamp(ring_scale_.value, 0.85f, 1.28f);
    const float solid_sc = std::clamp(solid_scale_.value, 0.82f, 1.0f);
    const float outer_r = radius * ring_sc;
    float inner_r = radius * solid_sc;
    if (inner_r > outer_r - 1.5f) inner_r = outer_r - 1.5f;

    ID2D1Factory* factory = nullptr;
    dc->GetFactory(&factory);
    ComPtr<ID2D1SolidColorBrush> ring_br;
    dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &ring_br.p);
    if (factory && ring_br.get()) {
        ComPtr<ID2D1StrokeStyle> round_cap;
        D2D1_STROKE_STYLE_PROPERTIES caps{};
        caps.startCap = D2D1_CAP_STYLE_ROUND;
        caps.endCap = D2D1_CAP_STYLE_ROUND;
        caps.dashCap = D2D1_CAP_STYLE_ROUND;
        caps.lineJoin = D2D1_LINE_JOIN_ROUND;
        caps.miterLimit = 1.0f;
        factory->CreateStrokeStyle(caps, nullptr, 0, &round_cap);
        constexpr int kSeg = 128;
        const float mid_r = (outer_r + inner_r) * 0.5f;
        const float width = (std::max)(1.8f, outer_r - inner_r);
        for (int i = 0; i < kSeg; ++i) {
            const float css0 = static_cast<float>(i) * (360.0f / kSeg);
            const float css1 = static_cast<float>(i + 1) * (360.0f / kSeg);
            ring_br->SetColor(SampleConic((css0 + css1) * 0.5f));
            const float a0 = css0 * kPi / 180.0f - kPi * 0.5f;
            const float a1 = css1 * kPi / 180.0f - kPi * 0.5f;
            dc->DrawLine(D2D1::Point2F(c.x + std::cos(a0) * mid_r, c.y + std::sin(a0) * mid_r),
                         D2D1::Point2F(c.x + std::cos(a1) * mid_r, c.y + std::sin(a1) * mid_r),
                         ring_br.get(), width, round_cap.get());
        }
    }

    for (int pass = 0; pass < kBloomDotCount; ++pass) {
        const int i = kBloomDotCount - 1 - pass;
        const BloomDotGeom g = BloomDotAt(i);
        const float sc = std::clamp(scale_[static_cast<size_t>(i)].value, 0.75f, 1.6f);
        const float dx = (g.base_x + push_x_[static_cast<size_t>(i)].value) * u;
        const float dy = (g.base_y + push_y_[static_cast<size_t>(i)].value) * u;
        const float r = kDotSize * 0.5f * sc * u;
        const D2D1_POINT_2F p = D2D1::Point2F(c.x + dx, c.y + dy);
        fill_ellipse(p, r, i == 0 ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f)
                                  : BloomDotColor(i));
        const float op = std::clamp(ring_op_[static_cast<size_t>(i)].value, 0.0f, 1.0f);
        if (op > 0.01f) {
            stroke_ellipse(p, r, D2D1::ColorF(1.0f, 1.0f, 1.0f, op),
                           (std::max)(1.0f, 2.0f * u));
        }
    }
}

} // namespace pulse::ui

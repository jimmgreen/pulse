// widget_painters.h - Reusable macOS 26 Tahoe widget drawing primitives for Direct2D.
// Keeps all text drawing routed through caller-provided LumaText callbacks so widget
// text caching, DPI alignment, and LumaText invariants remain strictly intact.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace pulse::desktop::render {

// Caller-supplied LumaText-backed text draw callback.
using WidgetTextDrawFn = std::function<void(const std::wstring& text,
                                            const D2D1_RECT_F& rect,
                                            uint32_t argb,
                                            float opacity,
                                            DWRITE_TEXT_ALIGNMENT align)>;

struct WorldClockCityEntry {
    std::wstring label;
    SYSTEMTIME local_time{};
    bool is_night = false;
    std::wstring day_label;     // "今天" / "明天" / "昨天"
    std::wstring offset_label;  // "+0 时" / "+1 时" / "−13 时"
};

// SF-Symbols-style vector weather icon (Boolean-combined cloud, crescent moon, glowing sun,
// rain/drizzle/snow/fog/thunderstorm). Adapts colors for light vs dark card ink.
void DrawTahoeWeatherIcon(ID2D1Factory* factory,
                          ID2D1DeviceContext* dc,
                          ID2D1SolidColorBrush* brush,
                          ID2D1StrokeStyle* round_stroke,
                          const D2D1_RECT_F& rect,
                          int wmo_code,
                          bool is_day,
                          bool dark_ink_light);

// Apple Weather diagonal location arrow right beside the city title.
void DrawTahoeLocationArrow(ID2D1Factory* factory,
                            ID2D1DeviceContext* dc,
                            ID2D1SolidColorBrush* brush,
                            float x,
                            float y,
                            uint32_t argb,
                            float opacity = 0.92f);

// Translucent inner glass sub-card with vertical sheen, top specular highlight, and crisp hairline rim.
void DrawTahoeGlassCard(ID2D1DeviceContext* dc,
                        ID2D1SolidColorBrush* brush,
                        const D2D1_RECT_F& rect,
                        float radius,
                        bool dark_ink_light);

// Subtle condition-aware atmospheric sky wash clipped inside Weather cards (preserves LiquidGlass refraction).
void DrawTahoeWeatherSkyWash(ID2D1Factory* factory,
                             ID2D1DeviceContext* dc,
                             const D2D1_RECT_F& rect,
                             float corner_radius,
                             int wmo_code,
                             bool is_day,
                             bool dark_ink_light);

// Compact rounded tinted squircle badge used behind widget header icons (Apple Reminders/Files/Notes style).
void DrawTahoeHeaderBadge(ID2D1DeviceContext* dc,
                          ID2D1SolidColorBrush* brush,
                          const D2D1_RECT_F& rect,
                          uint32_t accent_argb,
                          bool dark_ink_light);

// Soft radial ambient glow behind 1x1 launcher/system icons for dimensional macOS widget presence.
void DrawTahoeIconAura(ID2D1DeviceContext* dc,
                       float cx,
                       float cy,
                       float radius,
                       uint32_t accent_argb,
                       bool dark_ink_light);

// Luminous glowing circular progress ring (translucent track + soft outer halo + crisp rounded arc).
void DrawTahoeGlassRing(ID2D1Factory* factory,
                        ID2D1DeviceContext* dc,
                        ID2D1SolidColorBrush* brush,
                        ID2D1StrokeStyle* round_stroke,
                        float cx,
                        float cy,
                        float radius,
                        float stroke_w,
                        float percent,
                        uint32_t accent_argb,
                        bool dark_ink_light);

struct SparklineRange {
    float min_val = 0.0f;
    float max_val = 100.0f;
};

// Auto-zoomed Y-axis window for memory percentage history so tens-of-MB fluctuations remain
// legible on high-RAM systems (e.g. 32-128 GB) without distorting CPU's 0-100% scale.
SparklineRange ComputeMemoryZoomRange(std::span<const float> history, float total_gb);

// Compact honest range label for the zoomed memory track (e.g. "39.8–40.1%" or "35–52%").
std::wstring FormatMemoryZoomLabel(const SparklineRange& range);

// Real-time gradient-filled sparkline waveform (48 slots by default) with soft glow and live tip dot.
void DrawTahoeSparkline(ID2D1Factory* factory,
                        ID2D1DeviceContext* dc,
                        ID2D1SolidColorBrush* brush,
                        ID2D1StrokeStyle* round_stroke,
                        std::span<const float> history,
                        float max_val,
                        const D2D1_RECT_F& plot,
                        uint32_t accent_argb,
                        bool fill_area,
                        bool dark_ink_light,
                        size_t max_slots = 48,
                        float min_val = 0.0f);

// Translucent crystal analog clock dial: 60 ticks, optional 1-12 numerals, two-stage tapered
// shadowed hour/minute hands, vivid Apple Orange sweep second hand with counterweight & triple cap.
void DrawTahoeAnalogDial(ID2D1DeviceContext* dc,
                         ID2D1SolidColorBrush* brush,
                         ID2D1StrokeStyle* round_stroke,
                         float cx,
                         float cy,
                         float radius,
                         const SYSTEMTIME& time,
                         bool dark_ink_light,
                         bool night_shade,
                         bool show_ticks,
                         bool show_numbers,
                         const WidgetTextDrawFn& draw_numeral = {});

// Compact day-night cosine solar elevation curve with horizon line and glowing sun dot.
void DrawTahoeSolarArc(ID2D1Factory* factory,
                       ID2D1DeviceContext* dc,
                       ID2D1SolidColorBrush* brush,
                       ID2D1StrokeStyle* round_stroke,
                       const D2D1_RECT_F& rect,
                       int sunrise_min,
                       int sunset_min,
                       int now_min,
                       bool dark_ink_light);

// Live times and hour differences against local time for the world clock's cities (labels from
// model::kWorldCities; empty: 北京 东京 伦敦 纽约). Unknown labels are skipped.
std::vector<WorldClockCityEntry> QueryWorldClocks(std::span<const std::wstring> labels = {});

// Compact and full network throughput formatters ("12.4 MB/s" / "12.4M/s").
std::wstring FormatSpeedPerSec(uint64_t bytes_per_sec);
std::wstring FormatSpeedCompact(uint64_t bytes_per_sec);

// Simplified NOAA solar equation fallback for local sunrise/sunset minutes (0..1439).
void ComputeAstronomicalSunMinutes(double lat_deg,
                                   double lon_deg,
                                   int utc_offset_s,
                                   int& sunrise_min,
                                   int& sunset_min) noexcept;

} // namespace pulse::desktop::render

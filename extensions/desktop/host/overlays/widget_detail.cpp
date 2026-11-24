// The widget detail card: clicking 天气 / 日历 / 系统 opens a larger card beside the tile with what the tile
// has no room for, and a few actions — the "open the app" of a phone widget, kept on the desktop.
#include "host/overlays/desktop_overlays.h"
#include "host/overlays/lunar_calendar.h"
#include "host/render/vector_glyphs.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pulse::desktop::overlays {
namespace {
using Microsoft::WRL::ComPtr;
using render::DrawGlyph;

void StrokeCrispRoundedRect(ID2D1RenderTarget* dc, const D2D1_RECT_F& rc, float radius, ID2D1Brush* brush, float width) {
    const D2D1_RECT_F r = D2D1::RectF(std::round(rc.left) + width * 0.5f, std::round(rc.top) + width * 0.5f,
                                      std::round(rc.right) - width * 0.5f, std::round(rc.bottom) - width * 0.5f);
    if (r.right > r.left && r.bottom > r.top)
        dc->DrawRoundedRectangle(D2D1::RoundedRect(r, std::max(0.0f, radius - width * 0.5f), std::max(0.0f, radius - width * 0.5f)), brush, width);
}

constexpr const wchar_t* kWeekday[7] = {L"周日", L"周一", L"周二", L"周三", L"周四", L"周五", L"周六"};

const wchar_t* WeatherText(int code) {
    if (code == 0) return L"晴";
    if (code == 1) return L"晴间多云";
    if (code == 2) return L"多云转晴";
    if (code == 3) return L"阴";
    if (code == 45 || code == 48) return L"雾";
    if (code >= 51 && code <= 57) return L"毛毛雨";
    if (code >= 61 && code <= 63) return L"小雨";
    if (code >= 65 && code <= 67) return L"大雨";
    if (code >= 71 && code <= 77) return L"降雪";
    if (code >= 80 && code <= 82) return L"阵雨";
    if (code == 85 || code == 86) return L"阵雪";
    if (code >= 95) return L"雷阵雨";
    return L"多云";
}

std::wstring Bytes(uint64_t b) {
    wchar_t buf[32]{};
    const double gb = static_cast<double>(b) / (1024.0 * 1024.0 * 1024.0);
    if (gb >= 1024.0) std::swprintf(buf, 32, L"%.1f TB", gb / 1024.0);
    else if (gb >= 1.0) std::swprintf(buf, 32, gb >= 100.0 ? L"%.0f GB" : L"%.1f GB", gb);
    else std::swprintf(buf, 32, L"%.0f MB", static_cast<double>(b) / (1024.0 * 1024.0));
    return buf;
}

int DaysInMonth(int year, int month) {
    static constexpr int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return month == 2 && leap ? 29 : kDays[(month - 1) % 12];
}
// 0 = Sunday, for the first of the month (Zeller-free: Sakamoto's method).
int Weekday(int y, int m, int d) {
    static constexpr int t[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}
} // namespace

bool DesktopOverlayController::OpenWidgetDetail(const RECT& anchor_screen_rc, const std::string& tile_id,
                                                WidgetDetailData data) {
    Dismiss();
    active_kind_ = OverlayKind::WidgetDetail;
    detail_tile_id_ = tile_id;
    detail_ = std::move(data);
    detail_month_offset_ = 0;
    detail_sel_year_ = detail_sel_month_ = detail_sel_day_ = 0;
    int w = 368, h = 508;
    if (detail_.kind == model::TileKind::Calendar) { w = 360; h = 540; }
    if (detail_.kind == model::TileKind::System) { w = 336; h = 318; }
    if (detail_.kind == model::TileKind::Clock) { w = 336; h = 300; }
    if (detail_.kind == model::TileKind::DiskRing) {
        w = 336;
        h = 70 + 54 * static_cast<int>(std::clamp<size_t>(detail_.drives.size(), 1, 6)) + 56;
    }
    // Beside the tile, on the side with room (the board sits on the right, so usually its left).
    int x = anchor_screen_rc.left - w - 12;
    if (x < work_area_.left + 12) x = anchor_screen_rc.right + 12;
    const int y = std::clamp<int>(anchor_screen_rc.top, static_cast<int>(work_area_.top) + 12,
                                  std::max(static_cast<int>(work_area_.top) + 12, static_cast<int>(work_area_.bottom) - h - 12));
    RepositionAndShow(x, y, w, h, true);
    return true;
}

void DesktopOverlayController::UpdateWidgetDetail(WidgetDetailData data) {
    if (active_kind_ != OverlayKind::WidgetDetail || data.kind != detail_.kind) return;
    detail_ = std::move(data);
    Render();
}

bool DesktopOverlayController::HandleWidgetDetailAction(const std::string& act) {
    if (!act.starts_with("wd:")) return false;
    const std::string cmd = act.substr(3);
    if (cmd == "month-prev" || cmd == "month-next" || cmd == "month-today") {
        detail_month_offset_ = cmd == "month-today" ? 0 : detail_month_offset_ + (cmd == "month-next" ? 1 : -1);
        if (cmd == "month-today") detail_sel_year_ = detail_sel_month_ = detail_sel_day_ = 0;
        Render();
        return true;
    }
    if (cmd.starts_with("day:")) {
        // "day:YYYY-MM-DD": the chosen day, ringed, with its line under the grid.
        int y = 0, m = 0, d = 0;
        if (sscanf_s(cmd.c_str() + 4, "%d-%d-%d", &y, &m, &d) == 3) {
            detail_sel_year_ = y; detail_sel_month_ = m; detail_sel_day_ = d;
        }
        Render();
        return true;
    }
    ContextMenuSpec spec;
    spec.target_kind = ContextMenuTargetKind::LiveTile;
    spec.target_tile_id = detail_tile_id_;
    spec.tile_kind = detail_.kind;
    // Toggling a to-do and refreshing stay in the card (the host sends fresh data back); the rest open elsewhere.
    const bool stays = cmd.starts_with("todo-toggle:") || cmd == "weather-refresh" || cmd == "toggle-clock";
    if (!stays) Dismiss();
    if (callbacks_.on_menu_command) callbacks_.on_menu_command(cmd, spec);
    return true;
}

void DesktopOverlayController::DrawWidgetDetail(ID2D1DeviceContext* dc, float ox, float oy) {
    const float w = static_cast<float>(card_w_);
    const float h = static_cast<float>(card_h_);
    const uint32_t pri = dark_mode_ ? 0xF8FFFFFFu : 0xF8181E28u;
    const uint32_t sec = dark_mode_ ? 0x9EFFFFFFu : 0x9A303846u;
    const uint32_t line = dark_mode_ ? 0x1CFFFFFFu : 0x14000000u;
    const uint32_t track = dark_mode_ ? 0x1FFFFFFFu : 0x14000000u;
    ComPtr<ID2D1Factory> fac;
    dc->GetFactory(&fac);
    ComPtr<ID2D1StrokeStyle> round;
    if (fac)
        fac->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                           D2D1_LINE_JOIN_ROUND), nullptr, 0, &round);
    auto text = [&](const std::wstring& t, IDWriteTextFormat* f, float l, float tp, float r, float b, uint32_t c) {
        if (!t.empty() && r > l) detail_text_.DrawText(dwrite_.Get(),dc,t.c_str(), static_cast<UINT32>(t.size()), f, D2D1::RectF(ox + l, oy + tp, ox + r, oy + b), Brush(dc, c));
    };
    // Centred in its box, whatever the shared format's own alignment.
    auto centered = [&](const std::wstring& t, IDWriteTextFormat* f, float l, float tp, float r, float b, uint32_t c) {
        const DWRITE_TEXT_ALIGNMENT was = f->GetTextAlignment();
        f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        text(t, f, l, tp, r, b, c);
        f->SetTextAlignment(was);
    };
    auto hline = [&](float y) { dc->FillRectangle(D2D1::RectF(ox + 18.0f, oy + y, ox + w - 18.0f, oy + y + 1.0f), Brush(dc, line)); };
    // A pill button with a click region; `primary` takes the accent.
    auto button = [&](float l, float tp, float r, const std::wstring& label, const std::string& act, bool primary) {
        PulseButton(dc, ox, oy, D2D1::RectF(l, tp - 1.0f, r, tp + 31.0f), label, act, primary);
    };
    // Sun / cloud / rain / snow, drawn (no font glyphs).
    auto weather_icon = [&](float x, float y, float size, int code) {
        const float s = size / 24.0f;
        const bool sun = code <= 2, cloud = code >= 2, rain = (code >= 51 && code <= 67) || (code >= 80 && code <= 82) || code >= 95,
                   snow = (code >= 71 && code <= 77) || code == 85 || code == 86;
        if (sun) {
            const float cx = ox + x + (cloud ? 9.0f : 12.0f) * s, cy = oy + y + (cloud ? 9.0f : 12.0f) * s, r = (cloud ? 4.6f : 5.4f) * s;
            for (int i = 0; i < 8; ++i) {
                const float a = static_cast<float>(i) * 0.785398f;
                dc->DrawLine(D2D1::Point2F(cx + std::cos(a) * (r + 2.2f * s), cy + std::sin(a) * (r + 2.2f * s)),
                             D2D1::Point2F(cx + std::cos(a) * (r + 4.0f * s), cy + std::sin(a) * (r + 4.0f * s)),
                             Brush(dc, 0xFFFFC53Du), 1.4f * s, round.Get());
            }
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), Brush(dc, 0xFFFFC53Du));
        }
        if (cloud) {
            const uint32_t cc = dark_mode_ ? 0xFFE8EEF6u : 0xFFA7B4C6u;
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(ox + x + 10.0f * s, oy + y + 14.0f * s), 4.6f * s, 4.6f * s), Brush(dc, cc));
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(ox + x + 15.0f * s, oy + y + 12.5f * s), 5.6f * s, 5.6f * s), Brush(dc, cc));
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(ox + x + 5.0f * s, oy + y + 13.5f * s, ox + x + 21.0f * s, oy + y + 19.0f * s),
                                                       2.8f * s, 2.8f * s), Brush(dc, cc));
        }
        if (rain)
            for (int i = 0; i < 3; ++i) {
                const float dx = ox + x + (8.0f + 4.5f * static_cast<float>(i)) * s;
                dc->DrawLine(D2D1::Point2F(dx, oy + y + 20.5f * s), D2D1::Point2F(dx - 1.2f * s, oy + y + 23.5f * s), Brush(dc, 0xFF4CA6FFu), 1.5f * s, round.Get());
            }
        if (snow)
            for (int i = 0; i < 3; ++i)
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(ox + x + (8.0f + 4.5f * static_cast<float>(i)) * s, oy + y + 22.0f * s), 1.2f * s, 1.2f * s),
                                Brush(dc, dark_mode_ ? 0xFFFFFFFFu : 0xFF8FA3BBu));
    };

    switch (detail_.kind) {
    case model::TileKind::Weather: {
        const auto& wx = detail_.weather;
        const std::wstring city = wx.city.empty() ? detail_.weather_city : wx.city;
        const wchar_t pin[2] = {L'\xE707', 0};
        DrawGlyph(dc, pin, 1, fmt_fluent_12_.Get(), D2D1::RectF(ox + 18.0f, oy + 17.0f, ox + 32.0f, oy + 37.0f), Brush(dc, sec));
        text(city, fmt_title_.Get(), 36.0f, 14.0f, w - 140.0f, 40.0f, pri);
        if (!wx.updated.empty()) text(L"更新于 " + wx.updated, fmt_small_right_.Get(), w - 140.0f, 18.0f, w - 18.0f, 36.0f, sec);
        if (!wx.valid) {
            text(wx.status.empty() ? std::wstring(L"正在获取天气…") : wx.status, fmt_body_.Get(), 18.0f, 60.0f, w - 18.0f, 90.0f, sec);
            button(18.0f, h - 46.0f, 118.0f, L"刷新", "wd:weather-refresh", true);
            button(126.0f, h - 46.0f, 236.0f, L"更换城市…", "wd:weather-city-custom", false);
            break;
        }
        // Now: icon, temperature, condition and range; the extras on the right.
        weather_icon(14.0f, 44.0f, 60.0f, wx.code);
        text(std::to_wstring(wx.temp_c) + L"°", fmt_gallery_big_.Get(), 80.0f, 46.0f, 170.0f, 96.0f, pri);
        text(WeatherText(wx.code), fmt_body_semi_.Get(), 80.0f, 96.0f, 220.0f, 116.0f, pri);
        wchar_t range[40]{};
        std::swprintf(range, 40, L"最高 %d° · 最低 %d°", wx.hi_c, wx.lo_c);
        text(range, fmt_small_.Get(), 80.0f, 116.0f, 240.0f, 134.0f, sec);
        const std::pair<const wchar_t*, std::wstring> extras[] = {
            {L"体感", std::to_wstring(wx.feels_c) + L"°"}, {L"湿度", std::to_wstring(wx.humidity_pct) + L"%"},
            {L"风速", std::to_wstring(wx.wind_kmh) + L" km/h"}};
        float ey = 54.0f;
        for (const auto& [label, value] : extras) {
            text(label, fmt_small_.Get(), w - 128.0f, ey, w - 84.0f, ey + 18.0f, sec);
            text(value, fmt_small_right_.Get(), w - 100.0f, ey, w - 18.0f, ey + 18.0f, pri);
            ey += 24.0f;
        }
        hline(146.0f);
        // Hourly, with the temperature drawn as a curve between the icons and the numbers.
        text(L"逐小时", fmt_small_.Get(), 18.0f, 156.0f, 120.0f, 174.0f, sec);
        const size_t nh = std::min<size_t>(wx.hours.size(), 8);
        if (nh) {
            const float step = (w - 36.0f) / static_cast<float>(nh);
            int lo = 1000, hi = -1000;
            for (size_t i = 0; i < nh; ++i) { lo = std::min(lo, wx.hours[i].temp_c); hi = std::max(hi, wx.hours[i].temp_c); }
            const float span = static_cast<float>(std::max(1, hi - lo));
            ComPtr<ID2D1PathGeometry> curve;
            ComPtr<ID2D1GeometrySink> sink;
            const bool path = fac && SUCCEEDED(fac->CreatePathGeometry(&curve)) && SUCCEEDED(curve->Open(&sink));
            for (size_t i = 0; i < nh; ++i) {
                const auto& hr = wx.hours[i];
                const float cx = 18.0f + step * (static_cast<float>(i) + 0.5f);
                text(hr.label, fmt_small_center_.Get(), cx - 22.0f, 178.0f, cx + 22.0f, 194.0f, sec);
                weather_icon(cx - 11.0f, 198.0f, 22.0f, hr.code);
                const float py = 252.0f - 18.0f * static_cast<float>(hr.temp_c - lo) / span;
                if (path) {
                    if (i == 0) sink->BeginFigure(D2D1::Point2F(ox + cx, oy + py), D2D1_FIGURE_BEGIN_HOLLOW);
                    else sink->AddLine(D2D1::Point2F(ox + cx, oy + py));
                }
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(ox + cx, oy + py), 2.4f, 2.4f), Brush(dc, 0xFFF5B84Au));
                text(std::to_wstring(hr.temp_c) + L"°", fmt_small_center_.Get(), cx - 22.0f, 258.0f, cx + 22.0f, 276.0f, pri);
            }
            if (path) {
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                sink->Close();
                dc->DrawGeometry(curve.Get(), Brush(dc, 0xFFF5B84Au, 0.75f), 1.6f, round.Get());
            }
        }
        hline(286.0f);
        // The coming days: range bars against the week's lowest and highest.
        text(L"未来几天", fmt_small_.Get(), 18.0f, 294.0f, 140.0f, 312.0f, sec);
        int wlo = wx.lo_c, whi = wx.hi_c;
        for (const auto& d : wx.days) { wlo = std::min(wlo, d.lo_c); whi = std::max(whi, d.hi_c); }
        const float wspan = static_cast<float>(std::max(1, whi - wlo));
        float dy = 316.0f;
        for (size_t i = 0; i < wx.days.size() && dy + 18.0f <= h - 56.0f; ++i, dy += 22.0f) {
            const auto& d = wx.days[i];
            text(d.label, fmt_small_.Get(), 18.0f, dy, 62.0f, dy + 18.0f, pri);
            weather_icon(64.0f, dy - 1.0f, 20.0f, d.code);
            text(std::to_wstring(d.lo_c) + L"°", fmt_small_right_.Get(), 88.0f, dy, 124.0f, dy + 18.0f, sec);
            const float bx0 = ox + 132.0f, bx1 = ox + w - 64.0f;
            const float a = bx0 + (bx1 - bx0) * static_cast<float>(d.lo_c - wlo) / wspan;
            const float b = bx0 + (bx1 - bx0) * static_cast<float>(d.hi_c - wlo) / wspan;
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx0, oy + dy + 7.0f, bx1, oy + dy + 11.0f), 2.0f, 2.0f), Brush(dc, track));
            ComPtr<ID2D1GradientStopCollection> stops;
            ComPtr<ID2D1LinearGradientBrush> warm;
            const D2D1_GRADIENT_STOP gs[2] = {{0.0f, D2D1::ColorF(0x5BB4FF)}, {1.0f, D2D1::ColorF(0xF5A524)}};
            if (SUCCEEDED(dc->CreateGradientStopCollection(gs, 2, &stops)) &&
                SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(bx0, 0), D2D1::Point2F(bx1, 0)), stops.Get(), &warm)))
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(a, oy + dy + 7.0f, std::max(a + 6.0f, b), oy + dy + 11.0f), 2.0f, 2.0f), warm.Get());
            text(std::to_wstring(d.hi_c) + L"°", fmt_small_.Get(), w - 56.0f, dy, w - 18.0f, dy + 18.0f, pri);
        }
        button(18.0f, h - 46.0f, 118.0f, L"刷新", "wd:weather-refresh", false);
        button(126.0f, h - 46.0f, 236.0f, L"更换城市…", "wd:weather-city-custom", false);
        break;
    }
    case model::TileKind::Calendar: {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        int year = now.wYear, month = now.wMonth + detail_month_offset_;
        while (month < 1) { month += 12; --year; }
        while (month > 12) { month -= 12; ++year; }
        const bool this_month = detail_month_offset_ == 0;
        // The chosen day: today until another is picked.
        const int sel_y = detail_sel_day_ ? detail_sel_year_ : now.wYear;
        const int sel_m = detail_sel_day_ ? detail_sel_month_ : now.wMonth;
        const int sel_d = detail_sel_day_ ? detail_sel_day_ : now.wDay;
        const uint32_t red = dark_mode_ ? 0xFFFF6B6Bu : 0xFFD13438u;

        // Header: the month (a click goes back to today), 今天 when away, ‹ ›.
        wchar_t title[32]{};
        std::swprintf(title, 32, L"%d年%d月", year, month);
        const bool title_hot = hovered_action_id_ == "wd:month-today";
        text(title, fmt_title_.Get(), 20.0f, 14.0f, 200.0f, 40.0f, pri);
        click_regions_.push_back({"wd:month-today", RECT{16, 12, 150, 42}});
        if (!this_month || title_hot) {
            const D2D1_RECT_F chip = D2D1::RectF(ox + w - 146.0f, oy + 15.0f, ox + w - 94.0f, oy + 39.0f);
            dc->FillRoundedRectangle(D2D1::RoundedRect(chip, 12.0f, 12.0f), Brush(dc, accent_argb_, title_hot ? 0.28f : 0.18f));
            centered(L"回到今天", fmt_small_.Get(), w - 160.0f, 18.0f, w - 80.0f, 36.0f, pri);
            click_regions_.push_back({"wd:month-today", RECT{static_cast<LONG>(w - 146.0f), 15, static_cast<LONG>(w - 94.0f), 39}});
        }
        for (const bool next : {false, true}) {
            const float cx = w - (next ? 28.0f : 62.0f), cy = 27.0f;
            const std::string act = next ? "wd:month-next" : "wd:month-prev";
            if (hovered_action_id_ == act)
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(ox + cx - 15.0f, oy + cy - 14.0f, ox + cx + 15.0f, oy + cy + 14.0f), 5.0f, 5.0f),
                                         Brush(dc, dark_mode_ ? 0x14FFFFFFu : 0x0C000000u));
            const float dx = next ? 2.2f : -2.2f;
            dc->DrawLine(D2D1::Point2F(ox + cx - dx, oy + cy - 5.0f), D2D1::Point2F(ox + cx + dx, oy + cy), Brush(dc, pri), 1.5f, round.Get());
            dc->DrawLine(D2D1::Point2F(ox + cx + dx, oy + cy), D2D1::Point2F(ox + cx - dx, oy + cy + 5.0f), Brush(dc, pri), 1.5f, round.Get());
            click_regions_.push_back({act, RECT{static_cast<LONG>(cx - 15.0f), 13, static_cast<LONG>(cx + 15.0f), 41}});
        }

        // Monday-first grid; under each date its lunar day or festival, as Chinese calendars show.
        static constexpr const wchar_t* kHead[7] = {L"一", L"二", L"三", L"四", L"五", L"六", L"日"};
        const float gx = 14.0f, cw = (w - 28.0f) / 7.0f, ch = 42.0f, gy = 72.0f;
        for (int c = 0; c < 7; ++c) centered(kHead[c], fmt_small_.Get(), gx + cw * c, 50.0f, gx + cw * (c + 1), 66.0f, c >= 5 ? red : sec);
        const int first = (Weekday(year, month, 1) + 6) % 7;
        const int days = DaysInMonth(year, month);
        for (int d = 1; d <= days; ++d) {
            const int slot = first + d - 1;
            const float cl = gx + cw * static_cast<float>(slot % 7), ct = gy + ch * static_cast<float>(slot / 7);
            const bool today = this_month && d == now.wDay;
            const bool chosen = year == sel_y && month == sel_m && d == sel_d && !today;
            const bool weekend = slot % 7 >= 5;
            wchar_t act_buf[32]{};
            std::swprintf(act_buf, 32, L"wd:day:%d-%d-%d", year, month, d);
            std::string act;
            for (const wchar_t* q = act_buf; *q; ++q) act.push_back(static_cast<char>(*q));
            const bool hot = hovered_action_id_ == act;
            const D2D1_RECT_F plate = D2D1::RectF(ox + cl + 3.0f, oy + ct + 2.0f, ox + cl + cw - 3.0f, oy + ct + ch - 2.0f);
            if (today) dc->FillRoundedRectangle(D2D1::RoundedRect(plate, 9.0f, 9.0f), Brush(dc, red));
            else if (chosen) dc->FillRoundedRectangle(D2D1::RoundedRect(plate, 9.0f, 9.0f), Brush(dc, accent_argb_, 0.22f));
            else if (hot) dc->FillRoundedRectangle(D2D1::RoundedRect(plate, 9.0f, 9.0f), Brush(dc, dark_mode_ ? 0x14FFFFFFu : 0x0C000000u));
            if (chosen) StrokeCrispRoundedRect(dc, plate, 9.0f, Brush(dc, accent_argb_, 0.85f), 1.0f);
            const uint32_t num_ink = today ? 0xFFFFFFFFu : weekend ? red : pri;
            centered(std::to_wstring(d), today || chosen ? fmt_body_semi_.Get() : fmt_body_.Get(), cl, ct + 4.0f, cl + cw, ct + 24.0f, num_ink);
            bool festival = false;
            const std::wstring sub = LunarCellLabel(year, month, d, &festival);
            if (!sub.empty())
                centered(sub, fmt_small_.Get(), cl - 2.0f, ct + 23.0f, cl + cw + 2.0f, ct + 39.0f,
                         today ? 0xD9FFFFFFu : festival ? (dark_mode_ ? 0xFFFFB454u : 0xFFB45F06u) : sec);
            click_regions_.push_back({act, RECT{static_cast<LONG>(cl), static_cast<LONG>(ct), static_cast<LONG>(cl + cw), static_cast<LONG>(ct + ch)}});
        }

        // The chosen day in words: date, lunar date / festival, and how far away it is.
        const float iy = gy + ch * 6.0f + 6.0f;
        {
            static constexpr const wchar_t* kWd[7] = {L"周日", L"周一", L"周二", L"周三", L"周四", L"周五", L"周六"};
            const int wd = Weekday(sel_y, sel_m, sel_d);
            std::wstring line_text = std::to_wstring(sel_m) + L"月" + std::to_wstring(sel_d) + L"日 " + kWd[wd];
            const std::wstring lunar = LunarFullText(sel_y, sel_m, sel_d);
            if (!lunar.empty()) line_text += L"  " + lunar;
            const std::wstring fest = FestivalName(sel_y, sel_m, sel_d);
            if (!fest.empty()) line_text += L" · " + fest;
            text(line_text, fmt_body_.Get(), 20.0f, iy, w - 96.0f, iy + 22.0f, pri);
            // Whole days between today and the chosen day.
            auto serial = [](int y, int m, int d) {
                y -= m <= 2;
                const int era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
                const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
                return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy;
            };
            const int diff = serial(sel_y, sel_m, sel_d) - serial(now.wYear, now.wMonth, now.wDay);
            const std::wstring rel = diff == 0 ? L"今天" : diff == 1 ? L"明天" : diff == -1 ? L"昨天"
                                   : diff > 0 ? std::to_wstring(diff) + L" 天后" : std::to_wstring(-diff) + L" 天前";
            text(rel, fmt_small_right_.Get(), w - 100.0f, iy + 2.0f, w - 20.0f, iy + 20.0f, diff == 0 ? red : sec);
        }
        hline(iy + 30.0f);

        // To-dos, checkable here.
        size_t open = 0;
        for (const auto& td : detail_.todos) open += td.done ? 0u : 1u;
        wchar_t todo_head[32]{};
        std::swprintf(todo_head, 32, open ? L"待办 · 还有 %zu 项" : L"待办", open);
        text(todo_head, fmt_small_.Get(), 20.0f, iy + 38.0f, w - 20.0f, iy + 56.0f, sec);
        float ty = iy + 60.0f;
        for (size_t i = 0; i < detail_.todos.size() && ty + 24.0f <= h - 58.0f; ++i) {
            const auto& td = detail_.todos[i];
            const std::string act = "wd:todo-toggle:" + std::to_string(i);
            const bool hot = hovered_action_id_ == act;
            if (hot) dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(ox + 12.0f, oy + ty - 2.0f, ox + w - 12.0f, oy + ty + 24.0f), 6.0f, 6.0f),
                                              Brush(dc, dark_mode_ ? 0x14FFFFFFu : 0x0A000000u));
            const float cx = ox + 28.0f, cy = oy + ty + 11.0f;
            if (td.done) {
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 7.5f, 7.5f), Brush(dc, 0xFF30A46Cu));
                dc->DrawLine(D2D1::Point2F(cx - 3.2f, cy), D2D1::Point2F(cx - 0.8f, cy + 2.6f), Brush(dc, 0xFFFFFFFFu), 1.5f, round.Get());
                dc->DrawLine(D2D1::Point2F(cx - 0.8f, cy + 2.6f), D2D1::Point2F(cx + 3.4f, cy - 2.6f), Brush(dc, 0xFFFFFFFFu), 1.5f, round.Get());
            } else {
                dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 7.0f, 7.0f), Brush(dc, hot ? pri : sec), 1.4f);
            }
            text(td.text, fmt_body_.Get(), 44.0f, ty + 1.0f, w - 20.0f, ty + 21.0f, td.done ? sec : pri);
            click_regions_.push_back({act, RECT{12, static_cast<LONG>(ty - 2.0f), static_cast<LONG>(w - 12.0f), static_cast<LONG>(ty + 24.0f)}});
            ty += 26.0f;
        }
        if (detail_.todos.empty()) text(L"没有待办，轻松的一天", fmt_body_.Get(), 20.0f, ty, w - 20.0f, ty + 22.0f, sec);
        button(20.0f, h - 50.0f, 132.0f, L"添加待办", "wd:todo-add", true);
        button(140.0f, h - 50.0f, 266.0f, L"打开系统日历", "wd:open-calendar-app", false);
        break;
    }
    case model::TileKind::Clock: {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        wchar_t hm[8]{}, ss[4]{};
        std::swprintf(hm, 8, L"%02u:%02u", now.wHour, now.wMinute);
        std::swprintf(ss, 4, L"%02u", now.wSecond);
        text(hm, fmt_gallery_big_.Get(), 18.0f, 14.0f, 170.0f, 70.0f, pri);
        // Seconds sit right after the time, on its baseline side.
        float hm_w = 92.0f;
        ComPtr<IDWriteTextLayout> hm_layout;
        if (dwrite_ && SUCCEEDED(dwrite_->CreateTextLayout(hm, 5, fmt_gallery_big_.Get(), 200.0f, 60.0f, &hm_layout))) {
            DWRITE_TEXT_METRICS m{};
            if (SUCCEEDED(hm_layout->GetMetrics(&m))) hm_w = m.widthIncludingTrailingWhitespace;
        }
        text(ss, fmt_body_semi_.Get(), 18.0f + hm_w + 6.0f, 42.0f, 18.0f + hm_w + 40.0f, 64.0f, sec);
        // ISO week and how much of the year has gone, under the date.
        const int yday = [&] {
            int n = now.wDay;
            for (int m = 1; m < now.wMonth; ++m) n += DaysInMonth(now.wYear, m);
            return n;
        }();
        const int jan1 = (Weekday(now.wYear, 1, 1) + 6) % 7;  // 0 = Monday
        const int week = (yday + jan1 - 1) / 7 + (jan1 <= 3 ? 1 : 0);
        const int days_in_year = DaysInMonth(now.wYear, 2) == 29 ? 366 : 365;
        wchar_t date[64]{};
        std::swprintf(date, 64, L"%u月%u日 %ls · 第 %d 周", now.wMonth, now.wDay, kWeekday[now.wDayOfWeek % 7], std::max(1, week));
        text(date, fmt_small_.Get(), 18.0f, 74.0f, 210.0f, 92.0f, sec);
        const float year_ratio = static_cast<float>(yday) / static_cast<float>(days_in_year);
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(ox + 18.0f, oy + 100.0f, ox + 196.0f, oy + 104.0f), 2.0f, 2.0f), Brush(dc, track));
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(ox + 18.0f, oy + 100.0f, ox + 18.0f + 178.0f * year_ratio, oy + 104.0f), 2.0f, 2.0f),
                                 Brush(dc, accent_argb_));
        wchar_t yr[32]{};
        std::swprintf(yr, 32, L"今年已过 %d%%", static_cast<int>(year_ratio * 100.0f));
        text(yr, fmt_small_.Get(), 18.0f, 108.0f, 196.0f, 126.0f, sec);
        // The dial, with a sweeping second hand.
        const float cx = ox + w - 72.0f, cy = oy + 70.0f, r = 52.0f;
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy + 1.5f), r, r), Brush(dc, 0x40000000u));
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), Brush(dc, dark_mode_ ? 0xFF2A2F3Bu : 0xFFF7F8FAu));
        dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r - 0.5f, r - 0.5f), Brush(dc, line), 1.0f);
        for (int i = 0; i < 12; ++i) {
            const float a = static_cast<float>(i) * 0.5235988f;
            const float in = i % 3 == 0 ? r - 11.0f : r - 7.0f;
            dc->DrawLine(D2D1::Point2F(cx + std::sin(a) * in, cy - std::cos(a) * in), D2D1::Point2F(cx + std::sin(a) * (r - 4.0f), cy - std::cos(a) * (r - 4.0f)),
                         Brush(dc, pri, i % 3 == 0 ? 0.9f : 0.4f), i % 3 == 0 ? 2.0f : 1.0f, round.Get());
        }
        auto hand = [&](float turns, float len, float width, uint32_t color) {
            const float a = turns * 6.2831853f;
            dc->DrawLine(D2D1::Point2F(cx, cy), D2D1::Point2F(cx + std::sin(a) * len, cy - std::cos(a) * len), Brush(dc, color), width, round.Get());
        };
        const float sec_f = static_cast<float>(now.wSecond) + static_cast<float>(now.wMilliseconds) / 1000.0f;
        const float min_f = static_cast<float>(now.wMinute) + sec_f / 60.0f;
        hand((static_cast<float>(now.wHour % 12) + min_f / 60.0f) / 12.0f, r * 0.52f, 3.2f, pri);
        hand(min_f / 60.0f, r * 0.76f, 2.2f, pri);
        hand(sec_f / 60.0f, r * 0.82f, 1.1f, 0xFFFF8A2Bu);
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 3.0f, 3.0f), Brush(dc, 0xFFFF8A2Bu));
        hline(140.0f);
        // The weather city's clock, when it keeps another time (as a phone's world clock).
        FILETIME lf{}, uf{};
        SYSTEMTIME utc{};
        GetSystemTime(&utc);
        SystemTimeToFileTime(&now, &lf);
        SystemTimeToFileTime(&utc, &uf);
        const auto ticks = [](FILETIME f) { return (static_cast<int64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
        const int64_t local_off_s = (ticks(lf) - ticks(uf)) / 10000000;
        const auto& wx = detail_.weather;
        const std::wstring city = wx.city.empty() ? detail_.weather_city : wx.city;
        if (wx.has_utc_offset && !city.empty() && std::llabs(wx.utc_offset_s - local_off_s) >= 60) {
            FILETIME cf{};
            const int64_t city_ticks = ticks(uf) + static_cast<int64_t>(wx.utc_offset_s) * 10000000;
            cf.dwLowDateTime = static_cast<DWORD>(city_ticks & 0xFFFFFFFF);
            cf.dwHighDateTime = static_cast<DWORD>(city_ticks >> 32);
            SYSTEMTIME ct{};
            FileTimeToSystemTime(&cf, &ct);
            wchar_t cbuf[8]{};
            std::swprintf(cbuf, 8, L"%02u:%02u", ct.wHour, ct.wMinute);
            const int64_t diff_min = (wx.utc_offset_s - local_off_s) / 60;
            const wchar_t* day = ct.wDay == now.wDay ? L"今天" : (diff_min > 0 ? L"明天" : L"昨天");
            wchar_t rel[48]{};
            if (diff_min % 60 == 0) std::swprintf(rel, 48, L"%ls · %ls %lld 小时", day, diff_min > 0 ? L"快" : L"慢", std::llabs(diff_min / 60));
            else std::swprintf(rel, 48, L"%ls · %ls %lld 小时 %lld 分", day, diff_min > 0 ? L"快" : L"慢", std::llabs(diff_min / 60), std::llabs(diff_min % 60));
            text(city, fmt_body_semi_.Get(), 18.0f, 154.0f, 180.0f, 176.0f, pri);
            text(rel, fmt_small_.Get(), 18.0f, 178.0f, 220.0f, 196.0f, sec);
            text(cbuf, fmt_gallery_big_.Get(), w - 150.0f, 150.0f, w - 18.0f, 200.0f, pri);
        } else {
            text(L"天气城市和本机在同一时区", fmt_small_.Get(), 18.0f, 158.0f, w - 18.0f, 176.0f, sec);
            text(L"在天气卡片里更换城市后，这里会显示那里的时间", fmt_small_.Get(), 18.0f, 178.0f, w - 18.0f, 196.0f, sec);
        }
        button(18.0f, h - 46.0f, 148.0f, detail_.analog_clock ? L"换成数字表盘" : L"换成指针表盘", "wd:toggle-clock", false);
        button(156.0f, h - 46.0f, 286.0f, L"打开时钟应用", "wd:open-clock-app", false);
        break;
    }
    case model::TileKind::DiskRing: {
        text(L"磁盘", fmt_title_.Get(), 20.0f, 14.0f, 160.0f, 40.0f, pri);
        uint64_t total_free = 0;
        for (const auto& d : detail_.drives) total_free += d.free_bytes;
        text(L"共可用 " + Bytes(total_free), fmt_small_right_.Get(), w - 180.0f, 18.0f, w - 18.0f, 36.0f, sec);
        float ry = 52.0f;
        for (size_t i = 0; i < detail_.drives.size() && i < 6; ++i) {
            const auto& d = detail_.drives[i];
            const int pct = d.total_bytes ? static_cast<int>((d.total_bytes - d.free_bytes) * 100 / d.total_bytes) : 0;
            // Nearly full drives turn amber, then red, as a phone's storage page warns.
            const uint32_t color = pct >= 90 ? 0xFFE5484Du : pct >= 75 ? 0xFFF5A524u : 0xFF3D9BE9u;
            std::wstring root = d.root_path;
            if (root.size() > 2 && root.back() == L'\\') root.pop_back();
            text(root, fmt_body_semi_.Get(), 18.0f, ry, 100.0f, ry + 20.0f, pri);
            text(L"可用 " + Bytes(d.free_bytes) + L" · 共 " + Bytes(d.total_bytes), fmt_small_right_.Get(), 100.0f, ry + 1.0f, w - 18.0f, ry + 19.0f, sec);
            const float bx0 = ox + 18.0f, bx1 = ox + w - 18.0f;
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx0, oy + ry + 26.0f, bx1, oy + ry + 32.0f), 3.0f, 3.0f), Brush(dc, track));
            const float fill = bx0 + (bx1 - bx0) * static_cast<float>(std::clamp(pct, 0, 100)) / 100.0f;
            if (fill > bx0 + 1.0f)
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx0, oy + ry + 26.0f, std::max(bx0 + 6.0f, fill), oy + ry + 32.0f), 3.0f, 3.0f),
                                         Brush(dc, color));
            ry += 54.0f;
        }
        if (detail_.drives.empty()) text(L"没有读到磁盘信息", fmt_body_.Get(), 18.0f, 56.0f, w - 18.0f, 78.0f, sec);
        button(18.0f, h - 46.0f, 128.0f, L"打开此电脑", "wd:open-this-pc", false);
        button(136.0f, h - 46.0f, 246.0f, L"磁盘清理", "wd:disk-cleanup", false);
        break;
    }
    default: {  // System
        const auto& sy = detail_.system;
        text(L"系统", fmt_title_.Get(), 20.0f, 14.0f, 160.0f, 40.0f, pri);
        text(L"每 3 秒更新", fmt_small_right_.Get(), w - 140.0f, 18.0f, w - 18.0f, 36.0f, sec);
        const int mem_pct = sy.mem_total ? static_cast<int>(sy.mem_used * 100 / sy.mem_total) : 0;
        const int net_pct = static_cast<int>(std::min<uint64_t>(100, sy.net_down_bps / (1024 * 1024 / 4)));
        struct Row { const wchar_t* label; int pct; uint32_t color; std::wstring value; };
        const Row rows[] = {{L"CPU", sy.cpu_pct, 0xFF5A72E8u, std::to_wstring(sy.cpu_pct) + L"%"},
                            {L"内存", mem_pct, 0xFF30A46Cu, Bytes(sy.mem_used) + L" / " + Bytes(sy.mem_total)},
                            {L"C: 盘", sy.disk_used_pct, 0xFFF5A524u, L"可用 " + Bytes(sy.disk_free)},
                            {L"网络下载", net_pct, 0xFF8E6BE8u, Bytes(sy.net_down_bps) + L"/s"}};
        float ry = 52.0f;
        for (const auto& r : rows) {
            text(r.label, fmt_body_.Get(), 18.0f, ry, 140.0f, ry + 20.0f, pri);
            text(r.value, fmt_small_right_.Get(), 120.0f, ry + 1.0f, w - 18.0f, ry + 19.0f, sec);
            const float bx0 = ox + 18.0f, bx1 = ox + w - 18.0f;
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx0, oy + ry + 26.0f, bx1, oy + ry + 32.0f), 3.0f, 3.0f), Brush(dc, track));
            const float fill = bx0 + (bx1 - bx0) * static_cast<float>(std::clamp(r.pct, 0, 100)) / 100.0f;
            if (fill > bx0 + 1.0f)
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx0, oy + ry + 26.0f, std::max(bx0 + 6.0f, fill), oy + ry + 32.0f), 3.0f, 3.0f),
                                         Brush(dc, r.color));
            ry += 54.0f;
        }
        button(18.0f, h - 46.0f, 148.0f, L"打开任务管理器", "wd:open-taskmgr", false);
        break;
    }
    }
}

} // namespace pulse::desktop::overlays

// The widget detail card: clicking 天气 / 日历 / 系统 opens a larger card beside the tile with what the tile
// has no room for, and a few actions — the "open the app" of a phone widget, kept on the desktop.
#include "host/overlays/desktop_overlays.h"
#include "host/overlays/lunar_calendar.h"
#include "host/render/vector_glyphs.h"
#include "host/render/widget_painters.h"

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
    if (detail_.kind == model::TileKind::System) { w = 348; h = 412; }
    if (detail_.kind == model::TileKind::Clock) { w = 336; h = 300; }
    if (detail_.kind == model::TileKind::Countdown) { w = 348; h = 360; }
    if (detail_.kind == model::TileKind::Battery) { w = 348; h = 356; }
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
    // SF-Symbols-style vector weather icons shared with desktop tiles.
    auto weather_icon = [&](float x, float y, float size, int code, bool is_day = true) {
        render::DrawTahoeWeatherIcon(fac.Get(), dc, Brush(dc, 0xFFFFFFFFu), round.Get(),
                                     D2D1::RectF(ox + x, oy + y, ox + x + size, oy + y + size),
                                     code, is_day, dark_mode_);
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
        weather_icon(14.0f, 44.0f, 60.0f, wx.code, wx.is_day);
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
                weather_icon(cx - 11.0f, 198.0f, 22.0f, hr.code, hr.is_day);
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
        // The Tahoe crystal dial, with 60 ticks, 1-12 numerals, and counterweight sweep second hand.
        const float cx = ox + w - 72.0f, cy = oy + 70.0f, r = 52.0f;
        const render::WidgetTextDrawFn dial_num = [&](const std::wstring& s, const D2D1_RECT_F& nr,
                                                      uint32_t argb, float opacity, DWRITE_TEXT_ALIGNMENT) {
            const DWRITE_TEXT_ALIGNMENT was = fmt_small_.Get()->GetTextAlignment();
            fmt_small_.Get()->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            detail_text_.DrawText(dwrite_.Get(), dc, s.c_str(), static_cast<UINT32>(s.size()),
                                  fmt_small_.Get(), nr, Brush(dc, argb, opacity));
            fmt_small_.Get()->SetTextAlignment(was);
        };
        render::DrawTahoeAnalogDial(dc, Brush(dc, 0xFFFFFFFFu), round.Get(),
                                    cx, cy, r, now, dark_mode_, false, true, true, dial_num);
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
    case model::TileKind::Countdown: {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        const auto cd = live::ComputeCountdown(detail_.countdown_title, detail_.countdown_date, now);
        const uint32_t col_ind = dark_mode_ ? 0xFF7B9DFFu : 0xFF4755E8u;
        text(cd.title, fmt_title_.Get(), 20.0f, 14.0f, w - 108.0f, 40.0f, pri);
        text(cd.target_iso, fmt_small_right_.Get(), w - 110.0f, 18.0f, w - 18.0f, 36.0f, sec);

        const D2D1_RECT_F hero = D2D1::RectF(ox + 18.0f, oy + 48.0f, ox + w - 18.0f, oy + 124.0f);
        render::DrawTahoeGlassCard(dc, Brush(dc, 0xFFFFFFFFu), hero, 12.0f, dark_mode_);
        const std::wstring big_num = cd.is_today ? L"就是今天"
            : (std::to_wstring(std::abs(cd.days_delta)) + L" 天");
        const std::wstring sub_lbl = cd.is_today ? L"愿一切如期而至"
            : (cd.is_past ? L"已经过去" : L"距离目标日还有");
        text(sub_lbl, fmt_small_.Get(), 30.0f, 58.0f, w - 30.0f, 76.0f, sec);
        text(big_num, fmt_title_.Get(), 30.0f, 78.0f, w - 30.0f, 114.0f, col_ind);

        struct ProgRow { const wchar_t* label; float pct; uint32_t col; };
        const ProgRow rows[4] = {
            {L"今日进度", cd.day_pct,   dark_mode_ ? 0xFF54C8FFu : 0xFF0078D4u},
            {L"本周进度", cd.week_pct,  dark_mode_ ? 0xFF4EE08Au : 0xFF18984Au},
            {L"本月进度", cd.month_pct, dark_mode_ ? 0xFFFFB84Du : 0xFFD97706u},
            {L"今年进度", cd.year_pct,  dark_mode_ ? 0xFFB888FFu : 0xFF7C4DFFu},
        };
        float ry = 138.0f;
        for (const auto& pr : rows) {
            wchar_t pb[16]{};
            std::swprintf(pb, ARRAYSIZE(pb), L"%.1f%%", pr.pct);
            text(pr.label, fmt_small_.Get(), 20.0f, ry, 130.0f, ry + 18.0f, pri);
            text(pb, fmt_small_right_.Get(), 130.0f, ry, w - 20.0f, ry + 18.0f, sec);
            const float bx0 = ox + 20.0f, bx1 = ox + w - 20.0f;
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx0, oy + ry + 22.0f, bx1, oy + ry + 28.0f), 3.0f, 3.0f), Brush(dc, track));
            const float fill = bx0 + (bx1 - bx0) * std::clamp(pr.pct / 100.0f, 0.0f, 1.0f);
            if (fill > bx0 + 1.0f) {
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx0, oy + ry + 22.0f, std::max(bx0 + 6.0f, fill), oy + ry + 28.0f), 3.0f, 3.0f),
                                         Brush(dc, pr.col));
            }
            ry += 40.0f;
        }
        button(18.0f, h - 46.0f, 120.0f, L"改名称…", "wd:countdown-edit-title", false);
        button(128.0f, h - 46.0f, 232.0f, L"设日期…", "wd:countdown-edit-date", true);
        button(240.0f, h - 46.0f, w - 18.0f, L"+30天", "wd:countdown-plus30", false);
        break;
    }
    case model::TileKind::Battery: {
        const auto& bt = detail_.battery;
        const uint32_t col_ok = dark_mode_ ? 0xFF38DB70u : 0xFF18984Au;
        const uint32_t col_ac = dark_mode_ ? 0xFF4CC2FFu : 0xFF0078D4u;
        const uint32_t col_warn = dark_mode_ ? 0xFFFFB02Eu : 0xFFD97706u;
        const uint32_t col_low = dark_mode_ ? 0xFFFF5C5Cu : 0xFFD93838u;
        const uint32_t pc_col = !bt.has_battery ? col_ac : (bt.battery_pct < 20 ? col_low : (bt.battery_pct < 35 ? col_warn : col_ok));

        text(L"电池与设备电源", fmt_title_.Get(), 20.0f, 14.0f, w - 104.0f, 40.0f, pri);
        text(bt.power_plan.empty() ? L"平衡" : bt.power_plan, fmt_small_right_.Get(), w - 106.0f, 18.0f, w - 18.0f, 36.0f, sec);

        const D2D1_RECT_F pc_card = D2D1::RectF(ox + 18.0f, oy + 48.0f, ox + w - 18.0f, oy + 124.0f);
        render::DrawTahoeGlassCard(dc, Brush(dc, 0xFFFFFFFFu), pc_card, 12.0f, dark_mode_);
        render::DrawTahoeGlassRing(fac.Get(), dc, Brush(dc, 0xFFFFFFFFu), round.Get(),
                                   pc_card.left + 38.0f, (pc_card.top + pc_card.bottom) * 0.5f,
                                   23.0f, 4.8f, static_cast<float>(bt.has_battery ? bt.battery_pct : 100), pc_col, dark_mode_);
        const std::wstring pct_s = bt.has_battery ? (std::to_wstring(bt.battery_pct) + L"%") : L"AC";
        centered(pct_s, fmt_small_.Get(), 18.0f + 12.0f, 76.0f, 18.0f + 64.0f, 96.0f, pri);
        const std::wstring pc_title = bt.has_battery
            ? (bt.charging ? L"本机电池 · 充电中" : (bt.ac_online ? L"本机电池 · 已接电源" : L"本机电池 · 使用电池"))
            : L"台式机交流电源供电";
        const uint64_t mins = bt.uptime_sec / 60ull;
        wchar_t up_buf[64]{};
        if (mins >= 1440ull) std::swprintf(up_buf, ARRAYSIZE(up_buf), L"已运行 %llu 天 %llu 小时 · %ls模式", mins / 1440ull, (mins % 1440ull) / 60ull, bt.power_plan.c_str());
        else std::swprintf(up_buf, ARRAYSIZE(up_buf), L"已运行 %llu 小时 %llu 分钟 · %ls模式", mins / 60ull, mins % 60ull, bt.power_plan.c_str());
        text(pc_title, fmt_body_semi_.Get(), 96.0f, 62.0f, w - 28.0f, 84.0f, pri);
        text(up_buf, fmt_small_.Get(), 96.0f, 86.0f, w - 28.0f, 108.0f, sec);

        text(L"已连接的蓝牙设备电量", fmt_small_.Get(), 20.0f, 138.0f, w - 20.0f, 156.0f, sec);
        float ry = 162.0f;
        if (bt.bt_devices.empty()) {
            const D2D1_RECT_F empty_rc = D2D1::RectF(ox + 18.0f, oy + ry, ox + w - 18.0f, oy + ry + 52.0f);
            render::DrawTahoeGlassCard(dc, Brush(dc, 0xFFFFFFFFu), empty_rc, 10.0f, dark_mode_);
            text(L"未检测到上报电量的蓝牙外设", fmt_small_.Get(), 30.0f, ry + 16.0f, w - 30.0f, ry + 36.0f, sec);
        } else {
            for (size_t i = 0; i < bt.bt_devices.size() && i < 3; ++i) {
                const auto& d = bt.bt_devices[i];
                const uint32_t dc_col = d.percent < 20 ? col_low : (d.percent < 35 ? col_warn : col_ok);
                text(d.name, fmt_body_.Get(), 20.0f, ry, w - 76.0f, ry + 20.0f, pri);
                text(std::to_wstring(d.percent) + L"%", fmt_small_right_.Get(), w - 74.0f, ry + 1.0f, w - 20.0f, ry + 19.0f, dc_col);
                const float bx0 = ox + 20.0f, bx1 = ox + w - 20.0f;
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx0, oy + ry + 24.0f, bx1, oy + ry + 30.0f), 3.0f, 3.0f), Brush(dc, track));
                const float fill = bx0 + (bx1 - bx0) * std::clamp(static_cast<float>(d.percent) / 100.0f, 0.0f, 1.0f);
                if (fill > bx0 + 1.0f) {
                    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx0, oy + ry + 24.0f, std::max(bx0 + 6.0f, fill), oy + ry + 30.0f), 3.0f, 3.0f),
                                             Brush(dc, dc_col));
                }
                ry += 44.0f;
            }
        }
        button(18.0f, h - 46.0f, 176.0f, L"电源与电池设置", "wd:open-power-settings", false);
        break;
    }
    default: {  // System
        const auto& sy = detail_.system;
        const uint32_t col_cpu = dark_mode_ ? 0xFF33B8FFu : 0xFF0078D4u;
        const uint32_t col_mem = dark_mode_ ? 0xFF38DB70u : 0xFF18984Au;
        const uint32_t col_vram = dark_mode_ ? 0xFFBF6BFAu : 0xFF8839D8u;
        const uint32_t col_disk = dark_mode_ ? 0xFFFFA629u : 0xFFD97706u;
        const uint32_t col_net = dark_mode_ ? 0xFF5CD6FFu : 0xFF0284C7u;

        text(L"系统性能监控", fmt_title_.Get(), 20.0f, 14.0f, 200.0f, 40.0f, pri);
        text(sy.gpu_name.empty() ? L"每秒更新" : sy.gpu_name, fmt_small_right_.Get(), w - 164.0f, 18.0f, w - 18.0f, 36.0f, sec);
        const float mem_pct_f = sy.mem_total
            ? std::clamp(static_cast<float>(static_cast<double>(sy.mem_used) * 100.0 / static_cast<double>(sy.mem_total)), 0.0f, 100.0f)
            : sy.mem_pct_exact;
        const int mem_pct = static_cast<int>(std::lround(mem_pct_f));
        const int net_pct = static_cast<int>(std::min<uint64_t>(100, (sy.net_down_bps + sy.net_up_bps) / (1024 * 1024 / 4)));
        struct Row { std::wstring label; int pct; uint32_t color; std::wstring value; };
        std::vector<Row> rows;
        const std::wstring cpu_lbl = (sy.physical_cores > 0 && sy.logical_cores > sy.physical_cores)
            ? (L"CPU · " + std::to_wstring(sy.physical_cores) + L"核 " + std::to_wstring(sy.logical_cores) + L"线程")
            : (sy.logical_cores > 0 ? (L"CPU · " + std::to_wstring(sy.logical_cores) + L" 线程") : L"CPU");
        rows.push_back({cpu_lbl, sy.cpu_pct, col_cpu, std::to_wstring(sy.cpu_pct) + L"%"});
        wchar_t mp_buf[24]{};
        std::swprintf(mp_buf, ARRAYSIZE(mp_buf), L" (%.1f%%)", mem_pct_f);
        rows.push_back({L"内存", mem_pct, col_mem, Bytes(sy.mem_used) + L" / " + Bytes(sy.mem_total) + mp_buf});
        if (sy.has_vram) {
            wchar_t vb[48]{};
            std::swprintf(vb, ARRAYSIZE(vb), L"%.1f / %.1f GB (%ld%%)", sy.vram_used_gb, sy.vram_total_gb, std::lround(sy.vram_pct));
            rows.push_back({L"显存", static_cast<int>(std::lround(sy.vram_pct)), col_vram, vb});
        }
        rows.push_back({L"C: 盘", sy.disk_used_pct, col_disk, L"可用 " + Bytes(sy.disk_free)});
        rows.push_back({L"网络", net_pct, col_net,
                        L"↓ " + render::FormatSpeedPerSec(sy.net_down_bps) + L"  ↑ " + render::FormatSpeedPerSec(sy.net_up_bps)});

        const float step_y = sy.has_vram ? 44.0f : 50.0f;
        float ry = 50.0f;
        for (const auto& r : rows) {
            text(r.label, fmt_body_.Get(), 18.0f, ry, 172.0f, ry + 20.0f, pri);
            text(r.value, fmt_small_right_.Get(), 130.0f, ry + 1.0f, w - 18.0f, ry + 19.0f, sec);
            const float bx0 = ox + 18.0f, bx1 = ox + w - 18.0f;
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx0, oy + ry + 24.0f, bx1, oy + ry + 30.0f), 3.0f, 3.0f), Brush(dc, track));
            const float fill = bx0 + (bx1 - bx0) * static_cast<float>(std::clamp(r.pct, 0, 100)) / 100.0f;
            if (fill > bx0 + 1.0f)
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx0, oy + ry + 24.0f, std::max(bx0 + 6.0f, fill), oy + ry + 30.0f), 3.0f, 3.0f),
                                         Brush(dc, r.color));
            ry += step_y;
        }

        // Live 48-second split CPU (0-100%) and RAM (auto-zoomed) sparkline cards above the footer button
        const float box_top = ry + 2.0f;
        const float box_bot = h - 58.0f;
        if (box_bot - box_top >= 58.0f) {
            const float gap = 8.0f;
            const float card_w = (w - 36.0f - gap) * 0.5f;
            const D2D1_RECT_F cbox = D2D1::RectF(ox + 18.0f, oy + box_top, ox + 18.0f + card_w, oy + box_bot);
            const D2D1_RECT_F mbox = D2D1::RectF(cbox.right + gap, oy + box_top, ox + w - 18.0f, oy + box_bot);
            render::DrawTahoeGlassCard(dc, Brush(dc, 0xFFFFFFFFu), cbox, 12.0f, dark_mode_);
            render::DrawTahoeGlassCard(dc, Brush(dc, 0xFFFFFFFFu), mbox, 12.0f, dark_mode_);

            const float c_one = static_cast<float>(sy.cpu_pct);
            const float m_one = mem_pct_f;
            const std::span<const float> c_sp = sy.cpu_history.empty() ? std::span<const float>(&c_one, 1) : std::span<const float>(sy.cpu_history);
            const std::span<const float> m_sp = sy.mem_history.empty() ? std::span<const float>(&m_one, 1) : std::span<const float>(sy.mem_history);
            const float total_gb = static_cast<float>(static_cast<double>(sy.mem_total) / (1024.0 * 1024.0 * 1024.0));
            const render::SparklineRange mr = render::ComputeMemoryZoomRange(m_sp, total_gb);
            const std::wstring mr_lbl = render::FormatMemoryZoomLabel(mr);

            wchar_t cl[24]{}, ml[28]{};
            std::swprintf(cl, ARRAYSIZE(cl), L"● CPU %d%%", sy.cpu_pct);
            std::swprintf(ml, ARRAYSIZE(ml), L"● 内存 %.1f%%", mem_pct_f);
            const float c_x0 = 18.0f, c_x1 = 18.0f + card_w;
            const float m_x0 = c_x1 + gap, m_x1 = w - 18.0f;
            text(cl, fmt_small_.Get(), c_x0 + 8.0f, box_top + 6.0f, c_x0 + card_w * 0.62f, box_top + 22.0f, col_cpu);
            text(L"0–100%", fmt_small_right_.Get(), c_x0 + card_w * 0.52f, box_top + 6.0f, c_x1 - 8.0f, box_top + 22.0f, sec);
            text(ml, fmt_small_.Get(), m_x0 + 8.0f, box_top + 6.0f, m_x0 + card_w * 0.60f, box_top + 22.0f, col_mem);
            text(mr_lbl, fmt_small_right_.Get(), m_x0 + card_w * 0.50f, box_top + 6.0f, m_x1 - 8.0f, box_top + 22.0f, sec);

            const D2D1_RECT_F cplot = D2D1::RectF(cbox.left + 8.0f, cbox.top + 25.0f, cbox.right - 8.0f, cbox.bottom - 8.0f);
            const D2D1_RECT_F mplot = D2D1::RectF(mbox.left + 8.0f, mbox.top + 25.0f, mbox.right - 8.0f, mbox.bottom - 8.0f);
            dc->DrawLine(D2D1::Point2F(cplot.left, (cplot.top + cplot.bottom) * 0.5f),
                         D2D1::Point2F(cplot.right, (cplot.top + cplot.bottom) * 0.5f), Brush(dc, line), 0.7f);
            dc->DrawLine(D2D1::Point2F(mplot.left, (mplot.top + mplot.bottom) * 0.5f),
                         D2D1::Point2F(mplot.right, (mplot.top + mplot.bottom) * 0.5f), Brush(dc, line), 0.7f);
            render::DrawTahoeSparkline(fac.Get(), dc, Brush(dc, 0xFFFFFFFFu), round.Get(), c_sp, 100.0f, cplot, col_cpu, true, dark_mode_);
            render::DrawTahoeSparkline(fac.Get(), dc, Brush(dc, 0xFFFFFFFFu), round.Get(), m_sp, mr.max_val, mplot, col_mem, true, dark_mode_, 48, mr.min_val);
        }

        button(18.0f, h - 46.0f, 148.0f, L"打开任务管理器", "wd:open-taskmgr", false);
        break;
    }
    }
}

} // namespace pulse::desktop::overlays

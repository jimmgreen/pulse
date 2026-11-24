#include "host/render/desktop_renderer.h"
#include "host/render/vector_glyphs.h"
#include "host/render/desktop_appearance.h"
#include "../../../../src/common/os_version.h"
#include "../../../../src/common/localization.h"
#include "../../../../src/ui/ui_compositor.h"

#include <shellapi.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <functional>
#include <initializer_list>
#include <cmath>
#include <cstdio>
#include <cwctype>

namespace pulse::desktop::render {
namespace {

using Microsoft::WRL::ComPtr;

constexpr float kPi = 3.14159265358979323846f;

D2D1_COLOR_F ColorFromArgb(uint32_t argb, float opacity_mul = 1.0f) noexcept {
    const float a = (static_cast<float>((argb >> 24) & 0xFFu) / 255.0f) * opacity_mul;
    const float r = static_cast<float>((argb >> 16) & 0xFFu) / 255.0f;
    const float g = static_cast<float>((argb >> 8) & 0xFFu) / 255.0f;
    const float b = static_cast<float>(argb & 0xFFu) / 255.0f;
    return D2D1::ColorF(r, g, b, std::clamp(a, 0.0f, 1.0f));
}

uint32_t MixArgb(uint32_t c0, uint32_t c1, float t) noexcept {
    t = std::clamp(t, 0.0f, 1.0f);
    const auto lerp_ch = [t](uint32_t a, uint32_t b) -> uint32_t {
        return static_cast<uint32_t>(std::clamp(
            static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * t + 0.5f, 0.0f, 255.0f));
    };
    const uint32_t r = lerp_ch((c0 >> 16) & 0xFFu, (c1 >> 16) & 0xFFu);
    const uint32_t g = lerp_ch((c0 >> 8) & 0xFFu, (c1 >> 8) & 0xFFu);
    const uint32_t b = lerp_ch(c0 & 0xFFu, c1 & 0xFFu);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

D2D1_RECT_F ToRectF(const model::RectI& r) noexcept {
    return D2D1::RectF(
        static_cast<float>(r.x),
        static_cast<float>(r.y),
        static_cast<float>(r.right()),
        static_cast<float>(r.bottom()));
}

bool ContainsCaseInsensitive(std::wstring_view haystack, std::wstring_view needle) {
    if (needle.empty()) return false;
    if (haystack.size() < needle.size()) return false;
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool ok = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            if (std::towlower(haystack[i + j]) != std::towlower(needle[j])) {
                ok = false;
                break;
            }
        }
        if (ok) return true;
    }
    return false;
}

std::wstring LowerExtFromNameOrExt(std::wstring_view ext, std::wstring_view fallback_name = {}) {
    std::wstring out;
    if (!ext.empty()) {
        if (ext.front() == L'.') {
            out.assign(ext);
        } else {
            const size_t dot = ext.find_last_of(L'.');
            if (dot != std::wstring_view::npos) {
                out.assign(ext.substr(dot));
            } else {
                out.push_back(L'.');
                out.append(ext);
            }
        }
    } else if (!fallback_name.empty()) {
        const size_t dot = fallback_name.find_last_of(L'.');
        if (dot != std::wstring_view::npos) {
            out.assign(fallback_name.substr(dot));
        }
    }
    for (wchar_t& c : out) c = static_cast<wchar_t>(std::towlower(c));
    return out;
}

std::wstring FormatBytesCompact(uint64_t bytes) {
    wchar_t buf[64]{};
    if (bytes < 1024ull) {
        std::swprintf(buf, ARRAYSIZE(buf), L"%llu B", static_cast<unsigned long long>(bytes));
    } else if (bytes < 1024ull * 1024ull) {
        std::swprintf(buf, ARRAYSIZE(buf), L"%.0f KB", static_cast<double>(bytes) / 1024.0);
    } else if (bytes < 1024ull * 1024ull * 1024ull) {
        const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
        if (mb >= 100.0) {
            std::swprintf(buf, ARRAYSIZE(buf), L"%.0f MB", mb);
        } else {
            std::swprintf(buf, ARRAYSIZE(buf), L"%.1f MB", mb);
        }
    } else {
        const double gb = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
        if (gb >= 100.0) {
            std::swprintf(buf, ARRAYSIZE(buf), L"%.0f GB", gb);
        } else {
            std::swprintf(buf, ARRAYSIZE(buf), L"%.1f GB", gb);
        }
    }
    return buf;
}

std::wstring FormatRelativeTimeOrSize(int64_t modified_unix_ms, uint64_t size_bytes) {
    if (modified_unix_ms > 0) {
        FILETIME ft{};
        GetSystemTimeAsFileTime(&ft);
        ULARGE_INTEGER uli{};
        uli.LowPart = ft.dwLowDateTime;
        uli.HighPart = ft.dwHighDateTime;
        const int64_t now_ms = static_cast<int64_t>(uli.QuadPart / 10000ull) - 11644473600000ll;
        const int64_t diff_min = std::max<int64_t>(0, (now_ms - modified_unix_ms) / 60000ll);
        wchar_t buf[48]{};
        if (diff_min < 2) return L"刚刚";
        if (diff_min < 60) {
            std::swprintf(buf, ARRAYSIZE(buf), L"%lld 分钟前", static_cast<long long>(diff_min));
            return buf;
        }
        const int64_t diff_hr = diff_min / 60;
        if (diff_hr < 24) {
            std::swprintf(buf, ARRAYSIZE(buf), L"%lld 小时前", static_cast<long long>(diff_hr));
            return buf;
        }
        const int64_t diff_day = diff_hr / 24;
        if (diff_day == 1) return L"昨天";
        if (diff_day < 14) {
            std::swprintf(buf, ARRAYSIZE(buf), L"%lld 天前", static_cast<long long>(diff_day));
            return buf;
        }
    }
    if (size_bytes > 0) return FormatBytesCompact(size_bytes);
    return L"";
}

bool IsInstallerGroup(const model::DesktopGroup& group) noexcept {
    return group.id == "installers" || group.id == "inst" || group.title == L"安装包";
}

std::wstring MappedFolderLeafName(std::wstring_view path) {
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) {
        path.remove_suffix(1);
    }
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring_view::npos || slash + 1 >= path.size()) {
        return std::wstring(path);
    }
    return std::wstring(path.substr(slash + 1));
}

struct GroupFooterLayout {
    bool active = false;
    float fy = 0.0f;
    float bottom = 0.0f;
    D2D1_RECT_F prev_rc{};
    D2D1_RECT_F dots_rc{};
    D2D1_RECT_F next_rc{};
    D2D1_RECT_F view_all_rc{};
    std::wstring view_all_text;
    int start_page = 0;
    int shown_dots = 0;
    int cur_page = 0;
    int page_count = 1;
    std::array<D2D1_RECT_F, 7> dot_rects{};
    bool show_fraction = false;
    D2D1_RECT_F fraction_rc{};
    std::wstring fraction_text;
};

GroupFooterLayout ComputeGroupFooterLayout(const model::RectI& frame,
                                            const model::GroupComputedView& view) noexcept {
    GroupFooterLayout out{};
    const int pages = std::max(view.page_count, view.overflow_count > 0 ? 2 : 1);
    if (pages <= 1 || frame.empty()) return out;

    out.active = true;
    out.page_count = pages;
    out.cur_page = std::clamp(view.page_index, 0, pages - 1);

    const float left = static_cast<float>(frame.x);
    const float right = static_cast<float>(frame.right());
    out.bottom = static_cast<float>(frame.bottom());
    out.fy = out.bottom - static_cast<float>(model::kGroupFooterHeight);
    const float cy = out.fy + static_cast<float>(model::kGroupFooterHeight) * 0.5f;

    out.prev_rc = D2D1::RectF(left + 8.0f, out.fy + 3.0f, left + 28.0f, out.bottom - 3.0f);

    constexpr int kMaxDots = 7;
    out.shown_dots = std::min(pages, kMaxDots);
    out.start_page = (pages > kMaxDots) ? std::clamp(out.cur_page - 3, 0, pages - kMaxDots) : 0;

    float dx = left + 32.0f;
    const float dots_x0 = dx;
    for (int k = 0; k < out.shown_dots; ++k) {
        const int p = out.start_page + k;
        const float dw = (p == out.cur_page) ? 16.0f : 6.0f;
        out.dot_rects[static_cast<size_t>(k)] = D2D1::RectF(dx, cy - 3.0f, dx + dw, cy + 3.0f);
        dx += dw + 4.0f;
    }
    const float dots_x1 = std::max(dots_x0, dx - 4.0f);
    out.dots_rc = D2D1::RectF(dots_x0 - 2.0f, out.fy + 2.0f, dots_x1 + 2.0f, out.bottom - 2.0f);

    float next_x0 = dots_x1 + 5.0f;
    if (pages > kMaxDots) {
        out.show_fraction = true;
        wchar_t frac[32]{};
        std::swprintf(frac, ARRAYSIZE(frac), L"%d/%d", out.cur_page + 1, pages);
        out.fraction_text = frac;
        const float fw = std::max(28.0f, static_cast<float>(out.fraction_text.size()) * 6.5f + 4.0f);
        out.fraction_rc = D2D1::RectF(dots_x1 + 6.0f, out.fy + 2.0f, dots_x1 + 6.0f + fw, out.bottom - 2.0f);
        next_x0 = out.fraction_rc.right + 2.0f;
    }

    out.next_rc = D2D1::RectF(next_x0, out.fy + 3.0f, next_x0 + 20.0f, out.bottom - 3.0f);

    const float va_right = right - 14.0f;
    const float avail = va_right - (out.next_rc.right + 6.0f);
    float va_w = 22.0f;
    if (avail >= 104.0f) {
        out.view_all_text = L"在 Pulse 中查看";
        va_w = 102.0f;
    } else if (avail >= 68.0f) {
        out.view_all_text = L"查看全部";
        va_w = 66.0f;
    } else if (avail >= 42.0f) {
        out.view_all_text = L"全部";
        va_w = 40.0f;
    }
    out.view_all_rc = D2D1::RectF(std::max(out.next_rc.right + 4.0f, va_right - va_w),
                                  out.fy + 2.0f, va_right, out.bottom - 2.0f);
    return out;
}

bool IsPersonalTileKind(model::TileKind kind) noexcept {
    switch (kind) {
    case model::TileKind::Downloads:
    case model::TileKind::Recent:
    case model::TileKind::PinnedFolder:
    case model::TileKind::StagingTray:
    case model::TileKind::StickyNote:
    case model::TileKind::PinnedItem:
    case model::TileKind::Todo:
        return true;
    default:
        return false;
    }
}

bool IsFocusKeepTileKind(model::TileKind kind) noexcept {
    switch (kind) {
    case model::TileKind::Clock:
    case model::TileKind::StickyNote:
    case model::TileKind::Recent:
    case model::TileKind::StagingTray:
    case model::TileKind::Calendar:
    case model::TileKind::Todo:
    case model::TileKind::Apps:
        return true;
    default:
        return false;
    }
}

uint32_t TileHueArgb(model::TileKind kind, bool dark_mode) noexcept {
    switch (kind) {
    case model::TileKind::Downloads:     return dark_mode ? 0xFF5AAAE6u : 0xFF0A64C8u;
    case model::TileKind::Recent:        return dark_mode ? 0xFFA193FFu : 0xFF5B4BD6u;
    case model::TileKind::PinnedFolder:  return dark_mode ? 0xFF5FD08Au : 0xFF1E7F45u;
    case model::TileKind::StagingTray:   return dark_mode ? 0xFFC59BFFu : 0xFF7B4BC4u;
    case model::TileKind::RecycleBin:    return dark_mode ? 0xFF9AA6B5u : 0xFF5B6675u;
    case model::TileKind::ThisPC:        return dark_mode ? 0xFF4FC7D1u : 0xFF0F7C86u;
    case model::TileKind::DiskRing:      return dark_mode ? 0xFFF0B43Cu : 0xFFB26A00u;
    case model::TileKind::PulseLauncher: return dark_mode ? 0xFF5AAAE6u : 0xFF0A64C8u;
    case model::TileKind::Clock:         return dark_mode ? 0xFFFF9F5Au : 0xFFD2601Eu;
    case model::TileKind::StickyNote:    return dark_mode ? 0xFFF5C542u : 0xFF9A7300u;
    case model::TileKind::PinnedItem:    return dark_mode ? 0xFF7FB2FFu : 0xFF2459B8u;
    case model::TileKind::Apps:          return dark_mode ? 0xFF8E9BFFu : 0xFF4453C8u;
    case model::TileKind::Weather:       return dark_mode ? 0xFF5AB8F0u : 0xFF0B6FB8u;
    case model::TileKind::Calendar:      return dark_mode ? 0xFFFF6B6Bu : 0xFFC4312Cu;
    case model::TileKind::Todo:          return dark_mode ? 0xFF4CC38Au : 0xFF1E7F45u;
    case model::TileKind::System:        return dark_mode ? 0xFF8FA8FFu : 0xFF3A55C4u;
    case model::TileKind::Photos:
    default:                             return 0xFFFFFFFFu;
    }
}

wchar_t TileFluentGlyph(const model::DesktopTile& tile) noexcept {
    switch (tile.kind) {
    case model::TileKind::Photos:       return L'\xEB9F';
    case model::TileKind::Downloads:    return L'\xE896';
    case model::TileKind::Recent:       return L'\xE81C';
    case model::TileKind::PinnedFolder:
        if (ContainsCaseInsensitive(tile.title, L"源") ||
            ContainsCaseInsensitive(tile.title, L"code") ||
            ContainsCaseInsensitive(tile.target_path, L"src") ||
            ContainsCaseInsensitive(tile.target_path, L"源")) {
            return L'\xE943';
        }
        return L'\xE8B7';
    case model::TileKind::RecycleBin:   return L'\xE74D';
    case model::TileKind::ThisPC:       return L'\xE7F8';
    case model::TileKind::DiskRing:     return L'\xEDA2';
    case model::TileKind::Clock:        return L'\xE823';
    case model::TileKind::StickyNote:   return L'\xE70B';
    case model::TileKind::StagingTray:  return L'\xE7B8';
    case model::TileKind::PinnedItem:   return L'\xE8A5';
    case model::TileKind::Apps:         return L'\xE71D';
    case model::TileKind::Weather:      return L'\xE706';
    case model::TileKind::Calendar:     return L'\xE787';
    case model::TileKind::Todo:         return L'\xE73A';
    case model::TileKind::System:       return L'\xE9D9';
    case model::TileKind::PulseLauncher:
    default:                            return L'\xE945';
    }
}

void DrawCircularArc(ID2D1Factory* factory, ID2D1RenderTarget* rt, ID2D1Brush* brush,
                     float cx, float cy, float radius, float start_deg, float sweep_deg,
                     float stroke_width, ID2D1StrokeStyle* stroke_style = nullptr) {
    if (!factory || !rt || !brush || sweep_deg <= 0.5f || radius <= 1.0f) return;
    if (sweep_deg >= 359.5f) {
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), brush, stroke_width, stroke_style);
        return;
    }
    const float r0 = start_deg * (kPi / 180.0f);
    const float r1 = (start_deg + sweep_deg) * (kPi / 180.0f);
    const D2D1_POINT_2F p0 = D2D1::Point2F(cx + std::cos(r0) * radius, cy + std::sin(r0) * radius);
    const D2D1_POINT_2F p1 = D2D1::Point2F(cx + std::cos(r1) * radius, cy + std::sin(r1) * radius);

    ComPtr<ID2D1PathGeometry> path;
    if (FAILED(factory->CreatePathGeometry(&path)) || !path) return;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(path->Open(&sink)) || !sink) return;
    sink->BeginFigure(p0, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(
        p1,
        D2D1::SizeF(radius, radius),
        0.0f,
        D2D1_SWEEP_DIRECTION_CLOCKWISE,
        sweep_deg > 180.0f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if (SUCCEEDED(sink->Close())) {
        rt->DrawGeometry(path.Get(), brush, stroke_width, stroke_style);
    }
}

// The Pulse logo (assets/pulse-logo.svg), drawn as vector geometry: a deep-indigo folder back, a
// lavender front page with a soft sheen, the folded corner and its rim, all clipped to the folder outline.
namespace {

ComPtr<ID2D1PathGeometry> BuildLogoPath(ID2D1Factory* factory, const std::function<void(ID2D1GeometrySink*)>& build) {
    ComPtr<ID2D1PathGeometry> geo;
    if (FAILED(factory->CreatePathGeometry(&geo)) || !geo) return {};
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geo->Open(&sink)) || !sink) return {};
    build(sink.Get());
    if (FAILED(sink->Close())) return {};
    return geo;
}

D2D1_ARC_SEGMENT LogoArc(float x, float y, float r) {
    return D2D1::ArcSegment(D2D1::Point2F(x, y), D2D1::SizeF(r, r), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL);
}

D2D1_BEZIER_SEGMENT LogoCurve(float x1, float y1, float x2, float y2, float x3, float y3) {
    return D2D1::BezierSegment(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), D2D1::Point2F(x3, y3));
}

ComPtr<ID2D1LinearGradientBrush> LogoGradient(ID2D1DeviceContext* dc, D2D1_POINT_2F a, D2D1_POINT_2F b,
                                              std::initializer_list<D2D1_GRADIENT_STOP> stops) {
    std::vector<D2D1_GRADIENT_STOP> v(stops);
    ComPtr<ID2D1GradientStopCollection> coll;
    if (FAILED(dc->CreateGradientStopCollection(v.data(), static_cast<UINT32>(v.size()), &coll))) return {};
    ComPtr<ID2D1LinearGradientBrush> brush;
    dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(a, b), coll.Get(), &brush);
    return brush;
}

D2D1_COLOR_F LogoRgb(uint32_t rgb, float a = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}

} // namespace

void DrawPulseLogo(ID2D1DeviceContext* dc, const D2D1_RECT_F& dst) {
    if (!dc) return;
    ComPtr<ID2D1Factory> factory;
    dc->GetFactory(&factory);
    if (!factory) return;

    auto outline = BuildLogoPath(factory.Get(), [](ID2D1GeometrySink* s) {
        s->BeginFigure(D2D1::Point2F(16, 60), D2D1_FIGURE_BEGIN_FILLED);
        s->AddArc(LogoArc(40, 36, 24));
        s->AddLine(D2D1::Point2F(92, 36));
        s->AddBezier(LogoCurve(108, 36, 120, 44, 124, 60));
        s->AddLine(D2D1::Point2F(216, 60));
        s->AddArc(LogoArc(240, 84, 24));
        s->AddLine(D2D1::Point2F(240, 200));
        s->AddArc(LogoArc(212, 228, 28));
        s->AddLine(D2D1::Point2F(44, 228));
        s->AddArc(LogoArc(16, 200, 28));
        s->EndFigure(D2D1_FIGURE_END_CLOSED);
    });
    auto fold = BuildLogoPath(factory.Get(), [](ID2D1GeometrySink* s) {
        s->BeginFigure(D2D1::Point2F(96, 82), D2D1_FIGURE_BEGIN_FILLED);
        s->AddLine(D2D1::Point2F(128, 72));
        s->AddBezier(LogoCurve(150, 68, 170, 80, 190, 106));
        s->AddLine(D2D1::Point2F(228, 158));
        s->AddBezier(LogoCurve(236, 169, 240, 178, 240, 188));
        s->AddLine(D2D1::Point2F(260, 188));
        s->AddLine(D2D1::Point2F(260, 256));
        s->AddLine(D2D1::Point2F(120, 256));
        s->EndFigure(D2D1_FIGURE_END_CLOSED);
    });
    auto rim = BuildLogoPath(factory.Get(), [](ID2D1GeometrySink* s) {
        s->BeginFigure(D2D1::Point2F(128, 72), D2D1_FIGURE_BEGIN_HOLLOW);
        s->AddBezier(LogoCurve(150, 68, 170, 80, 190, 106));
        s->AddLine(D2D1::Point2F(228, 158));
        s->AddBezier(LogoCurve(236, 169, 240, 178, 240, 188));
        s->AddLine(D2D1::Point2F(240, 200));
        s->AddArc(LogoArc(212, 228, 28));
        s->EndFigure(D2D1_FIGURE_END_OPEN);
    });
    auto page = BuildLogoPath(factory.Get(), [](ID2D1GeometrySink* s) {
        s->BeginFigure(D2D1::Point2F(0, 0), D2D1_FIGURE_BEGIN_FILLED);
        s->AddLine(D2D1::Point2F(92, 0));
        s->AddLine(D2D1::Point2F(92, 30));
        s->AddBezier(LogoCurve(110, 30, 120, 44, 124, 60));
        s->AddBezier(LogoCurve(126, 68, 127, 70, 129, 80));
        s->AddLine(D2D1::Point2F(147, 169));
        s->AddBezier(LogoCurve(150, 187, 154, 207, 160, 228));
        s->AddLine(D2D1::Point2F(170, 256));
        s->AddLine(D2D1::Point2F(0, 256));
        s->EndFigure(D2D1_FIGURE_END_CLOSED);
    });
    if (!outline || !fold || !rim || !page) return;

    // The SVG's viewBox is (10, 14, 236, 236).
    const float k = std::min(dst.right - dst.left, dst.bottom - dst.top) / 236.0f;
    D2D1_MATRIX_3X2_F outer{};
    dc->GetTransform(&outer);
    const D2D1_MATRIX_3X2_F to_dst = D2D1::Matrix3x2F::Translation(-10.0f, -14.0f) * D2D1::Matrix3x2F::Scale(k, k) *
                                     D2D1::Matrix3x2F::Translation(dst.left, dst.top);

    // Blurred shadows: recorded in logo space, drawn under the outer transform.
    auto blurred = [&](ID2D1Geometry* geo, D2D1_POINT_2F offset, D2D1_COLOR_F color) -> ComPtr<ID2D1Effect> {
        ComPtr<ID2D1CommandList> list;
        if (FAILED(dc->CreateCommandList(&list))) return {};
        ComPtr<ID2D1Image> prev;
        dc->GetTarget(&prev);
        dc->SetTarget(list.Get());
        dc->SetTransform(D2D1::Matrix3x2F::Translation(offset.x, offset.y) * to_dst);
        ComPtr<ID2D1SolidColorBrush> b;
        dc->CreateSolidColorBrush(color, &b);
        dc->FillGeometry(geo, b.Get());
        list->Close();
        dc->SetTarget(prev.Get());
        dc->SetTransform(outer);
        ComPtr<ID2D1Effect> blur;
        if (FAILED(dc->CreateEffect(CLSID_D2D1GaussianBlur, &blur))) return {};
        blur->SetInput(0, list.Get());
        blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, 7.0f * k);
        return blur;
    };
    auto fold_shadow = blurred(fold.Get(), D2D1::Point2F(-3, -6), LogoRgb(0x120F52, 0.5f));
    auto page_shadow = blurred(page.Get(), D2D1::Point2F(6, 0), LogoRgb(0x120F52, 0.45f));

    auto back = LogoGradient(dc, D2D1::Point2F(0, 36), D2D1::Point2F(0, 228),
                             {{0.0f, LogoRgb(0x2B2F86)}, {1.0f, LogoRgb(0x120F52)}});
    auto front = LogoGradient(dc, D2D1::Point2F(16, 70), D2D1::Point2F(210, 240),
                              {{0.0f, LogoRgb(0xC3C0FF)}, {0.48f, LogoRgb(0x8079F4)}, {1.0f, LogoRgb(0x4B3FC6)}});
    auto sheen = LogoGradient(dc, D2D1::Point2F(0, 72), D2D1::Point2F(0, 170),
                              {{0.0f, D2D1::ColorF(1, 1, 1, 0.30f)}, {1.0f, D2D1::ColorF(1, 1, 1, 0.0f)}});
    auto fold_fill = LogoGradient(dc, D2D1::Point2F(120, 80), D2D1::Point2F(236, 226),
                                  {{0.0f, LogoRgb(0x8C84F8)}, {1.0f, LogoRgb(0x4A3FC4)}});
    auto rim_stroke = LogoGradient(dc, D2D1::Point2F(0, 72), D2D1::Point2F(0, 225),
                                   {{0.0f, D2D1::ColorF(1, 1, 1, 0.55f)}, {0.72f, D2D1::ColorF(1, 1, 1, 0.55f)},
                                    {0.87f, D2D1::ColorF(1, 1, 1, 0.36f)}, {1.0f, D2D1::ColorF(1, 1, 1, 0.0f)}});
    if (!back || !front || !sheen || !fold_fill || !rim_stroke) return;

    // One clip for the whole composite, so stacked antialiased edges cannot reveal the dark back.
    dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), outline.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                         to_dst, 1.0f, nullptr, D2D1_LAYER_OPTIONS1_NONE),
                  nullptr);
    dc->SetTransform(to_dst * outer);
    dc->FillRectangle(D2D1::RectF(0, 0, 256, 256), back.Get());
    dc->SetTransform(outer);
    if (fold_shadow) dc->DrawImage(fold_shadow.Get());
    dc->SetTransform(to_dst * outer);
    dc->FillGeometry(fold.Get(), fold_fill.Get());
    dc->DrawGeometry(rim.Get(), rim_stroke.Get(), 1.0f);
    dc->SetTransform(outer);
    if (page_shadow) dc->DrawImage(page_shadow.Get());
    dc->SetTransform(to_dst * outer);
    dc->FillGeometry(page.Get(), front.Get());
    dc->FillGeometry(page.Get(), sheen.Get());
    dc->SetTransform(outer);
    dc->PopLayer();
}

void FillPoints(ID2D1Factory* factory, ID2D1RenderTarget* rt, const D2D1_POINT_2F* pts, size_t n, ID2D1Brush* brush) {
    if (!factory || !rt || !pts || n < 3 || !brush) return;
    ComPtr<ID2D1PathGeometry> geo;
    if (FAILED(factory->CreatePathGeometry(&geo)) || !geo) return;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geo->Open(&sink)) || !sink) return;
    sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_FILLED);
    for (size_t i = 1; i < n; ++i) sink->AddLine(pts[i]);
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (SUCCEEDED(sink->Close())) rt->FillGeometry(geo.Get(), brush);
}

double QpcDeltaMs(LARGE_INTEGER t0, LARGE_INTEGER t1) noexcept {
    LARGE_INTEGER freq{};
    if (!QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0) return 0.0;
    return static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}

} // namespace

// ---------------------------------------------------------------------------
// 1:1 Vector File & Folder Icon Renderer (matching desktop_pack_mockup.html ICON_SVG)
// ---------------------------------------------------------------------------
void DrawVectorFileIcon(ID2D1Factory* factory, ID2D1RenderTarget* rt, IDWriteFactory* dwrite,
                        const D2D1_RECT_F& icon_rect, std::wstring_view raw_ext, bool is_folder,
                        float opacity) {
    if (!factory || !rt) return;
    const float w = icon_rect.right - icon_rect.left;
    const float h = icon_rect.bottom - icon_rect.top;
    if (w < 8.0f || h < 8.0f) return;

    ComPtr<ID2D1SolidColorBrush> br;
    if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &br)) || !br) return;
    auto set_col = [&](uint32_t argb, float mul = 1.0f) -> ID2D1SolidColorBrush* {
        br->SetColor(ColorFromArgb(argb, opacity * mul));
        return br.Get();
    };

    const float x = icon_rect.left;
    const float y = icon_rect.top;

    if (is_folder) {
        // Back folder body + upper-left tab (#E6A93B)
        const D2D1_RECT_F tab_rc = D2D1::RectF(x + w * 0.08f, y + h * 0.16f, x + w * 0.44f, y + h * 0.36f);
        rt->FillRoundedRectangle(D2D1::RoundedRect(tab_rc, w * 0.06f, w * 0.06f), set_col(0xFFE6A93Bu));
        const D2D1_RECT_F back_rc = D2D1::RectF(x + w * 0.08f, y + h * 0.24f, x + w * 0.92f, y + h * 0.84f);
        rt->FillRoundedRectangle(D2D1::RoundedRect(back_rc, w * 0.07f, w * 0.07f), set_col(0xFFE09E2Eu));

        // Cream paper sheet peeking inside
        const D2D1_RECT_F paper_rc = D2D1::RectF(x + w * 0.15f, y + h * 0.28f, x + w * 0.85f, y + h * 0.44f);
        rt->FillRoundedRectangle(D2D1::RoundedRect(paper_rc, w * 0.04f, w * 0.04f), set_col(0xFFFFF8E8u));

        // Front golden flap (#FFCF73 -> #F2B544)
        const D2D1_RECT_F front_rc = D2D1::RectF(x + w * 0.06f, y + h * 0.35f, x + w * 0.94f, y + h * 0.86f);
        D2D1_GRADIENT_STOP fstops[2] = {
            {0.0f, ColorFromArgb(0xFFFFD47Eu, opacity)},
            {1.0f, ColorFromArgb(0xFFF2B441u, opacity)},
        };
        ComPtr<ID2D1GradientStopCollection> fcol;
        if (SUCCEEDED(rt->CreateGradientStopCollection(fstops, 2, &fcol)) && fcol) {
            ComPtr<ID2D1LinearGradientBrush> fbr;
            if (SUCCEEDED(rt->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(
                        D2D1::Point2F(x, front_rc.top), D2D1::Point2F(x, front_rc.bottom)),
                    fcol.Get(), &fbr)) && fbr) {
                rt->FillRoundedRectangle(D2D1::RoundedRect(front_rc, w * 0.08f, w * 0.08f), fbr.Get());
            }
        } else {
            rt->FillRoundedRectangle(D2D1::RoundedRect(front_rc, w * 0.08f, w * 0.08f), set_col(0xFFFFCD70u));
        }
        rt->DrawLine(D2D1::Point2F(front_rc.left + w * 0.08f, front_rc.top + 0.8f),
                     D2D1::Point2F(front_rc.right - w * 0.08f, front_rc.top + 0.8f),
                     set_col(0xFFFFFFFFu, 0.50f), 1.0f);
        return;
    }

    const std::wstring ext = LowerExtFromNameOrExt(raw_ext);

    // Image files (.png, .jpg, .jpeg, .webp, .bmp, .gif)
    if (ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".webp" || ext == L".bmp" || ext == L".gif") {
        const D2D1_RECT_F card = D2D1::RectF(x + w * 0.09f, y + h * 0.12f, x + w * 0.91f, y + h * 0.88f);
        rt->FillRoundedRectangle(D2D1::RoundedRect(card, w * 0.11f, w * 0.11f), set_col(0xFFF5F7FAu));
        const D2D1_RECT_F pic = D2D1::RectF(card.left + w * 0.07f, card.top + h * 0.07f,
                                            card.right - w * 0.07f, card.bottom - h * 0.07f);
        D2D1_GRADIENT_STOP sstops[2] = {
            {0.0f, ColorFromArgb(0xFF3E95F2u, opacity)},
            {1.0f, ColorFromArgb(0xFF91D2FFu, opacity)},
        };
        ComPtr<ID2D1GradientStopCollection> scol;
        if (SUCCEEDED(rt->CreateGradientStopCollection(sstops, 2, &scol)) && scol) {
            ComPtr<ID2D1LinearGradientBrush> sbr;
            if (SUCCEEDED(rt->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(
                        D2D1::Point2F(pic.left, pic.top), D2D1::Point2F(pic.left, pic.bottom)),
                    scol.Get(), &sbr)) && sbr) {
                rt->FillRoundedRectangle(D2D1::RoundedRect(pic, w * 0.06f, w * 0.06f), sbr.Get());
            }
        }
        const float sr = std::max(1.8f, w * 0.065f);
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(pic.right - w * 0.16f, pic.top + h * 0.16f), sr, sr),
                        set_col(0xFFFFDC73u));
        const D2D1_POINT_2F m1[3] = {
            {pic.left, pic.bottom},
            {pic.left + (pic.right - pic.left) * 0.38f, pic.top + (pic.bottom - pic.top) * 0.44f},
            {pic.left + (pic.right - pic.left) * 0.78f, pic.bottom},
        };
        FillPoints(factory, rt, m1, 3, set_col(0xFF235CA4u));
        const D2D1_POINT_2F m2[3] = {
            {pic.left + (pic.right - pic.left) * 0.36f, pic.bottom},
            {pic.left + (pic.right - pic.left) * 0.72f, pic.top + (pic.bottom - pic.top) * 0.54f},
            {pic.right, pic.bottom},
        };
        FillPoints(factory, rt, m2, 3, set_col(0xFF2F855Au));
        rt->DrawRoundedRectangle(D2D1::RoundedRect(card, w * 0.11f, w * 0.11f), set_col(0x28000000u), 1.0f);
        return;
    }

    // Archive files (.zip, .7z, .rar, .tar, .gz)
    if (ext == L".zip" || ext == L".7z" || ext == L".rar" || ext == L".tar" || ext == L".gz") {
        const D2D1_RECT_F box = D2D1::RectF(x + w * 0.12f, y + h * 0.10f, x + w * 0.88f, y + h * 0.90f);
        rt->FillRoundedRectangle(D2D1::RoundedRect(box, w * 0.11f, w * 0.11f), set_col(0xFFF0BE49u));
        rt->DrawRoundedRectangle(D2D1::RoundedRect(box, w * 0.11f, w * 0.11f), set_col(0xFF9C721Cu, 0.55f), 1.0f);
        const float cx = (box.left + box.right) * 0.5f;
        const D2D1_RECT_F track = D2D1::RectF(cx - w * 0.07f, box.top + h * 0.04f, cx + w * 0.07f, box.bottom - h * 0.24f);
        rt->FillRectangle(track, set_col(0xFF6B4F16u));
        for (int i = 0; i < 5; ++i) {
            const float ty = track.top + h * (0.05f + i * 0.085f);
            const float tx = (i % 2 == 0) ? (cx - w * 0.06f) : cx;
            rt->FillRectangle(D2D1::RectF(tx, ty, tx + w * 0.06f, ty + h * 0.045f), set_col(0xFFFFF4D2u));
        }
        const D2D1_RECT_F clasp = D2D1::RectF(cx - w * 0.09f, track.bottom, cx + w * 0.09f, track.bottom + h * 0.13f);
        rt->FillRoundedRectangle(D2D1::RoundedRect(clasp, w * 0.03f, w * 0.03f), set_col(0xFFF8FAFCu));
        return;
    }

    // Executable installer (.exe)
    if (ext == L".exe") {
        const D2D1_RECT_F win = D2D1::RectF(x + w * 0.10f, y + h * 0.14f, x + w * 0.90f, y + h * 0.86f);
        rt->FillRoundedRectangle(D2D1::RoundedRect(win, w * 0.11f, w * 0.11f), set_col(0xFFEDF2F8u));
        const D2D1_RECT_F bar = D2D1::RectF(win.left, win.top, win.right, win.top + h * 0.20f);
        rt->FillRoundedRectangle(D2D1::RoundedRect(bar, w * 0.10f, w * 0.10f), set_col(0xFF3872D6u));
        rt->FillRectangle(D2D1::RectF(bar.left, bar.bottom - h * 0.08f, bar.right, bar.bottom), set_col(0xFF3872D6u));
        const float cx = (win.left + win.right) * 0.5f;
        const float cy = win.top + (win.bottom - win.top) * 0.62f;
        const float cr = w * 0.17f;
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), cr, cr), set_col(0xFF2563EBu));
        rt->DrawLine(D2D1::Point2F(cx, cy - cr * 0.48f), D2D1::Point2F(cx, cy + cr * 0.42f),
                     set_col(0xFFFFFFFFu), std::max(1.3f, w * 0.045f));
        rt->DrawLine(D2D1::Point2F(cx - cr * 0.38f, cy + cr * 0.06f), D2D1::Point2F(cx, cy + cr * 0.45f),
                     set_col(0xFFFFFFFFu), std::max(1.3f, w * 0.045f));
        rt->DrawLine(D2D1::Point2F(cx + cr * 0.38f, cy + cr * 0.06f), D2D1::Point2F(cx, cy + cr * 0.45f),
                     set_col(0xFFFFFFFFu), std::max(1.3f, w * 0.045f));
        rt->DrawRoundedRectangle(D2D1::RoundedRect(win, w * 0.11f, w * 0.11f), set_col(0x2A000000u), 1.0f);
        return;
    }

    // Windows Installer package (.msi) — isometric parcel cube
    if (ext == L".msi") {
        const D2D1_POINT_2F top_face[4] = {
            {x + w * 0.50f, y + h * 0.12f},
            {x + w * 0.88f, y + h * 0.30f},
            {x + w * 0.50f, y + h * 0.48f},
            {x + w * 0.12f, y + h * 0.30f},
        };
        const D2D1_POINT_2F left_face[4] = {
            {x + w * 0.12f, y + h * 0.30f},
            {x + w * 0.50f, y + h * 0.48f},
            {x + w * 0.50f, y + h * 0.88f},
            {x + w * 0.12f, y + h * 0.69f},
        };
        const D2D1_POINT_2F right_face[4] = {
            {x + w * 0.50f, y + h * 0.48f},
            {x + w * 0.88f, y + h * 0.30f},
            {x + w * 0.88f, y + h * 0.69f},
            {x + w * 0.50f, y + h * 0.88f},
        };
        FillPoints(factory, rt, top_face, 4, set_col(0xFFF6DFB3u));
        FillPoints(factory, rt, left_face, 4, set_col(0xFFE0B063u));
        FillPoints(factory, rt, right_face, 4, set_col(0xFFC58E3Fu));
        rt->DrawLine(D2D1::Point2F(x + w * 0.31f, y + h * 0.21f),
                     D2D1::Point2F(x + w * 0.69f, y + h * 0.39f),
                     set_col(0xFFFFF4DCu, 0.85f), std::max(1.5f, w * 0.06f));
        return;
    }

    // Document page silhouette with folded corner + optional Office/PDF/MD ribbon badge
    const float pl = x + w * 0.18f;
    const float pt = y + h * 0.08f;
    const float pr = x + w * 0.86f;
    const float pb = y + h * 0.92f;
    const float fold = w * 0.20f;
    const D2D1_POINT_2F page_pts[5] = {
        {pl, pt},
        {pr - fold, pt},
        {pr, pt + fold},
        {pr, pb},
        {pl, pb},
    };
    FillPoints(factory, rt, page_pts, 5, set_col(0xFFF7F8FAu));
    const D2D1_POINT_2F fold_pts[3] = {
        {pr - fold, pt},
        {pr, pt + fold},
        {pr - fold, pt + fold},
    };
    FillPoints(factory, rt, fold_pts, 3, set_col(0xFFD8DFE9u));

    // Ruled content lines
    for (int i = 0; i < 3; ++i) {
        const float ly = pt + (pb - pt) * (0.38f + i * 0.16f);
        rt->DrawLine(D2D1::Point2F(pl + w * 0.11f, ly),
                     D2D1::Point2F(pr - w * (i == 2 ? 0.22f : 0.11f), ly),
                     set_col(0xFFC5CEDBu), std::max(1.0f, h * 0.042f));
    }

    uint32_t badge_col = 0;
    const wchar_t* badge_txt = nullptr;
    if (ext == L".docx" || ext == L".doc") {
        badge_col = 0xFF2B5BB5u;
        badge_txt = L"W";
    } else if (ext == L".xlsx" || ext == L".xls" || ext == L".csv") {
        badge_col = 0xFF1D7A46u;
        badge_txt = L"X";
    } else if (ext == L".pptx" || ext == L".ppt" || ext == L".potx") {
        badge_col = 0xFFC4523Au;
        badge_txt = L"P";
    } else if (ext == L".pdf") {
        badge_col = 0xFFC8372Du;
        badge_txt = (w >= 26.0f) ? L"PDF" : L"P";
    } else if (ext == L".md") {
        badge_col = 0xFF475569u;
        badge_txt = L"M";
    } else if (ext == L".cpp" || ext == L".cc" || ext == L".c" || ext == L".h" || ext == L".hpp") {
        badge_col = 0xFF0284C7u;
        badge_txt = L"C";
    }

    if (badge_col != 0 && badge_txt != nullptr) {
        const D2D1_RECT_F brc = D2D1::RectF(x + w * 0.06f, y + h * 0.40f, x + w * 0.54f, y + h * 0.78f);
        rt->FillRoundedRectangle(D2D1::RoundedRect(brc, w * 0.08f, w * 0.08f), set_col(badge_col));
        if (dwrite && w >= 16.0f) {
            const float fpx = (std::wcslen(badge_txt) > 1) ? std::max(6.5f, w * 0.16f) : std::max(7.5f, w * 0.25f);
            ComPtr<IDWriteTextFormat> tf;
            if (SUCCEEDED(dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD,
                                                   DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                   fpx, L"en-US", &tf)) && tf) {
                tf->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                tf->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                rt->DrawText(badge_txt, static_cast<UINT32>(std::wcslen(badge_txt)), tf.Get(), brc, set_col(0xFFFFFFFFu));
            }
        }
    }
}

DesktopRenderer::~DesktopRenderer() {
    Shutdown();
}

void DesktopRenderer::Shutdown() noexcept {
    widget_text_.Reset();
    host_backdrop_.Reset();
    backdrop_.ShutdownAsyncWorker();
    backdrop_.ReleaseDeviceResources();
    round_stroke_.Reset();
    dashed_stroke_.Reset();
    border_grad_brush_.Reset();
    border_grad_key_ = 0;
    card_shadow_bitmap_.Reset();
    solid_brush_.Reset();

    fmt_fluent_21_.Reset();
    fmt_fluent_14_.Reset();
    fmt_fluent_12_.Reset();
    fmt_banner_text_.Reset();
    fmt_empty_hint_.Reset();
    fmt_note_body_.Reset();
    fmt_clock_big_.Reset();
    fmt_disk_pct_.Reset();
    fmt_tile_small_title_.Reset();
    fmt_pill_.Reset();
    fmt_caption_.Reset();
    fmt_item_label_.Reset();
    fmt_row_meta_.Reset();
    fmt_row_lead_.Reset();
    fmt_row_name_.Reset();
    fmt_tile_sub_.Reset();
    fmt_tile_hdr_.Reset();
    fmt_group_title_.Reset();
    dwrite_factory_.Reset();

    if (d2d_dc_) {
        d2d_dc_->SetTarget(nullptr);
    }
    glass_pool_.Reset();
    live_wallpaper_.ReleaseDeviceResources();
    target_bitmap_.Reset();
    glass_vsurf_.Reset();
    glass_visual_.Reset();
    dcomp_visual_.Reset();
    dcomp_root_visual_.Reset();
    dcomp_target_.Reset();
    vsurf_.Reset();
    dcomp_device_.Reset();
    shared_dcomp_.Reset();
    swap_chain_.Reset();
    ID3D11Device* raw_d3d = d3d_device_.Get();
    d2d_dc_.Reset();
    d2d_device_.Reset();
    d2d_factory_.Reset();
    dxgi_device_.Reset();
    d3d_context_.Reset();
    d3d_device_.Reset();
    if (raw_d3d) {
        ui::Compositor::ReleaseSharedThreadDevices(raw_d3d);
    }

    hwnd_ = nullptr;
    width_ = 0;
    height_ = 0;
    using_dcomp_ = false;
    has_presented_full_frame_ = false;
    glass_only_dirty_ = false;
    dirty_rects_.clear();
    glass_dirty_rects_.clear();
    last_trimmed_occupied_.clear();
    last_glass_trimmed_occupied_.clear();
    pending_glass_cards_.clear();
    committed_glass_cards_.clear();
    active_clip_dip_ = D2D1::RectF(0.0f, 0.0f, 1e6f, 1e6f);
}

bool DesktopRenderer::CreateCoreDevices() {
    ui::Compositor::SharedThreadDevices shared{};
    if (!ui::Compositor::AcquireSharedThreadDevices(shared) || !shared.d3d || !shared.dxgi ||
        !shared.d2d_factory || !shared.d2d) {
        return false;
    }
    d3d_device_ = shared.d3d;
    d3d_device_->GetImmediateContext(&d3d_context_);
    dxgi_device_ = shared.dxgi;
    d2d_factory_ = shared.d2d_factory;
    d2d_device_ = shared.d2d;
    shared_dcomp_ = shared.dcomp;

    // Cap Direct2D's internal resource/glyph/staging cache across the shared thread device.
    d2d_device_->SetMaximumTextureMemory(6u * 1024u * 1024u);

    if (FAILED(d2d_device_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2d_dc_)) || !d2d_dc_) {
        return false;
    }
    d2d_dc_->SetUnitMode(D2D1_UNIT_MODE_PIXELS);

    if (FAILED(d2d_dc_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &solid_brush_))) {
        return false;
    }

    D2D1_STROKE_STYLE_PROPERTIES dash_props = D2D1::StrokeStyleProperties(
        D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
        D2D1_LINE_JOIN_ROUND, 4.0f, D2D1_DASH_STYLE_DASH, 0.0f);
    d2d_factory_->CreateStrokeStyle(dash_props, nullptr, 0, &dashed_stroke_);

    D2D1_STROKE_STYLE_PROPERTIES round_props = D2D1::StrokeStyleProperties(
        D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
        D2D1_LINE_JOIN_ROUND, 4.0f, D2D1_DASH_STYLE_SOLID, 0.0f);
    d2d_factory_->CreateStrokeStyle(round_props, nullptr, 0, &round_stroke_);

    return CreateTextFormats();
}

bool DesktopRenderer::CreateTextFormats() {
    if (FAILED(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(dwrite_factory_.GetAddressOf()))) || !dwrite_factory_) {
        return false;
    }

    auto family_exists = [this](const wchar_t* family_name) noexcept -> bool {
        if (!dwrite_factory_ || !family_name) return false;
        ComPtr<IDWriteFontCollection> collection;
        if (FAILED(dwrite_factory_->GetSystemFontCollection(&collection, FALSE)) || !collection) return false;
        UINT32 idx = 0;
        BOOL exists = FALSE;
        return SUCCEEDED(collection->FindFamilyName(family_name, &idx, &exists)) && exists;
    };
    const wchar_t* const body_family = family_exists(L"Segoe UI Variable Text")
        ? L"Segoe UI Variable Text"
        : (family_exists(L"Microsoft YaHei UI") ? L"Microsoft YaHei UI" : L"Segoe UI");
    const wchar_t* const icon_family = family_exists(L"Segoe Fluent Icons")
        ? L"Segoe Fluent Icons"
        : L"Segoe MDL2 Assets";

    auto make_fmt = [&](float px, DWRITE_FONT_WEIGHT weight,
                        DWRITE_TEXT_ALIGNMENT h_align,
                        DWRITE_PARAGRAPH_ALIGNMENT v_align,
                        bool word_wrap,
                        bool ellipsis_trim,
                        ComPtr<IDWriteTextFormat>& out) -> bool {
        if (FAILED(dwrite_factory_->CreateTextFormat(
                body_family, nullptr, weight,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                px, L"zh-CN", &out))) {
            if (FAILED(dwrite_factory_->CreateTextFormat(
                    L"Microsoft YaHei UI", nullptr, weight,
                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                    px, L"zh-CN", &out))) {
                return false;
            }
        }
        out->SetTextAlignment(h_align);
        out->SetParagraphAlignment(v_align);
        out->SetWordWrapping(word_wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        if (ellipsis_trim) {
            ComPtr<IDWriteInlineObject> ellipsis;
            if (SUCCEEDED(dwrite_factory_->CreateEllipsisTrimmingSign(out.Get(), &ellipsis)) && ellipsis) {
                DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                out->SetTrimming(&trim, ellipsis.Get());
            }
        }
        return true;
    };

    auto make_fluent = [&](float px, ComPtr<IDWriteTextFormat>& out) -> bool {
        if (FAILED(dwrite_factory_->CreateTextFormat(
                icon_family, nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                px, L"en-US", &out))) {
            if (FAILED(dwrite_factory_->CreateTextFormat(
                    L"Segoe MDL2 Assets", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                    px, L"en-US", &out))) {
                return false;
            }
        }
        out->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        return true;
    };

    const bool ok =
        make_fmt(13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING,
                 DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false, true, fmt_group_title_) &&
        make_fmt(12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING,
                 DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false, true, fmt_tile_hdr_) &&
        make_fmt(11.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING,
                 DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false, true, fmt_tile_sub_) &&
        make_fmt(12.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING,
                 DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false, true, fmt_row_name_) &&
        make_fmt(12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING,
                 DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false, true, fmt_row_lead_) &&
        make_fmt(11.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING,
                 DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false, false, fmt_row_meta_) &&
        make_fmt(12.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER,
                 DWRITE_PARAGRAPH_ALIGNMENT_NEAR, true, true, fmt_item_label_) &&
        make_fmt(11.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING,
                 DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false, true, fmt_caption_) &&
        make_fmt(11.0f, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER,
                 DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false, false, fmt_pill_) &&
        make_fmt(11.5f, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER,
                 DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false, true, fmt_tile_small_title_) &&
        make_fmt(12.0f, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER,
                 DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false, false, fmt_disk_pct_) &&
        make_fmt(42.0f, DWRITE_FONT_WEIGHT_LIGHT, DWRITE_TEXT_ALIGNMENT_LEADING,
                 DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false, false, fmt_clock_big_) &&
        make_fmt(12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING,
                 DWRITE_PARAGRAPH_ALIGNMENT_NEAR, true, true, fmt_note_body_) &&
        make_fmt(12.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER,
                 DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false, false, fmt_empty_hint_) &&
        make_fmt(12.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING,
                 DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false, false, fmt_banner_text_) &&
        make_fluent(12.0f, fmt_fluent_12_) &&
        make_fluent(13.5f, fmt_fluent_14_) &&
        make_fluent(21.0f, fmt_fluent_21_);

    if (ok && fmt_note_body_) {
        fmt_note_body_->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 19.0f, 15.0f);
    }
    if (ok && fmt_item_label_) {
        fmt_item_label_->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 15.0f, 12.0f);
    }
    return ok;
}

float DesktopRenderer::MeasureSingleLineWidth(const std::wstring& text, IDWriteTextFormat* fmt, float max_w) const {
    if (text.empty() || !dwrite_factory_ || !fmt) return 0.0f;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_factory_->CreateTextLayout(
            text.c_str(), static_cast<UINT32>(text.size()), fmt, max_w, 40.0f, &layout)) || !layout) {
        return static_cast<float>(text.size()) * 12.0f;
    }
    DWRITE_TEXT_METRICS tm{};
    if (FAILED(layout->GetMetrics(&tm))) {
        return static_cast<float>(text.size()) * 12.0f;
    }
    return tm.widthIncludingTrailingWhitespace;
}

bool DesktopRenderer::CreateSwapChainResources(HWND hwnd, uint32_t width, uint32_t height, bool prefer_dcomp) {
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgi_device_->GetAdapter(&adapter)) || !adapter) return false;
    ComPtr<IDXGIFactory2> dxgi_factory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(&dxgi_factory))) || !dxgi_factory) return false;

    DXGI_SWAP_CHAIN_DESC1 sc_desc{};
    sc_desc.Width = std::max<uint32_t>(1u, width);
    sc_desc.Height = std::max<uint32_t>(1u, height);
    sc_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sc_desc.SampleDesc.Count = 1;
    sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc_desc.BufferCount = 2;
    sc_desc.Scaling = DXGI_SCALING_STRETCH;
    sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;

    using_dcomp_ = false;
    if (prefer_dcomp) {
        sc_desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        if (SUCCEEDED(dxgi_factory->CreateSwapChainForComposition(
                d3d_device_.Get(), &sc_desc, nullptr, &swap_chain_)) && swap_chain_) {
            if (!hwnd) {
                using_dcomp_ = true;
            } else {
                HRESULT dc_hr = DCompositionCreateDevice(dxgi_device_.Get(), IID_PPV_ARGS(&dcomp_device_));
                if (FAILED(dc_hr)) {
                    dc_hr = ui::Compositor::CreateCompositionOnlyDevice(&dcomp_device_);
                }
                if (FAILED(dc_hr) && shared_dcomp_) {
                    dcomp_device_ = shared_dcomp_;
                    dc_hr = S_OK;
                }
                if (SUCCEEDED(dc_hr) && dcomp_device_ &&
                    SUCCEEDED(dcomp_device_->CreateTargetForHwnd(hwnd, TRUE, &dcomp_target_)) &&
                    SUCCEEDED(dcomp_device_->CreateVisual(&dcomp_visual_)) &&
                    SUCCEEDED(dcomp_visual_->SetContent(swap_chain_.Get())) &&
                    SUCCEEDED(dcomp_target_->SetRoot(dcomp_visual_.Get())) &&
                    SUCCEEDED(dcomp_device_->Commit())) {
                    using_dcomp_ = true;
                } else {
                    dcomp_visual_.Reset();
                    dcomp_target_.Reset();
                    dcomp_device_.Reset();
                    swap_chain_.Reset();
                }
            }
        }
    }

    if (!swap_chain_ && hwnd) {
        sc_desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        sc_desc.Scaling = DXGI_SCALING_NONE;
        if (FAILED(dxgi_factory->CreateSwapChainForHwnd(
                d3d_device_.Get(), hwnd, &sc_desc, nullptr, nullptr, &swap_chain_))) {
            sc_desc.Scaling = DXGI_SCALING_STRETCH;
            if (FAILED(dxgi_factory->CreateSwapChainForHwnd(
                    d3d_device_.Get(), hwnd, &sc_desc, nullptr, nullptr, &swap_chain_))) {
                return false;
            }
        }
    }

    if (!swap_chain_) return false;
    has_presented_full_frame_ = false;
    if (hwnd && using_dcomp_) {
        d2d_dc_->SetUnitMode(D2D1_UNIT_MODE_DIPS);
        d2d_dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    return BindSwapChainTargetBitmap();
}

bool DesktopRenderer::BindSwapChainTargetBitmap() {
    if (!swap_chain_ || !d2d_dc_) return false;
    d2d_dc_->SetTarget(nullptr);
    target_bitmap_.Reset();

    ComPtr<IDXGISurface> surface;
    if (FAILED(swap_chain_->GetBuffer(0, IID_PPV_ARGS(&surface))) || !surface) return false;

    const float target_dpi = (hwnd_ && using_dcomp_) ? static_cast<float>(dpi_ ? dpi_ : 96u) : 96.0f;
    const D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        target_dpi, target_dpi);
    if (FAILED(d2d_dc_->CreateBitmapFromDxgiSurface(surface.Get(), &bp, &target_bitmap_)) || !target_bitmap_) {
        return false;
    }
    d2d_dc_->SetTarget(target_bitmap_.Get());
    if (hwnd_ && using_dcomp_) {
        d2d_dc_->SetDpi(target_dpi, target_dpi);
    }
    return true;
}

bool DesktopRenderer::InitializeForHwnd(HWND hwnd, uint32_t width, uint32_t height, bool prefer_dcomp,
                                        bool load_default_wallpaper) {
    Shutdown();
    hwnd_ = hwnd;
    width_ = std::max<uint32_t>(64u, width);
    height_ = std::max<uint32_t>(64u, height);

    if (!CreateCoreDevices()) {
        Shutdown();
        return false;
    }
    // A layer window composes one sparse virtual surface; fall back to a composition swapchain
    // (or HWND swapchain) if VirtualSurface is unavailable, while offscreen/snapshots keep swapchain.
    bool created = (hwnd && prefer_dcomp) ? CreateVirtualSurfaceResources(hwnd, width_, height_) : false;
    if (!created) {
        created = CreateSwapChainResources(hwnd, width_, height_, prefer_dcomp);
    }
    if (!created) {
        Shutdown();
        return false;
    }
    if (hwnd && vsurf_) host_backdrop_.Initialize(hwnd);
    if (load_default_wallpaper) {
        backdrop_.LoadForScreen(d2d_dc_.Get(), BackdropWidth(), BackdropHeight());
        ApplyBackdropMapping();
        TrimDxgiMemory();
    }
    InvalidateAll();
    return true;
}

bool DesktopRenderer::InitializeOffscreen(uint32_t width, uint32_t height) {
    return InitializeForHwnd(nullptr, width, height, true);
}

bool DesktopRenderer::Resize(uint32_t width, uint32_t height) {
    width = std::max<uint32_t>(64u, width);
    height = std::max<uint32_t>(64u, height);
    last_trimmed_occupied_.clear();
    last_glass_trimmed_occupied_.clear();
    if (vsurf_) {
        if (width != width_ || height != height_) {
            if (FAILED(vsurf_->Resize(width, height))) return false;
            if (glass_vsurf_ && FAILED(glass_vsurf_->Resize(width, height))) {
                glass_vsurf_.Reset();
                if (glass_visual_) glass_visual_->SetContent(nullptr);
            }
            width_ = width;
            height_ = height;
        }
        backdrop_.LoadForScreen(d2d_dc_.Get(), BackdropWidth(), BackdropHeight(), backdrop_.ActivePreset(),
                                backdrop_.PresetDarkMode());
        ApplyBackdropMapping();
        TrimDxgiMemory();
        InvalidateAll();
        return true;
    }
    if (width == width_ && height == height_ && swap_chain_) return true;
    if (!swap_chain_ || !d2d_dc_) return false;

    d2d_dc_->SetTarget(nullptr);
    target_bitmap_.Reset();

    if (FAILED(swap_chain_->ResizeBuffers(2, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 0))) {
        return false;
    }
    width_ = width;
    height_ = height;
    has_presented_full_frame_ = false;
    if (!BindSwapChainTargetBitmap()) return false;
    if (dcomp_device_) {
        dcomp_device_->Commit();
    }
    backdrop_.LoadForScreen(d2d_dc_.Get(), BackdropWidth(), BackdropHeight(),
                            backdrop_.ActivePreset(), backdrop_.PresetDarkMode());
    if (hwnd_ && using_dcomp_) ApplyBackdropMapping();
    TrimDxgiMemory();
    InvalidateAll();
    return true;
}

void DesktopRenderer::TrimDxgiMemory() noexcept {
    if (d2d_device_) {
        d2d_device_->ClearResources(0);
    }
    if (!dxgi_device_) return;
    ComPtr<IDXGIDevice3> dxgi3;
    if (SUCCEEDED(dxgi_device_.As(&dxgi3)) && dxgi3) {
        dxgi3->Trim();
    }
}

bool DesktopRenderer::ReloadWallpaper(model::WallpaperPreset preset, bool dark_mode, const std::wstring& override_path) {
    if (!d2d_dc_) return false;
    const bool ok = backdrop_.LoadForScreen(d2d_dc_.Get(), BackdropWidth(), BackdropHeight(), preset, dark_mode,
                                            override_path);
    ApplyBackdropMapping();
    TrimDxgiMemory();
    InvalidateAll();
    return ok;
}

void DesktopRenderer::InvalidateAll() noexcept {
    full_dirty_ = true;
    dirty_rects_.clear();
}

void DesktopRenderer::InvalidateRect(const model::RectI& rc) noexcept {
    if (full_dirty_ || rc.empty()) return;
    RECT clamped{};
    clamped.left = std::clamp<LONG>(rc.x - 16, 0, static_cast<LONG>(width_));
    clamped.top = std::clamp<LONG>(rc.y - 16, 0, static_cast<LONG>(height_));
    clamped.right = std::clamp<LONG>(rc.right() + 16, 0, static_cast<LONG>(width_));
    clamped.bottom = std::clamp<LONG>(rc.bottom() + 16, 0, static_cast<LONG>(height_));
    if (clamped.right <= clamped.left || clamped.bottom <= clamped.top) return;

    if (dirty_rects_.size() >= 16) {
        InvalidateAll();
        return;
    }
    dirty_rects_.push_back(clamped);
}

bool DesktopRenderer::ShouldPauseLiveActivity(bool enabled) noexcept {
    if (!enabled) return false;
    QUERY_USER_NOTIFICATION_STATE quns = QUNS_ACCEPTS_NOTIFICATIONS;
    if (SUCCEEDED(SHQueryUserNotificationState(&quns))) {
        if (quns == QUNS_BUSY ||
            quns == QUNS_RUNNING_D3D_FULL_SCREEN ||
            quns == QUNS_PRESENTATION_MODE) {
            return true;
        }
    }
    SYSTEM_POWER_STATUS sps{};
    if (GetSystemPowerStatus(&sps)) {
        if (sps.SystemStatusFlag == 1 ||
            (sps.ACLineStatus == 0 && sps.BatteryLifePercent <= 20 && sps.BatteryLifePercent != 255)) {
            return true;
        }
    }
    return false;
}

model::RectI DesktopRenderer::ComputeBoardRectFallback(const model::RectI& work_area, bool tiles_enabled) noexcept {
    if (!tiles_enabled || work_area.empty()) return {};
    const int board_w = model::kTileCols * model::kTileUnitPx + (model::kTileCols - 1) * model::kTileGapPx;
    const int board_h = model::kTileRows * model::kTileUnitPx + (model::kTileRows - 1) * model::kTileGapPx;
    return model::RectI{
        work_area.right() - model::kTileBoardRightMargin - board_w,
        work_area.y + model::kTileBoardTopMargin,
        board_w,
        board_h,
    };
}

model::RectI DesktopRenderer::ComputeTileRectInline(const model::RectI& board_rect,
                                                    const model::DesktopTile& tile) noexcept {
    return model::ResolveTileRect(tile, board_rect);
}

namespace {
struct GroupDetailsColumns { D2D1_RECT_F name{}, size{}, modified{}; };
GroupDetailsColumns DetailsColumns(const D2D1_RECT_F& rect) {
    GroupDetailsColumns columns;
    float right = rect.right - 6;
    if (rect.right - rect.left >= 420) {
        columns.modified = D2D1::RectF(right - 112, rect.top, right, rect.bottom);
        right -= 120;
    }
    if (rect.right - rect.left >= 280) {
        columns.size = D2D1::RectF(right - 72, rect.top, right, rect.bottom);
        right -= 80;
    }
    columns.name = D2D1::RectF(rect.left + 34, rect.top, right, rect.bottom);
    return columns;
}
std::wstring GroupModifiedDate(int64_t unix_ms) {
    if (unix_ms <= 0) return {};
    ULARGE_INTEGER value{};
    value.QuadPart = (static_cast<uint64_t>(unix_ms) + 11644473600000ULL) * 10000ULL;
    FILETIME utc{value.LowPart, value.HighPart}, local{};
    SYSTEMTIME time{};
    wchar_t text[80]{};
    if (FileTimeToLocalFileTime(&utc, &local) && FileTimeToSystemTime(&local, &time))
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &time, nullptr, text, ARRAYSIZE(text), nullptr);
    return text;
}
}

GroupTabLayout ComputeGroupTabLayout(const model::GroupComputedView& view) {
    GroupTabLayout result;
    if (view.frame.empty() || view.tab_group_ids.size() < 2) return result;
    const int count = static_cast<int>(view.tab_group_ids.size());
    const int active = std::clamp(view.active_tab_index, 0, count - 1);
    int left = view.frame.x + 8;
    const int right = view.frame.right() - 68;
    int available = right - left;
    if (available <= 0) return result;
    const bool overflow = count > available / 58;
    if (overflow) {
        result.previous = {left, view.frame.y + 5, 18, 28};
        result.next = {right - 18, view.frame.y + 5, 18, 28};
        if (active > 0) result.previous_group_id = view.tab_group_ids[static_cast<size_t>(active - 1)];
        if (active + 1 < count) result.next_group_id = view.tab_group_ids[static_cast<size_t>(active + 1)];
        left += 18;
        available -= 36;
    }
    if (available <= 0) return result;
    const int shown = std::min(count, std::max(1, available / 58));
    const int first = std::clamp(active - shown / 2, 0, count - shown);
    const int width = std::min(120, available / shown);
    for (int i = first; i < first + shown; ++i) {
        result.tabs.push_back({view.tab_group_ids[static_cast<size_t>(i)],
            {left + (i - first) * width, view.frame.y + 4, width - (shown > 1 ? 2 : 0), 30}, i == active});
    }
    return result;
}

std::string HitTestGroupTab(const model::GroupComputedView& view, int x, int y) {
    const auto layout = ComputeGroupTabLayout(view);
    if (layout.previous.contains(x, y)) return layout.previous_group_id;
    if (layout.next.contains(x, y)) return layout.next_group_id;
    for (const auto& tab : layout.tabs) if (tab.rect.contains(x, y)) return tab.group_id;
    return {};
}

std::vector<model::GroupComputedView> DesktopRenderer::BuildSimpleGroupViews(
    const model::DesktopModelState& state) {
    FILETIME time{};
    GetSystemTimeAsFileTime(&time);
    ULARGE_INTEGER value{};
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return model::ComputeGroupViews(state, static_cast<int64_t>(value.QuadPart / 10000ULL - 11644473600000ULL));
}

GroupHeaderHitZone DesktopRenderer::HitTestGroupCard(const model::DesktopGroup& group,
                                                     const model::GroupComputedView& view,
                                                     bool /*focus_scene*/,
                                                     uint64_t cleanable_installer_bytes,
                                                     bool group_hovered,
                                                     int px, int py) const noexcept {
    const bool effective_collapsed = !view.expanded;
    model::RectI frame = view.frame;
    if (effective_collapsed) {
        frame.h = model::kGroupHeaderHeight;
    }
    if (!frame.contains(px, py)) return GroupHeaderHitZone::None;

    if (!effective_collapsed && px >= frame.right() - 16 && py >= frame.bottom() - 16) {
        return GroupHeaderHitZone::ResizeGrip;
    }
    if (!effective_collapsed) {
        const GroupFooterLayout footer = ComputeGroupFooterLayout(frame, view);
        if (footer.active && py >= frame.bottom() - model::kGroupFooterHeight) {
            const float fx = static_cast<float>(px);
            const float fy = static_cast<float>(py);
            auto in_rc = [fx, fy](const D2D1_RECT_F& r) noexcept {
                return fx >= r.left && fx < r.right && fy >= r.top && fy < r.bottom;
            };
            if (in_rc(footer.prev_rc)) return GroupHeaderHitZone::FooterPrevPage;
            if (in_rc(footer.dots_rc)) return GroupHeaderHitZone::FooterPagerDots;
            if (in_rc(footer.next_rc)) return GroupHeaderHitZone::FooterNextPage;
            if (in_rc(footer.view_all_rc) || fx >= footer.view_all_rc.left) {
                return GroupHeaderHitZone::FooterViewAll;
            }
            return GroupHeaderHitZone::None;
        }
    }
    if (py >= frame.y + model::kGroupHeaderHeight) {
        return GroupHeaderHitZone::None;
    }

    const bool show_right_btns = group_hovered || effective_collapsed;
    if (show_right_btns) {
        // Menu button [right-34 .. right-8], Collapse button [right-62 .. right-36]
        if (px >= frame.right() - 34 && px < frame.right() - 8) {
            return GroupHeaderHitZone::MenuButton;
        }
        if (px >= frame.right() - 62 && px < frame.right() - 36) {
            return GroupHeaderHitZone::CollapseChevron;
        }
    }

    if (view.tab_group_ids.size() > 1) {
        const auto tabs = ComputeGroupTabLayout(view);
        if (tabs.previous.contains(px, py) && !tabs.previous_group_id.empty()) return GroupHeaderHitZone::TabPrev;
        if (tabs.next.contains(px, py) && !tabs.next_group_id.empty()) return GroupHeaderHitZone::TabNext;
        for (const auto& tab : tabs.tabs) if (tab.rect.contains(px, py)) return GroupHeaderHitZone::Tab;
        return GroupHeaderHitZone::None;
    }

    const float chip_right = static_cast<float>(frame.right() - (frame.w >= 260 ? 68 : (show_right_btns ? 68 : 12)));
    if (IsInstallerGroup(group) && !effective_collapsed && cleanable_installer_bytes > 0 && frame.w >= 260) {
        const std::wstring chip_txt = L"可清理 " + FormatBytesCompact(cleanable_installer_bytes);
        const float chip_w = MeasureSingleLineWidth(chip_txt, fmt_pill_.Get(), 200.0f) + 28.0f;
        if (px >= static_cast<int>(chip_right - chip_w) && px <= static_cast<int>(chip_right) &&
            py >= frame.y + 7 && py <= frame.y + 31) {
            return GroupHeaderHitZone::CleanChip;
        }
    } else if (!effective_collapsed && !view.single_subfolder_path.empty() && frame.w >= 260) {
        const std::wstring chip_txt = L"展开文件夹内容";
        const float chip_w = MeasureSingleLineWidth(chip_txt, fmt_pill_.Get(), 160.0f) + 28.0f;
        if (px >= static_cast<int>(chip_right - chip_w) && px <= static_cast<int>(chip_right) &&
            py >= frame.y + 7 && py <= frame.y + 31) {
            return GroupHeaderHitZone::MapSingleFolderChip;
        }
    } else if (group_hovered && !group.mapped_folder_path.empty() && frame.w >= 260) {
        const std::wstring chip_txt = L"目录 · " + MappedFolderLeafName(group.mapped_folder_path);
        const float txt_w = std::min(152.0f, MeasureSingleLineWidth(chip_txt, fmt_pill_.Get(), 152.0f));
        const float chip_w = txt_w + 28.0f;
        if (px >= static_cast<int>(chip_right - chip_w) && px <= static_cast<int>(chip_right) &&
            py >= frame.y + 7 && py <= frame.y + 31) {
            return GroupHeaderHitZone::RuleButton;
        }
    } else if (group_hovered && !group.rule_summary.empty() && frame.w >= 260) {
        const float chip_w = std::min(180.0f, MeasureSingleLineWidth(group.rule_summary, fmt_pill_.Get(), 180.0f) + 28.0f);
        if (px >= static_cast<int>(chip_right - chip_w) && px <= static_cast<int>(chip_right) &&
            py >= frame.y + 7 && py <= frame.y + 31) {
            return GroupHeaderHitZone::RuleButton;
        }
    }

    return GroupHeaderHitZone::None;
}

int DesktopRenderer::HitTestGroupFooterDot(const model::GroupComputedView& view, int px, int py) const noexcept {
    const GroupFooterLayout footer = ComputeGroupFooterLayout(view.frame, view);
    if (!footer.active) return -1;
    const float fx = static_cast<float>(px);
    const float fy = static_cast<float>(py);
    if (fy < footer.dots_rc.top || fy >= footer.dots_rc.bottom ||
        fx < footer.dots_rc.left || fx >= footer.dots_rc.right) {
        return -1;
    }
    int best_page = footer.cur_page;
    float best_dist = 1e9f;
    for (int k = 0; k < footer.shown_dots; ++k) {
        const D2D1_RECT_F& dr = footer.dot_rects[static_cast<size_t>(k)];
        if (fx >= dr.left - 2.0f && fx <= dr.right + 2.0f) {
            return footer.start_page + k;
        }
        const float center = (dr.left + dr.right) * 0.5f;
        const float dist = std::abs(fx - center);
        if (dist < best_dist) {
            best_dist = dist;
            best_page = footer.start_page + k;
        }
    }
    return best_page;
}

TileResizeZone DesktopRenderer::HitTestTileResize(const model::DesktopTile& tile,
                                                  const model::RectI& tile_rect,
                                                  int px, int py) noexcept {
    if (tile_rect.empty() || !tile_rect.contains(px, py)) {
        return TileResizeZone::None;
    }
    const bool in_right_band = (px >= tile_rect.right() - 10);
    const bool in_bottom_band = (py >= tile_rect.bottom() - 10);
    if (px >= tile_rect.right() - 18 && py >= tile_rect.bottom() - 18) {
        return TileResizeZone::BottomRight;
    }
    if (in_bottom_band && px >= tile_rect.x + 12 && px < tile_rect.right() - 18) {
        return TileResizeZone::Bottom;
    }
    if (in_right_band && py >= tile_rect.y + 34 && py < tile_rect.bottom() - 18) {
        if (tile.stack.size() >= 2) {
            const int mid_y = tile_rect.y + tile_rect.h / 2;
            const int dots_half_band = static_cast<int>(tile.stack.size()) * 10 + 26;
            if (std::abs(py - mid_y) <= dots_half_band) {
                return TileResizeZone::None;
            }
        }
        return TileResizeZone::Right;
    }
    return TileResizeZone::None;
}

bool DesktopRenderer::Tick(const model::DesktopModelState& state,
                           std::span<const model::GroupComputedView> group_views,
                           const model::RectI& tile_board_rect,
                           const LiveTileRuntimeData& live_data,
                           const DesktopInteractionState& interaction) {
    if (!d2d_dc_ || (!swap_chain_ && !vsurf_)) return false;

    current_glass_settings_ = state.settings;
    current_glass_live_rate_ = state.settings.glass_live_rate;
    const uint64_t now_ms = GetTickCount64();
    if (now_ms - energy_saver_check_ms_ > 2000ull) {
        energy_saver_check_ms_ = now_ms;
        SYSTEM_POWER_STATUS sps{};
        energy_saver_cached_ = state.settings.pause_on_fullscreen_or_battery &&
                               GetSystemPowerStatus(&sps) &&
                               (sps.ACLineStatus == 0 || sps.SystemStatusFlag == 1);
    }

    // Auto-sync preset wallpaper if user switched preset or theme in Beautify settings!
    if (state.settings.wallpaper_preset != backdrop_.ActivePreset() ||
        (state.settings.wallpaper_preset != model::WallpaperPreset::System &&
         state.settings.dark_mode != backdrop_.PresetDarkMode())) {
        backdrop_.LoadForScreen(d2d_dc_.Get(), BackdropWidth(), BackdropHeight(),
                                state.settings.wallpaper_preset, state.settings.dark_mode);
        ApplyBackdropMapping();
        full_dirty_ = true;
    }
    if (vsurf_) return TickVirtual(state, group_views, tile_board_rect, live_data, interaction);

    UpdateHoverLight(state, group_views, tile_board_rect, interaction);
    if (hover_light_.animating || glass_only_dirty_) {
        full_dirty_ = true;
        glass_only_dirty_ = false;
    }

    // Strict 0-Present Idle Sleep: zero BeginDraw and zero Present1 when nothing changed!
    if (!full_dirty_ && dirty_rects_.empty() && !animating_) {
        metrics_.idle_ticks_skipped++;
        return false;
    }

    glass_pool_.BeginFrame();

    LARGE_INTEGER t0{}, t1{};
    QueryPerformanceCounter(&t0);

    const bool use_dirty_rects = !full_dirty_ && !animating_ && has_presented_full_frame_ && !dirty_rects_.empty();
    RECT union_dirty{};
    if (use_dirty_rects) {
        union_dirty = dirty_rects_[0];
        for (size_t i = 1; i < dirty_rects_.size(); ++i) {
            union_dirty.left = std::min(union_dirty.left, dirty_rects_[i].left);
            union_dirty.top = std::min(union_dirty.top, dirty_rects_[i].top);
            union_dirty.right = std::max(union_dirty.right, dirty_rects_[i].right);
            union_dirty.bottom = std::max(union_dirty.bottom, dirty_rects_[i].bottom);
        }
    }

    if (hwnd_ && using_dcomp_) {
        d2d_dc_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
    }
    d2d_dc_->BeginDraw();
    if (use_dirty_rects) {
        active_clip_dip_ = D2D1::RectF(
            static_cast<float>(union_dirty.left),
            static_cast<float>(union_dirty.top),
            static_cast<float>(union_dirty.right),
            static_cast<float>(union_dirty.bottom));
        d2d_dc_->PushAxisAlignedClip(active_clip_dip_, D2D1_ANTIALIAS_MODE_ALIASED);
    } else {
        active_clip_dip_ = D2D1::RectF(0.0f, 0.0f, 1e6f, 1e6f);
    }

    DrawScene(state, group_views, tile_board_rect, live_data, interaction);
    active_clip_dip_ = D2D1::RectF(0.0f, 0.0f, 1e6f, 1e6f);

    if (use_dirty_rects) {
        d2d_dc_->PopAxisAlignedClip();
    }
    const HRESULT end_hr = d2d_dc_->EndDraw();
    glass_pool_.EndFrame();

    HRESULT present_hr = S_OK;
    if (SUCCEEDED(end_hr)) {
        DXGI_PRESENT_PARAMETERS params{};
        std::vector<RECT> present_dirty;
        if (use_dirty_rects) {
            if (hwnd_ && using_dcomp_ && dip_scale_ != 1.0f) {
                present_dirty.reserve(dirty_rects_.size());
                for (const RECT& d : dirty_rects_) {
                    RECT pr = DipRectToPx(model::RectI{d.left, d.top, d.right - d.left, d.bottom - d.top}, 0);
                    pr.left = std::clamp<LONG>(pr.left, 0, static_cast<LONG>(width_));
                    pr.top = std::clamp<LONG>(pr.top, 0, static_cast<LONG>(height_));
                    pr.right = std::clamp<LONG>(pr.right, 0, static_cast<LONG>(width_));
                    pr.bottom = std::clamp<LONG>(pr.bottom, 0, static_cast<LONG>(height_));
                    if (pr.right > pr.left && pr.bottom > pr.top) present_dirty.push_back(pr);
                }
            } else {
                present_dirty = dirty_rects_;
            }
            if (!present_dirty.empty()) {
                params.DirtyRectsCount = static_cast<UINT>(present_dirty.size());
                params.pDirtyRects = present_dirty.data();
                present_hr = swap_chain_->Present1(0, 0, &params);
            } else {
                present_hr = swap_chain_->Present1(0, 0, &params);
            }
            if (present_hr == DXGI_ERROR_INVALID_CALL) {
                DXGI_PRESENT_PARAMETERS full_params{};
                present_hr = swap_chain_->Present1(0, 0, &full_params);
            } else if (SUCCEEDED(present_hr) && !present_dirty.empty()) {
                metrics_.dirty_presents++;
            }
        } else {
            present_hr = swap_chain_->Present1(0, 0, &params);
        }

        if (SUCCEEDED(present_hr)) {
            has_presented_full_frame_ = true;
            metrics_.total_presents++;
            if (dcomp_device_ && metrics_.total_presents == 1) {
                dcomp_device_->Commit();
            }
        }
    }

    if (end_hr == D2DERR_RECREATE_TARGET || present_hr == DXGI_ERROR_DEVICE_REMOVED ||
        present_hr == DXGI_ERROR_DEVICE_RESET) {
        const HWND saved_hwnd = hwnd_;
        const uint32_t saved_w = width_;
        const uint32_t saved_h = height_;
        const bool saved_dcomp = using_dcomp_;
        InitializeForHwnd(saved_hwnd, saved_w, saved_h, saved_dcomp);
        return false;
    }

    full_dirty_ = false;
    dirty_rects_.clear();

    QueryPerformanceCounter(&t1);
    metrics_.last_draw_ms = QpcDeltaMs(t0, t1);
    return SUCCEEDED(present_hr);
}

// ---------- Desktop layer: one sparse virtual surface drawn in DIPs ----------
// The layer window covers the primary work area, but only the cards, icons and pills on it hold pixels: each
// frame draws the occupied (or dirty) regions through their own BeginDraw, and Trim() returns the rest.

void DesktopRenderer::SetSurfaceGeometry(uint32_t dpi, const model::RectI& monitor_px, const model::RectI& surface_px) {
    dpi_ = dpi ? dpi : 96u;
    dip_scale_ = dpi_ / 96.0f;
    monitor_px_ = monitor_px;
    surface_px_ = surface_px;
    if (!vsurf_ && swap_chain_ && hwnd_ && using_dcomp_) {
        BindSwapChainTargetBitmap();
    }
    ApplyBackdropMapping();
    InvalidateAll();
}

uint32_t DesktopRenderer::BackdropWidth() const noexcept {
    return monitor_px_.w > 0 ? static_cast<uint32_t>(monitor_px_.w) : width_;
}

uint32_t DesktopRenderer::BackdropHeight() const noexcept {
    return monitor_px_.h > 0 ? static_cast<uint32_t>(monitor_px_.h) : height_;
}

void DesktopRenderer::ApplyBackdropMapping() noexcept {
    backdrop_.SetMonitorScreenOrigin({monitor_px_.x, monitor_px_.y});
    backdrop_.SetDipMapping(dip_scale_, static_cast<float>(surface_px_.x - monitor_px_.x),
                            static_cast<float>(surface_px_.y - monitor_px_.y));
}

bool DesktopRenderer::CreateVirtualSurfaceResources(HWND hwnd, uint32_t width, uint32_t height) {
    auto cleanup = [&]() noexcept {
        glass_vsurf_.Reset();
        glass_visual_.Reset();
        vsurf_.Reset();
        dcomp_visual_.Reset();
        dcomp_root_visual_.Reset();
        dcomp_target_.Reset();
        dcomp_device_.Reset();
    };
    if (shared_dcomp_) {
        dcomp_device_ = shared_dcomp_;
    } else if (FAILED(DCompositionCreateDevice(dxgi_device_.Get(), IID_PPV_ARGS(&dcomp_device_))) || !dcomp_device_) {
        cleanup();
        return false;
    }
    if (FAILED(dcomp_device_->CreateTargetForHwnd(hwnd, TRUE, &dcomp_target_)) ||
        FAILED(dcomp_device_->CreateVisual(&dcomp_root_visual_)) ||
        FAILED(dcomp_device_->CreateVisual(&glass_visual_)) ||
        FAILED(dcomp_device_->CreateVisual(&dcomp_visual_)) ||
        FAILED(dcomp_device_->CreateVirtualSurface(width, height, DXGI_FORMAT_B8G8R8A8_UNORM,
                                                   DXGI_ALPHA_MODE_PREMULTIPLIED, &vsurf_)) ||
        !vsurf_) {
        cleanup();
        return false;
    }
    dcomp_visual_->SetContent(vsurf_.Get());
    // Visual order: glass_visual_ (bottom, lazy virtual surface) -> dcomp_visual_ (top, icons & text).
    if (FAILED(dcomp_root_visual_->AddVisual(glass_visual_.Get(), FALSE, nullptr)) ||
        FAILED(dcomp_root_visual_->AddVisual(dcomp_visual_.Get(), TRUE, glass_visual_.Get())) ||
        FAILED(dcomp_target_->SetRoot(dcomp_root_visual_.Get()))) {
        cleanup();
        return false;
    }
    using_dcomp_ = true;
    // DIPs: the scene is laid out at 96 DPI and drawn at the monitor's DPI.
    d2d_dc_->SetUnitMode(D2D1_UNIT_MODE_DIPS);
    d2d_dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    if (FAILED(dcomp_device_->Commit())) {
        cleanup();
        using_dcomp_ = false;
        return false;
    }
    return true;
}

bool DesktopRenderer::EnsureGlassVirtualSurface() {
    if (glass_vsurf_) return true;
    if (!dcomp_device_ || !glass_visual_ || width_ == 0 || height_ == 0) return false;
    if (FAILED(dcomp_device_->CreateVirtualSurface(width_, height_, DXGI_FORMAT_B8G8R8A8_UNORM,
                                                   DXGI_ALPHA_MODE_PREMULTIPLIED, &glass_vsurf_)) ||
        !glass_vsurf_) {
        glass_vsurf_.Reset();
        return false;
    }
    if (FAILED(glass_visual_->SetContent(glass_vsurf_.Get()))) {
        glass_vsurf_.Reset();
        return false;
    }
    last_glass_trimmed_occupied_.clear();
    return true;
}

void DesktopRenderer::ConfigureLiveWallpaperNotification(HWND notify_hwnd, UINT frame_msg) noexcept {
    live_wallpaper_.ConfigureNotification(notify_hwnd, frame_msg);
}

void DesktopRenderer::SetLiveWallpaperDecision(const host::LiveWallpaperDecision& decision) noexcept {
    if (decision == live_decision_) return;
    live_decision_ = decision;
    glass_only_dirty_ = true;
    full_dirty_ = true;
}

bool DesktopRenderer::OnLiveWallpaperFrameNotification(bool paused) noexcept {
    if (!d3d_device_ || !d2d_dc_) return false;
    std::vector<RECT> card_rects_px;
    card_rects_px.reserve(committed_glass_cards_.size());
    bool any_frost = false;
    for (const auto& c : committed_glass_cards_) {
        if (c.req.frost_percent > 6 || c.req.energy_saver) any_frost = true;
        const int pad_dip = std::max<int>(36, static_cast<int>(c.req.thickness_dip) + 12);
        card_rects_px.push_back(DipRectToPx(model::RectI{
            static_cast<int>(std::floor(c.req.left_dip)),
            static_cast<int>(std::floor(c.req.top_dip)),
            static_cast<int>(std::ceil(c.req.right_dip - c.req.left_dip)),
            static_cast<int>(std::ceil(c.req.bottom_dip - c.req.top_dip))}, pad_dip));
    }
    const RECT surf_scr{
        surface_px_.x, surface_px_.y,
        surface_px_.x + (surface_px_.w > 0 ? surface_px_.w : static_cast<int>(width_)),
        surface_px_.y + (surface_px_.h > 0 ? surface_px_.h : static_cast<int>(height_))};
    live_wallpaper_.SyncState(d3d_device_.Get(), d2d_dc_.Get(), live_decision_, surf_scr,
                              current_glass_live_rate_, !committed_glass_cards_.empty(), paused);
    if (live_wallpaper_.ConsumePendingFrame(d3d_device_.Get(), d2d_dc_.Get(), any_frost, card_rects_px)) {
        glass_only_dirty_ = true;
        return true;
    }
    return false;
}

void DesktopRenderer::UpdateHoverLight(const model::DesktopModelState& state,
                                       std::span<const model::GroupComputedView> group_views,
                                       const model::RectI& tile_board_rect,
                                       const DesktopInteractionState& interaction) noexcept {
    std::string active_id;
    D2D1_RECT_F active_rc{};
    if (interaction.cursor_inside && interaction.transparency_enabled && !energy_saver_cached_) {
        if (!interaction.hovered_group_id.empty()) {
            for (size_t i = 0; i < state.groups.size() && i < group_views.size(); ++i) {
                if (state.groups[i].id == interaction.hovered_group_id) {
                    if (ResolveMaterial(state.groups[i].material, state.settings, interaction) ==
                        model::CardMaterial::LiquidGlass) {
                        model::RectI f = group_views[i].frame;
                        if (!group_views[i].expanded) f.h = model::kGroupHeaderHeight;
                        active_id = "grp:" + state.groups[i].id;
                        active_rc = D2D1::RectF(static_cast<float>(f.x), static_cast<float>(f.y),
                                                static_cast<float>(f.right()), static_cast<float>(f.bottom()));
                    }
                    break;
                }
            }
        } else if (!interaction.hovered_tile_id.empty()) {
            for (const auto& t : state.tiles) {
                if (t.id == interaction.hovered_tile_id) {
                    if (ResolveMaterial(t.material, state.settings, interaction) ==
                        model::CardMaterial::LiquidGlass) {
                        const model::RectI tr = ComputeTileRectInline(tile_board_rect, t);
                        active_id = "tile:" + t.id;
                        active_rc = D2D1::RectF(static_cast<float>(tr.x), static_cast<float>(tr.y),
                                                static_cast<float>(tr.right()), static_cast<float>(tr.bottom()));
                    }
                    break;
                }
            }
        }
    }

    if (!active_id.empty()) {
        if (hover_light_card_id_ != active_id) {
            // Dirty previous hovered glass card so it returns to default -45 deg light.
            if (!hover_light_card_id_.empty()) {
                glass_dirty_rects_.push_back(RECT{
                    static_cast<LONG>(std::floor(hover_light_card_rect_.left)) - 24,
                    static_cast<LONG>(std::floor(hover_light_card_rect_.top)) - 24,
                    static_cast<LONG>(std::ceil(hover_light_card_rect_.right)) + 24,
                    static_cast<LONG>(std::ceil(hover_light_card_rect_.bottom)) + 24});
            }
            hover_light_card_id_ = active_id;
        }
        hover_light_card_rect_ = active_rc;
        hover_light_.SetCursorOnCard(
            static_cast<float>(interaction.cursor_pos.x),
            static_cast<float>(interaction.cursor_pos.y),
            active_rc.left, active_rc.top, active_rc.right, active_rc.bottom);
    } else if (!hover_light_card_id_.empty()) {
        hover_light_.SetDefaultTarget();
    }

    const uint64_t now = GetTickCount64();
    const float dt = (last_hover_step_ms_ == 0)
                         ? 16.0f
                         : static_cast<float>(std::min<uint64_t>(now - last_hover_step_ms_, 100ull));
    last_hover_step_ms_ = now;
    if (hover_light_.Step(dt)) {
        if (!hover_light_card_id_.empty()) {
            glass_dirty_rects_.push_back(RECT{
                static_cast<LONG>(std::floor(hover_light_card_rect_.left)) - 24,
                static_cast<LONG>(std::floor(hover_light_card_rect_.top)) - 24,
                static_cast<LONG>(std::ceil(hover_light_card_rect_.right)) + 24,
                static_cast<LONG>(std::ceil(hover_light_card_rect_.bottom)) + 24});
            if (active_id.empty() && !hover_light_.animating) {
                hover_light_card_id_.clear();
            }
        }
    }
}

namespace {

// Pixel rectangles merged onto a coarse grid so a few hundred icons cost a few dozen BeginDraw calls.
std::vector<RECT> CoalesceToGrid(const std::vector<RECT>& rects, LONG width, LONG height) {
    constexpr LONG kCell = 128;
    const LONG cols = (width + kCell - 1) / kCell, rows = (height + kCell - 1) / kCell;
    if (cols <= 0 || rows <= 0) return {};
    std::vector<uint8_t> grid(static_cast<size_t>(cols * rows), 0);
    for (const RECT& r : rects) {
        const LONG l = std::max(0L, r.left) / kCell, t = std::max(0L, r.top) / kCell;
        const LONG rr = std::min(width, r.right), bb = std::min(height, r.bottom);
        if (rr <= 0 || bb <= 0 || rr <= r.left || bb <= r.top) continue;
        const LONG cr = (rr - 1) / kCell, cb = (bb - 1) / kCell;
        for (LONG y = t; y <= cb; ++y) {
            for (LONG x = l; x <= cr; ++x) grid[static_cast<size_t>(y * cols + x)] = 1;
        }
    }
    std::vector<RECT> out;
    for (LONG y = 0; y < rows; ++y) {
        LONG x = 0;
        while (x < cols) {
            if (!grid[static_cast<size_t>(y * cols + x)]) { ++x; continue; }
            const LONG start = x;
            while (x < cols && grid[static_cast<size_t>(y * cols + x)]) ++x;
            RECT r{start * kCell, y * kCell, std::min(width, x * kCell), std::min(height, (y + 1) * kCell)};
            // Extend downwards while the next rows hold exactly the same run.
            if (!out.empty() && out.back().left == r.left && out.back().right == r.right && out.back().bottom == r.top) {
                out.back().bottom = r.bottom;
            } else {
                out.push_back(r);
            }
        }
    }
    return out;
}

} // namespace

RECT DesktopRenderer::DipRectToPx(const model::RectI& rc, int margin_dip) const noexcept {
    const float s = dip_scale_;
    return RECT{static_cast<LONG>(std::floor((rc.x - margin_dip) * s)), static_cast<LONG>(std::floor((rc.y - margin_dip) * s)),
                static_cast<LONG>(std::ceil((rc.right() + margin_dip) * s)), static_cast<LONG>(std::ceil((rc.bottom() + margin_dip) * s))};
}

bool DesktopRenderer::IntersectsActiveClip(const model::RectI& rc, int margin_dip) const noexcept {
    if (rc.empty()) return false;
    return static_cast<float>(rc.right() + margin_dip) > active_clip_dip_.left &&
           static_cast<float>(rc.x - margin_dip) < active_clip_dip_.right &&
           static_cast<float>(rc.bottom() + margin_dip) > active_clip_dip_.top &&
           static_cast<float>(rc.y - margin_dip) < active_clip_dip_.bottom;
}

// Every rectangle DrawScene can paint this frame, in surface pixels with room for shadows.
std::vector<RECT> DesktopRenderer::CollectOccupiedRects(const model::DesktopModelState& state,
                                                        std::span<const model::GroupComputedView> group_views,
                                                        const model::RectI& tile_board_rect,
                                                        const DesktopInteractionState& interaction) const {
    std::vector<RECT> out;
    constexpr int kShadow = 16;
    const bool focus_scene = (state.settings.active_scene == model::DesktopScene::Focus);
    const bool present_scene = (state.settings.active_scene == model::DesktopScene::Present);
    if (!state.settings.icons_hidden && !present_scene) {
        const model::PointI cell_sz = model::IconCellMetrics(state.settings.icon_size);
        const int cw = std::max(cell_sz.x, model::kLooseCellW), ch = std::max(cell_sz.y, model::kLooseCellH);
        std::vector<uint8_t> grouped(state.items.size(), 0);
        if (state.settings.organize_enabled) {
            for (const auto& gv : group_views) {
                for (size_t idx : gv.member_item_indices) if (idx < grouped.size()) grouped[idx] = 1;
            }
            for (size_t i = 0; i < state.items.size(); ++i) {
                const auto& it = state.items[i];
                if (it.hidden_by_mapped_group || !it.mapped_owner_group_id.empty() ||
                    it.in_apps_tile || (!it.force_loose && !it.pinned_group_id.empty() &&
                                        it.pinned_group_id != model::kAppsTileGroupId)) grouped[i] = 1;
            }
        } else {
            for (size_t i = 0; i < state.items.size(); ++i) {
                const auto& it = state.items[i];
                if (it.hidden_by_mapped_group || !it.mapped_owner_group_id.empty()) grouped[i] = 1;
            }
        }
        if (!(state.settings.organize_enabled && focus_scene)) {
            for (size_t i = 0; i < state.items.size(); ++i) {
                if (grouped[i]) continue;
                const auto& it = state.items[i];
                out.push_back(DipRectToPx(model::RectI{it.loose_pos.x, it.loose_pos.y, cw, ch}, 4));
            }
        }
        if (state.settings.organize_enabled) {
            for (size_t i = 0; i < state.groups.size() && i < group_views.size(); ++i) {
                model::RectI f = group_views[i].frame;
                if (f.empty()) continue;
                if (!group_views[i].expanded) f.h = model::kGroupHeaderHeight;
                out.push_back(DipRectToPx(f, kShadow));
            }
        }
    }
    if (state.settings.tiles_enabled && state.settings.organize_enabled && !interaction.side_drawer_open) {
        const model::RectI wa_fallback = !state.monitors.empty()
            ? state.monitors.front().work_area
            : model::RectI{0, 0, static_cast<int>(width_ / dip_scale_), static_cast<int>(height_ / dip_scale_)};
        const model::RectI hdr_anchor = !tile_board_rect.empty() ? tile_board_rect
                                                                 : ComputeBoardRectFallback(wa_fallback, true);
        if (!hdr_anchor.empty()) {
            out.push_back(DipRectToPx(model::RectI{hdr_anchor.x, hdr_anchor.y - 34, hdr_anchor.w, 30}, 4));
        }
        for (const auto& tile : state.tiles) {
            out.push_back(DipRectToPx(ComputeTileRectInline(tile_board_rect, tile), kShadow));
        }
        const bool any_free = std::any_of(state.tiles.begin(), state.tiles.end(),
                                          [](const model::DesktopTile& t) { return t.free_placed; });
        if (interaction.tile_edit_mode && !any_free && !tile_board_rect.empty()) {
            out.push_back(DipRectToPx(tile_board_rect, kShadow));
        }
        if (interaction.dragging_tile && !interaction.drag_tile_preview_rect.empty()) {
            out.push_back(DipRectToPx(interaction.drag_tile_preview_rect, 4));
        }
    }
    if (interaction.marquee_active && !interaction.marquee_rect.empty()) out.push_back(DipRectToPx(interaction.marquee_rect, 2));
    if (interaction.dragging_items && interaction.drag_item_count > 0) {
        out.push_back(DipRectToPx(model::RectI{interaction.drag_cursor_pos.x + 12, interaction.drag_cursor_pos.y + 4, 74, 74}, kShadow));
    }
    if (interaction.drop_hint_visible && !interaction.drop_hint_label.empty()) {
        out.push_back(DipRectToPx(model::RectI{interaction.drop_hint_pos.x + 16, interaction.drop_hint_pos.y + 18, 400, 30}, kShadow));
    }
    const bool banner = !interaction.banner_suppressed &&
                        (focus_scene || present_scene || state.settings.icons_hidden);
    if (banner) {
        const model::RectI wa = !state.monitors.empty() ? state.monitors.front().work_area
                                                        : model::RectI{0, 0, static_cast<int>(width_ / dip_scale_),
                                                                       static_cast<int>(height_ / dip_scale_)};
        out.push_back(DipRectToPx(model::RectI{wa.x + 24, wa.bottom() - 56, 820, 36}, kShadow));
    }
    return out;
}

namespace {
void ApplyBeautifyGlassOptics(GlassCardRequest& req,
                              const model::BeautifySettings& settings,
                              bool cursor_active,
                              const HoverLightTracker& hover_light) noexcept {
    req.glass_options = settings.glass_options;
    req.reference_mode = settings.glass_reference;
    req.blur_radius = settings.glass_blur_radius;
    req.blur_edge = settings.glass_blur_edge;
    req.thickness_dip = static_cast<float>(std::clamp<uint8_t>(settings.glass_thickness, 1u, 80u));
    req.ior = static_cast<float>(std::clamp<uint16_t>(settings.glass_ior_x100, 100u, 400u)) * 0.01f;
    req.dispersion_gain = static_cast<float>(std::min<uint8_t>(settings.glass_dispersion, 50u));
    req.glare_percent = std::min<uint8_t>(settings.glass_glare, 120u);
    req.glare_angle_deg = static_cast<float>(std::clamp<int16_t>(settings.glass_glare_angle, -180, 180));
    req.frost_percent = std::min<uint8_t>(settings.glass_frost, 100u);
    req.opacity_percent = settings.card_opacity_percent;
    req.dark_mode = settings.dark_mode;
    req.cursor_light_active = cursor_active;
    if (cursor_active) {
        req.light_dir_x = hover_light.current_x;
        req.light_dir_y = hover_light.current_y;
    }
}

bool GlassRequestsDiffer(const GlassCardRequest& a, const GlassCardRequest& b) noexcept {
    return a.reference_mode != b.reference_mode ||
           (a.reference_mode && a.glass_options != b.glass_options) ||
           std::fabs(a.left_dip - b.left_dip) > 0.1f ||
           std::fabs(a.top_dip - b.top_dip) > 0.1f ||
           std::fabs(a.right_dip - b.right_dip) > 0.1f ||
           std::fabs(a.bottom_dip - b.bottom_dip) > 0.1f ||
           std::fabs(a.thickness_dip - b.thickness_dip) > 0.1f ||
           std::fabs(a.ior - b.ior) > 0.002f ||
           std::fabs(a.dispersion_gain - b.dispersion_gain) > 0.1f ||
           a.glare_percent != b.glare_percent ||
           std::fabs(a.glare_angle_deg - b.glare_angle_deg) > 0.25f ||
           a.frost_percent != b.frost_percent ||
           a.opacity_percent != b.opacity_percent ||
           a.dark_mode != b.dark_mode ||
           a.elevated_hover != b.elevated_hover ||
           a.cursor_light_active != b.cursor_light_active ||
           (a.cursor_light_active && (std::fabs(a.light_dir_x - b.light_dir_x) > 0.005f ||
                                      std::fabs(a.light_dir_y - b.light_dir_y) > 0.005f)) ||
           std::fabs(a.press_amount - b.press_amount) > 0.002f ||
           std::fabs(a.background_opacity - b.background_opacity) > 0.002f;
}
} // namespace

bool DesktopRenderer::DrawSingleLiquidGlassCard(ID2D1DeviceContext* dc,
                                                const GlassCardRequest& req,
                                                float shadow_strength) {
    if (!dc || !d2d_factory_ || glass_pool_.Unavailable()) return false;

    const float fade = std::clamp(static_cast<float>(req.opacity_percent) / 30.0f, 0.0f, 1.0f);
    if (fade <= 0.0f || req.background_opacity <= 1e-4f) {
        return true;
    }

    const bool use_live = live_wallpaper_.HasFrame() &&
                          (live_decision_.active || live_wallpaper_.Stats().paused);
    ID2D1Bitmap1* sharp_bmp = nullptr;
    ID2D1Bitmap1* frost_bmp = nullptr;
    uint32_t sharp_w = 0, sharp_h = 0, frost_w = 0, frost_h = 0;
    uint32_t logical_w = BackdropWidth(), logical_h = BackdropHeight();
    float orig_x = backdrop_.OriginX(), orig_y = backdrop_.OriginY();

    if (use_live) {
        sharp_bmp = live_wallpaper_.SharpBitmap();
        sharp_w = live_wallpaper_.SharpWidth();
        sharp_h = live_wallpaper_.SharpHeight();
        frost_bmp = live_wallpaper_.FrostBitmap();
        frost_w = live_wallpaper_.FrostWidth();
        frost_h = live_wallpaper_.FrostHeight();
        logical_w = live_wallpaper_.LogicalScreenWidth();
        logical_h = live_wallpaper_.LogicalScreenHeight();
        orig_x = 0.0f;
        orig_y = 0.0f;
    } else {
        const bool sync_decode = (!vsurf_ && hwnd_ == nullptr) || force_inline_glass_;
        sharp_bmp = backdrop_.GlassSharpBitmap(dc, sync_decode, sharp_w, sharp_h);
        if (req.frost_percent > 6 || req.energy_saver) {
            frost_bmp = backdrop_.GlassFrostBitmap(dc, frost_w, frost_h);
        }
    }
    if (!sharp_bmp || sharp_w == 0 || sharp_h == 0) {
        return false;
    }

    D2D1_MATRIX_3X2_F prev_xf{};
    dc->GetTransform(&prev_xf);
    const D2D1_UNIT_MODE prev_unit = dc->GetUnitMode();
    const float unit_scale = (prev_unit == D2D1_UNIT_MODE_PIXELS) ? 1.0f : dip_scale_;

    // Map card DIP bounds through any axis-aligned scale/translation on `dc` (handles VirtualSurface
    // atlas shift, pressed-tile scale, and offscreen menu underlay capture uniformly).
    GlassCardRequest mapped = req;
    mapped.live_source = use_live;
    if (use_live && mapped.background_luminance < 0.0f) {
        mapped.background_luminance = live_wallpaper_.AverageLuminance();
    }
    const float sx = (std::fabs(prev_xf.m11) > 1e-4f) ? prev_xf.m11 : 1.0f;
    const float sy = (std::fabs(prev_xf.m22) > 1e-4f) ? prev_xf.m22 : 1.0f;
    const float atlas_shift_x = prev_xf.dx * unit_scale;
    const float atlas_shift_y = prev_xf.dy * unit_scale;
    if (std::fabs(sx - 1.0f) > 1e-4f || std::fabs(sy - 1.0f) > 1e-4f) {
        mapped.left_dip *= sx;
        mapped.right_dip *= sx;
        mapped.top_dip *= sy;
        mapped.bottom_dip *= sy;
        mapped.radius_dip *= std::min(sx, sy);
    }

    const GlassDrawPlan plan = MakeGlassDrawPlan(mapped, unit_scale, atlas_shift_x, atlas_shift_y);
    if (!plan.visible) return true;

    if (shadow_strength > 0.0f) {
        if (!card_shadow_bitmap_ && dc == d2d_dc_.Get()) {
            card_shadow_bitmap_ = CreateExteriorCardShadowBitmap(d2d_dc_.Get());
        }
        if (card_shadow_bitmap_ && dc == d2d_dc_.Get()) {
            const D2D1_RECT_F shadow_rc = D2D1::RectF(req.left_dip, req.top_dip, req.right_dip, req.bottom_dip);
            DrawExteriorCardShadow(dc, card_shadow_bitmap_.Get(), shadow_rc, req.radius_dip, shadow_strength);
        }
    }

    return glass_pool_.DrawCard(
        d2d_factory_.Get(), dc,
        sharp_bmp, sharp_w, sharp_h,
        frost_bmp, frost_w, frost_h,
        logical_w, logical_h,
        orig_x, orig_y,
        atlas_shift_x, atlas_shift_y,
        plan);
}

bool DesktopRenderer::SyncGlassVirtualSurface(bool force_all, const std::vector<RECT>& content_regions_px) {
    if (!vsurf_) return true;
    const LONG w = static_cast<LONG>(width_), h = static_cast<LONG>(height_);

    if (pending_glass_cards_.empty()) {
        if (glass_vsurf_ && !last_glass_trimmed_occupied_.empty()) {
            for (const RECT& r : last_glass_trimmed_occupied_) {
                POINT off{};
                ComPtr<IDXGISurface> surf;
                if (SUCCEEDED(glass_vsurf_->BeginDraw(&r, IID_PPV_ARGS(&surf), &off)) && surf) {
                    const D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
                        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                        static_cast<float>(dpi_), static_cast<float>(dpi_));
                    ComPtr<ID2D1Bitmap1> target;
                    if (SUCCEEDED(d2d_dc_->CreateBitmapFromDxgiSurface(surf.Get(), &bp, &target))) {
                        d2d_dc_->SetTarget(target.Get());
                        d2d_dc_->BeginDraw();
                        d2d_dc_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
                        d2d_dc_->EndDraw();
                        d2d_dc_->SetTarget(nullptr);
                    }
                    glass_vsurf_->EndDraw();
                }
            }
            glass_vsurf_->Trim(nullptr, 0);
            last_glass_trimmed_occupied_.clear();
        }
        committed_glass_cards_.clear();
        glass_only_dirty_ = false;
        glass_dirty_rects_.clear();
        return true;
    }

    if (!EnsureGlassVirtualSurface()) {
        return false;
    }

    // Build occupied regions for every active LiquidGlass card (expanded for shadow + fusion bridge).
    std::vector<RECT> raw_occupied;
    raw_occupied.reserve(pending_glass_cards_.size());
    for (const auto& c : pending_glass_cards_) {
        const int pad = std::max<int>(c.neighbor_cnt > 0 ? 48 : 32, static_cast<int>(c.req.thickness_dip) + 12);
        raw_occupied.push_back(DipRectToPx(model::RectI{
            static_cast<int>(std::floor(c.req.left_dip)),
            static_cast<int>(std::floor(c.req.top_dip)),
            static_cast<int>(std::ceil(c.req.right_dip - c.req.left_dip)),
            static_cast<int>(std::ceil(c.req.bottom_dip - c.req.top_dip))}, pad));
    }
    std::vector<RECT> glass_occupied = CoalesceToGrid(raw_occupied, w, h);

    bool geometry_changed = (pending_glass_cards_.size() != committed_glass_cards_.size());
    if (!geometry_changed) {
        for (size_t i = 0; i < pending_glass_cards_.size(); ++i) {
            if (pending_glass_cards_[i].id != committed_glass_cards_[i].id ||
                pending_glass_cards_[i].neighbor_cnt != committed_glass_cards_[i].neighbor_cnt ||
                GlassRequestsDiffer(pending_glass_cards_[i].req, committed_glass_cards_[i].req)) {
                geometry_changed = true;
                break;
            }
        }
    }

    std::vector<RECT> draw_regions;
    if (force_all || glass_only_dirty_ || geometry_changed) {
        draw_regions = glass_occupied;
        // Also clear any previously occupied tiles that cards moved away from!
        if (geometry_changed && !last_glass_trimmed_occupied_.empty()) {
            std::vector<RECT> combined = raw_occupied;
            combined.insert(combined.end(), last_glass_trimmed_occupied_.begin(), last_glass_trimmed_occupied_.end());
            draw_regions = CoalesceToGrid(combined, w, h);
        }
    } else if (!glass_dirty_rects_.empty() || !content_regions_px.empty()) {
        std::vector<RECT> px;
        for (const RECT& d : glass_dirty_rects_) {
            px.push_back(DipRectToPx(model::RectI{d.left, d.top, d.right - d.left, d.bottom - d.top}, 4));
        }
        for (const RECT& cr : content_regions_px) {
            for (const RECT& occ : glass_occupied) {
                RECT inter{};
                if (IntersectRect(&inter, &cr, &occ)) {
                    px.push_back(inter);
                }
            }
        }
        draw_regions = CoalesceToGrid(px, w, h);
    }

    if (draw_regions.empty()) {
        glass_only_dirty_ = false;
        glass_dirty_rects_.clear();
        return true;
    }

    const float s = dip_scale_;
    HRESULT hr = S_OK;
    glass_pool_.BeginFrame();
    for (const RECT& r : draw_regions) {
        POINT off{};
        ComPtr<IDXGISurface> surface;
        hr = glass_vsurf_->BeginDraw(&r, IID_PPV_ARGS(&surface), &off);
        if (FAILED(hr) || !surface) break;
        const D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            static_cast<float>(dpi_), static_cast<float>(dpi_));
        ComPtr<ID2D1Bitmap1> target;
        hr = d2d_dc_->CreateBitmapFromDxgiSurface(surface.Get(), &bp, &target);
        if (FAILED(hr)) {
            glass_vsurf_->EndDraw();
            break;
        }
        d2d_dc_->SetTarget(target.Get());
        d2d_dc_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
        d2d_dc_->BeginDraw();
        d2d_dc_->SetTransform(D2D1::Matrix3x2F::Translation((off.x - r.left) / s, (off.y - r.top) / s));
        const D2D1_RECT_F clip_dip = D2D1::RectF(r.left / s, r.top / s, r.right / s, r.bottom / s);
        d2d_dc_->PushAxisAlignedClip(clip_dip, D2D1_ANTIALIAS_MODE_ALIASED);
        d2d_dc_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

        for (auto& card : pending_glass_cards_) {
            const float pad = std::max(40.0f, card.req.thickness_dip + 16.0f);
            if (card.req.right_dip + pad <= clip_dip.left ||
                card.req.left_dip - pad >= clip_dip.right ||
                card.req.bottom_dip + pad <= clip_dip.top ||
                card.req.top_dip - pad >= clip_dip.bottom) {
                continue;
            }
            card.req.neighbors = std::span<const GlassNeighborDip>(card.neighbor_buf.data(), card.neighbor_cnt);
            if (!DrawSingleLiquidGlassCard(d2d_dc_.Get(), card.req, card.shadow_strength)) {
                // Hardware could not run ps_4_0 effect: force foreground fallback pass!
                full_dirty_ = true;
            }
        }

        d2d_dc_->PopAxisAlignedClip();
        d2d_dc_->SetTransform(D2D1::Matrix3x2F::Identity());
        hr = d2d_dc_->EndDraw();
        d2d_dc_->SetTarget(nullptr);
        glass_vsurf_->EndDraw();
        if (FAILED(hr)) break;
    }
    glass_pool_.EndFrame();

    if (SUCCEEDED(hr)) {
        const bool same_trim =
            glass_occupied.size() == last_glass_trimmed_occupied_.size() &&
            std::equal(glass_occupied.begin(), glass_occupied.end(), last_glass_trimmed_occupied_.begin(),
                       [](const RECT& a, const RECT& b) noexcept {
                           return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
                       });
        if (!same_trim) {
            glass_vsurf_->Trim(glass_occupied.empty() ? nullptr : glass_occupied.data(),
                               static_cast<UINT>(glass_occupied.size()));
            last_glass_trimmed_occupied_ = std::move(glass_occupied);
        }
        committed_glass_cards_ = pending_glass_cards_;
        glass_only_dirty_ = false;
        glass_dirty_rects_.clear();
    }
    return SUCCEEDED(hr);
}

bool DesktopRenderer::TickVirtual(const model::DesktopModelState& state,
                                  std::span<const model::GroupComputedView> group_views,
                                  const model::RectI& tile_board_rect,
                                  const LiveTileRuntimeData& live_data,
                                  const DesktopInteractionState& interaction) {
    UpdateHoverLight(state, group_views, tile_board_rect, interaction);

    // Build authoritative settled LiquidGlass card list independent of partial `active_clip_dip_` culling.
    pending_glass_cards_.clear();
    if (interaction.transparency_enabled && !glass_pool_.Unavailable()) {
        const bool focus_scene = (state.settings.active_scene == model::DesktopScene::Focus);
        const bool present_scene = (state.settings.active_scene == model::DesktopScene::Present);
        if (!state.settings.icons_hidden && !present_scene && state.settings.organize_enabled) {
            for (size_t i = 0; i < state.groups.size() && i < group_views.size(); ++i) {
                const auto& grp = state.groups[i];
                const auto& view = group_views[i];
                if (view.frame.empty()) continue;
                if (ResolveMaterial(grp.material, state.settings, interaction) != model::CardMaterial::LiquidGlass) {
                    continue;
                }
                model::RectI f = view.frame;
                if (!view.expanded) f.h = model::kGroupHeaderHeight;
                const D2D1_RECT_F rc = ToRectF(f);
                const float op_scale = (focus_scene && !grp.focus_visible) ? 0.40f : 1.0f;
                const float fade = std::clamp(static_cast<float>(state.settings.card_opacity_percent) / 30.0f, 0.0f, 1.0f);
                const bool hov = (interaction.hovered_group_id == grp.id);
                const bool drop = (interaction.drop_target_group_id == grp.id);

                PendingGlassCard pc{};
                pc.id = "grp:" + grp.id;
                pc.is_group = true;
                pc.req.left_dip = rc.left;
                pc.req.top_dip = rc.top;
                pc.req.right_dip = rc.right;
                pc.req.bottom_dip = rc.bottom;
                pc.req.radius_dip = 14.0f;
                ApplyBeautifyGlassOptics(pc.req, state.settings, hover_light_card_id_ == pc.id, hover_light_);
                pc.req.elevated_hover = hov;
                pc.req.allow_clear_passthrough = !interaction.force_opaque_wallpaper;
                pc.req.energy_saver = energy_saver_cached_;
                pc.req.draw_rim_in_shader = !drop;
                pc.req.background_luminance = backdrop_.AverageLuminance(rc);
                pc.req.background_opacity = op_scale;
                pc.shadow_strength = (state.settings.dark_mode ? (hov ? 0.65f : 0.54f) : (hov ? 0.46f : 0.32f)) * fade * op_scale;
                pending_glass_cards_.push_back(std::move(pc));
            }
        }
        if (state.settings.tiles_enabled && state.settings.organize_enabled && !interaction.side_drawer_open) {
            for (const auto& tile : state.tiles) {
                if (ResolveMaterial(tile.material, state.settings, interaction) != model::CardMaterial::LiquidGlass) {
                    continue;
                }
                const bool active_drag = (interaction.dragging_tile && interaction.dragging_tile_id == tile.id);
                const bool jiggle = interaction.tile_edit_mode && !active_drag && (interaction.resizing_tile_id != tile.id);
                // Dragged and jiggling tiles render inline on `vsurf_` for proper foreground overlap / rotation.
                if (active_drag || jiggle) continue;

                model::RectI tr = ComputeTileRectInline(tile_board_rect, tile);
                if (tr.empty()) continue;
                D2D1_RECT_F rc = ToRectF(tr);
                const bool pressed = (interaction.pressed_tile_id == tile.id && std::abs(interaction.tile_press) > 0.001f);
                if (pressed) {
                    const float shrink = 5.0f * interaction.tile_press;
                    rc.left += shrink; rc.right -= shrink;
                    rc.top += shrink; rc.bottom -= shrink;
                }
                const float tile_op = (focus_scene && !IsFocusKeepTileKind(model::ActiveTileView(tile).kind)) ? 0.38f : 1.0f;
                const float fade = std::clamp(static_cast<float>(state.settings.card_opacity_percent) / 30.0f, 0.0f, 1.0f);
                const bool hov = (interaction.hovered_tile_id == tile.id);
                const bool drop = (interaction.drop_target_tile_id == tile.id ||
                                   interaction.stack_drop_target_tile_id == tile.id);

                PendingGlassCard pc{};
                pc.id = "tile:" + tile.id;
                pc.is_group = false;
                pc.req.left_dip = rc.left;
                pc.req.top_dip = rc.top;
                pc.req.right_dip = rc.right;
                pc.req.bottom_dip = rc.bottom;
                pc.req.radius_dip = 16.0f;
                ApplyBeautifyGlassOptics(pc.req, state.settings, hover_light_card_id_ == pc.id, hover_light_);
                pc.req.elevated_hover = hov;
                pc.req.allow_clear_passthrough = !interaction.force_opaque_wallpaper;
                pc.req.energy_saver = energy_saver_cached_;
                pc.req.draw_rim_in_shader = !drop;
                pc.req.background_luminance = backdrop_.AverageLuminance(rc);
                pc.req.background_opacity = tile_op;
                pc.req.press_amount = pressed ? interaction.tile_press : 0.0f;
                pc.shadow_strength = (state.settings.dark_mode ? (hov ? 0.65f : 0.54f) : (hov ? 0.46f : 0.32f)) * fade * tile_op;
                pending_glass_cards_.push_back(std::move(pc));
            }
        }

        // Smooth-min liquid fusion:
        // 1) Group cards manually placed within <= 18 DIP of another LiquidGlass group card fuse smoothly.
        // 2) While dragging/resizing a tile, nearby settled tiles reach out toward `drag_tile_preview_rect`!
        for (size_t i = 0; i < pending_glass_cards_.size(); ++i) {
            auto& ci = pending_glass_cards_[i];
            if (ci.is_group) {
                for (size_t j = 0; j < pending_glass_cards_.size() && ci.neighbor_cnt < 4; ++j) {
                    if (i == j || !pending_glass_cards_[j].is_group) continue;
                    const auto& cj = pending_glass_cards_[j].req;
                    const float gx = std::max({0.0f, ci.req.left_dip - cj.right_dip, cj.left_dip - ci.req.right_dip});
                    const float gy = std::max({0.0f, ci.req.top_dip - cj.bottom_dip, cj.top_dip - ci.req.bottom_dip});
                    if (std::hypot(gx, gy) <= 18.0f) {
                        ci.neighbor_buf[ci.neighbor_cnt++] = {cj.left_dip, cj.top_dip, cj.right_dip, cj.bottom_dip, cj.radius_dip};
                    }
                }
            } else if (interaction.dragging_tile && !interaction.drag_tile_preview_rect.empty() && ci.neighbor_cnt < 4) {
                const auto& dr = interaction.drag_tile_preview_rect;
                const float dl = static_cast<float>(dr.x), dt = static_cast<float>(dr.y);
                const float drt = static_cast<float>(dr.right()), db = static_cast<float>(dr.bottom());
                const float gx = std::max({0.0f, ci.req.left_dip - drt, dl - ci.req.right_dip});
                const float gy = std::max({0.0f, ci.req.top_dip - db, dt - ci.req.bottom_dip});
                if (std::hypot(gx, gy) <= 28.0f) {
                    ci.neighbor_buf[ci.neighbor_cnt++] = {dl, dt, drt, db, 16.0f};
                }
            }
        }
    }

    // Sync live dynamic wallpaper state & evict static glass source 5s after switching away.
    const uint64_t now_tick = GetTickCount64();
    if (!pending_glass_cards_.empty()) {
        last_glass_used_ms_ = now_tick;
    } else if (last_glass_used_ms_ != 0 && (now_tick - last_glass_used_ms_ > 5000ull)) {
        backdrop_.ReleaseGlassSource();
        last_glass_used_ms_ = 0;
    }
    const bool live_paused = ShouldPauseLiveActivity(state.settings.pause_on_fullscreen_or_battery);
    (void)OnLiveWallpaperFrameNotification(live_paused);

    const bool content_dirty = full_dirty_ || !dirty_rects_.empty() || animating_;
    bool glass_dirty = glass_only_dirty_ || !glass_dirty_rects_.empty() ||
                       (pending_glass_cards_.size() != committed_glass_cards_.size());
    if (!glass_dirty && !content_dirty) {
        for (size_t i = 0; i < pending_glass_cards_.size(); ++i) {
            if (pending_glass_cards_[i].id != committed_glass_cards_[i].id ||
                pending_glass_cards_[i].neighbor_cnt != committed_glass_cards_[i].neighbor_cnt ||
                GlassRequestsDiffer(pending_glass_cards_[i].req, committed_glass_cards_[i].req)) {
                glass_dirty = true;
                break;
            }
        }
    }

    if (!content_dirty && !glass_dirty) {
        metrics_.idle_ticks_skipped++;
        return false;
    }

    // Fast Glass-Only Path: live wallpaper frame or cursor specular light moved while icons & text are unchanged!
    // Performs ZERO `vsurf_->BeginDraw` and ZERO `DrawScene` text/icon work!
    if (!content_dirty && glass_dirty) {
        LARGE_INTEGER gt0{}, gt1{};
        QueryPerformanceCounter(&gt0);
        const std::vector<RECT> empty_content;
        bool g_ok = SyncGlassVirtualSurface(false, empty_content);
        HRESULT g_hr = g_ok ? dcomp_device_->Commit() : E_FAIL;
        if (g_hr == D2DERR_RECREATE_TARGET || g_hr == DXGI_ERROR_DEVICE_REMOVED || g_hr == DXGI_ERROR_DEVICE_RESET) {
            const HWND saved_hwnd = hwnd_;
            const uint32_t saved_w = width_, saved_h = height_;
            InitializeForHwnd(saved_hwnd, saved_w, saved_h, true);
            return false;
        }
        metrics_.glass_only_presents++;
        metrics_.total_presents++;
        QueryPerformanceCounter(&gt1);
        metrics_.last_draw_ms = QpcDeltaMs(gt0, gt1);
        return SUCCEEDED(g_hr);
    }

    native_acrylic_allowed_ = interaction.transparency_enabled;
    host_backdrop_.BeginFrame();
    glass_pool_.BeginFrame();
    LARGE_INTEGER t0{}, t1{};
    QueryPerformanceCounter(&t0);

    const LONG w = static_cast<LONG>(width_), h = static_cast<LONG>(height_);
    // Native acrylic EndFrame replaces the complete card visual list. A clipped
    // foreground pass would otherwise remove every undrawn card's backdrop.
    const bool full = full_dirty_ || animating_ ||
                      (host_backdrop_.Active() && native_acrylic_allowed_);
    std::vector<RECT> occupied;
    std::vector<RECT> regions;
    if (full) {
        occupied = CoalesceToGrid(CollectOccupiedRects(state, group_views, tile_board_rect, interaction), w, h);
        regions = occupied;
    } else {
        // Dirty rectangles arrive in scene DIPs.
        std::vector<RECT> px;
        for (const RECT& d : dirty_rects_) {
            px.push_back(DipRectToPx(model::RectI{d.left, d.top, d.right - d.left, d.bottom - d.top}, 2));
        }
        regions = CoalesceToGrid(px, w, h);
    }

    HRESULT hr = S_OK;
    const float s = dip_scale_;
    for (const RECT& r : regions) {
        POINT off{};
        ComPtr<IDXGISurface> surface;
        hr = vsurf_->BeginDraw(&r, IID_PPV_ARGS(&surface), &off);
        if (FAILED(hr) || !surface) break;
        const D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            static_cast<float>(dpi_), static_cast<float>(dpi_));
        ComPtr<ID2D1Bitmap1> target;
        hr = d2d_dc_->CreateBitmapFromDxgiSurface(surface.Get(), &bp, &target);
        if (FAILED(hr)) {
            vsurf_->EndDraw();
            break;
        }
        d2d_dc_->SetTarget(target.Get());
        d2d_dc_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
        d2d_dc_->BeginDraw();
        // Scene DIP p lands at surface pixel off + p * s - r.origin.
        d2d_dc_->SetTransform(D2D1::Matrix3x2F::Translation((off.x - r.left) / s, (off.y - r.top) / s));
        active_clip_dip_ = D2D1::RectF(r.left / s, r.top / s, r.right / s, r.bottom / s);
        d2d_dc_->PushAxisAlignedClip(active_clip_dip_, D2D1_ANTIALIAS_MODE_ALIASED);
        d2d_dc_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        DrawScene(state, group_views, tile_board_rect, live_data, interaction);
        active_clip_dip_ = D2D1::RectF(0.0f, 0.0f, 1e6f, 1e6f);
        d2d_dc_->PopAxisAlignedClip();
        d2d_dc_->SetTransform(D2D1::Matrix3x2F::Identity());
        hr = d2d_dc_->EndDraw();
        d2d_dc_->SetTarget(nullptr);
        vsurf_->EndDraw();
        if (FAILED(hr)) break;
    }
    if (SUCCEEDED(hr) && full) {
        const bool same_trim =
            occupied.size() == last_trimmed_occupied_.size() &&
            std::equal(occupied.begin(), occupied.end(), last_trimmed_occupied_.begin(),
                       [](const RECT& a, const RECT& b) noexcept {
                           return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
                       });
        if (!same_trim) {
            // Pixels outside what was just drawn belong to nothing now: let DirectComposition reclaim them.
            vsurf_->Trim(occupied.empty() ? nullptr : occupied.data(), static_cast<UINT>(occupied.size()));
            last_trimmed_occupied_ = std::move(occupied);
        }
    }
    glass_pool_.EndFrame();
    if (SUCCEEDED(hr)) {
        (void)SyncGlassVirtualSurface(full_dirty_, regions);
    }
    const bool had_native_acrylic = host_backdrop_.Active();
    host_backdrop_.EndFrame();
    const bool acrylic_failed = had_native_acrylic && !host_backdrop_.Active();
    if (SUCCEEDED(hr)) hr = dcomp_device_->Commit();

    if (hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        const HWND saved_hwnd = hwnd_;
        const uint32_t saved_w = width_, saved_h = height_;
        InitializeForHwnd(saved_hwnd, saved_w, saved_h, true);
        return false;
    }
    full_dirty_ = acrylic_failed;
    dirty_rects_.clear();
    metrics_.total_presents++;
    QueryPerformanceCounter(&t1);
    metrics_.last_draw_ms = QpcDeltaMs(t0, t1);
    return SUCCEEDED(hr);
}

bool DesktopRenderer::RenderCompositePng(
    const std::wstring& png_path,
    const model::DesktopModelState& state,
    std::span<const model::GroupComputedView> group_views,
    const model::RectI& tile_board_rect,
    const LiveTileRuntimeData& live_data,
    const DesktopInteractionState& interaction,
    const std::function<void(ID2D1DeviceContext*)>& post_overlay_draw) {
    if (!d2d_dc_ || !swap_chain_ || !d3d_device_ || !d3d_context_ || png_path.empty()) return false;

    backdrop_.LoadForScreen(d2d_dc_.Get(), BackdropWidth(), BackdropHeight(),
                            state.settings.wallpaper_preset, state.settings.dark_mode);
    ApplyBackdropMapping();

    current_glass_settings_ = state.settings;
    current_glass_live_rate_ = state.settings.glass_live_rate;
    DesktopInteractionState snap_inter = interaction;
    snap_inter.force_opaque_wallpaper = true;

    const bool prev_force = force_inline_glass_;
    force_inline_glass_ = true;
    glass_pool_.BeginFrame();
    d2d_dc_->BeginDraw();
    d2d_dc_->SetTransform(D2D1::Matrix3x2F::Identity());
    DrawScene(state, group_views, tile_board_rect, live_data, snap_inter);
    if (post_overlay_draw) {
        d2d_dc_->SetTransform(D2D1::Matrix3x2F::Identity());
        post_overlay_draw(d2d_dc_.Get());
    }
    const HRESULT snap_hr = d2d_dc_->EndDraw();
    glass_pool_.EndFrame();
    force_inline_glass_ = prev_force;
    if (FAILED(snap_hr)) return false;

    ComPtr<ID3D11Texture2D> back_tex;
    if (FAILED(swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back_tex))) || !back_tex) return false;

    D3D11_TEXTURE2D_DESC desc{};
    back_tex->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(d3d_device_->CreateTexture2D(&desc, nullptr, &staging)) || !staging) return false;
    d3d_context_->CopyResource(staging.Get(), back_tex.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(d3d_context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;

    bool encoded_ok = false;
    ComPtr<IWICImagingFactory> wic;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) && wic) {
        ComPtr<IWICStream> stream;
        if (SUCCEEDED(wic->CreateStream(&stream)) && stream &&
            SUCCEEDED(stream->InitializeFromFilename(png_path.c_str(), GENERIC_WRITE))) {
            ComPtr<IWICBitmapEncoder> enc;
            if (SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && enc &&
                SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache))) {
                ComPtr<IWICBitmapFrameEncode> frame;
                ComPtr<IPropertyBag2> props;
                if (SUCCEEDED(enc->CreateNewFrame(&frame, &props)) && frame &&
                    SUCCEEDED(frame->Initialize(props.Get())) &&
                    SUCCEEDED(frame->SetSize(desc.Width, desc.Height))) {
                    WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
                    if (SUCCEEDED(frame->SetPixelFormat(&pf))) {
                        const UINT total_bytes = mapped.RowPitch * desc.Height;
                        if (SUCCEEDED(frame->WritePixels(
                                desc.Height, mapped.RowPitch, total_bytes, static_cast<BYTE*>(mapped.pData))) &&
                            SUCCEEDED(frame->Commit()) &&
                            SUCCEEDED(enc->Commit())) {
                            encoded_ok = true;
                        }
                    }
                }
            }
        }
    }

    d3d_context_->Unmap(staging.Get(), 0);
    return encoded_ok;
}

void DesktopRenderer::CaptureMenuUnderlay(POINT anchor_screen_px,
                                          const model::DesktopModelState& state,
                                          std::span<const model::GroupComputedView> group_views,
                                          const model::RectI& tile_board_rect,
                                          const LiveTileRuntimeData& live_data,
                                          const DesktopInteractionState& interaction) {
    if (!d2d_dc_ || !d3d_device_ || !d3d_context_ || width_ == 0 || height_ == 0) return;

    const model::CardMaterial menu_mat =
        (state.settings.default_material == model::CardMaterial::FollowHost)
            ? interaction.host_material
            : state.settings.default_material;
    if (menu_mat != model::CardMaterial::LiquidGlass || !interaction.transparency_enabled) {
        backdrop_.SetMenuUnderlaySnapshot({}, 0, 0, {});
        return;
    }

    // Cover typical root popup + submenu footprint around `anchor_screen_px`, clamped to our surface.
    const RECT surf_scr{
        surface_px_.x, surface_px_.y,
        surface_px_.x + (surface_px_.w > 0 ? surface_px_.w : static_cast<int>(width_)),
        surface_px_.y + (surface_px_.h > 0 ? surface_px_.h : static_cast<int>(height_))};
    const LONG pad_px = static_cast<LONG>(std::ceil(64.0f * dip_scale_));
    const LONG span_w = static_cast<LONG>(std::ceil(560.0f * dip_scale_));
    const LONG span_h = static_cast<LONG>(std::ceil(520.0f * dip_scale_));
    RECT cap_scr{
        std::max(surf_scr.left, anchor_screen_px.x - pad_px),
        std::max(surf_scr.top, anchor_screen_px.y - pad_px),
        std::min(surf_scr.right, anchor_screen_px.x + span_w),
        std::min(surf_scr.bottom, anchor_screen_px.y + span_h)};
    const LONG cap_w = cap_scr.right - cap_scr.left;
    const LONG cap_h = cap_scr.bottom - cap_scr.top;
    if (cap_w < 32 || cap_h < 32) return;

    // Render at 1/2 physical resolution (~280x260 = ~290 KB, < 0.8 ms) onto an isolated target texture.
    const uint32_t out_w = std::max(16u, static_cast<uint32_t>(cap_w) / 2u);
    const uint32_t out_h = std::max(16u, static_cast<uint32_t>(cap_h) / 2u);

    D3D11_TEXTURE2D_DESC td{};
    td.Width = out_w;
    td.Height = out_h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> gpu_tex;
    if (FAILED(d3d_device_->CreateTexture2D(&td, nullptr, &gpu_tex)) || !gpu_tex) return;

    ComPtr<IDXGISurface> dxgi_surf;
    if (FAILED(gpu_tex.As(&dxgi_surf)) || !dxgi_surf) return;

    const float half_dpi = static_cast<float>(dpi_) * 0.5f;
    const D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        half_dpi, half_dpi);
    ComPtr<ID2D1Bitmap1> target;
    if (FAILED(d2d_dc_->CreateBitmapFromDxgiSurface(dxgi_surf.Get(), &bp, &target)) || !target) return;

    ComPtr<ID2D1Image> prev_target;
    d2d_dc_->GetTarget(&prev_target);
    float prev_dpi_x = 96.0f, prev_dpi_y = 96.0f;
    d2d_dc_->GetDpi(&prev_dpi_x, &prev_dpi_y);
    const D2D1_RECT_F prev_clip = active_clip_dip_;
    const bool prev_force = force_inline_glass_;

    const float local_left_dip = static_cast<float>(cap_scr.left - surf_scr.left) / dip_scale_;
    const float local_top_dip = static_cast<float>(cap_scr.top - surf_scr.top) / dip_scale_;
    const float local_right_dip = static_cast<float>(cap_scr.right - surf_scr.left) / dip_scale_;
    const float local_bottom_dip = static_cast<float>(cap_scr.bottom - surf_scr.top) / dip_scale_;

    DesktopInteractionState under_inter = interaction;
    under_inter.force_opaque_wallpaper = true;
    force_inline_glass_ = true;
    current_glass_settings_ = state.settings;
    current_glass_live_rate_ = state.settings.glass_live_rate;

    glass_pool_.BeginFrame();
    d2d_dc_->SetTarget(target.Get());
    d2d_dc_->SetDpi(half_dpi, half_dpi);
    d2d_dc_->BeginDraw();
    d2d_dc_->SetTransform(D2D1::Matrix3x2F::Translation(-local_left_dip, -local_top_dip));
    active_clip_dip_ = D2D1::RectF(local_left_dip, local_top_dip, local_right_dip, local_bottom_dip);

    // If live wallpaper is active, paint its latest GPU frame underneath before desktop cards!
    DrawScene(state, group_views, tile_board_rect, live_data, under_inter);
    d2d_dc_->SetTransform(D2D1::Matrix3x2F::Identity());
    const HRESULT draw_hr = d2d_dc_->EndDraw();
    d2d_dc_->SetTarget(prev_target.Get());
    d2d_dc_->SetDpi(prev_dpi_x, prev_dpi_y);
    active_clip_dip_ = prev_clip;
    glass_pool_.EndFrame();
    force_inline_glass_ = prev_force;
    if (FAILED(draw_hr)) return;

    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(d3d_device_->CreateTexture2D(&td, nullptr, &staging)) || !staging) return;
    d3d_context_->CopyResource(staging.Get(), gpu_tex.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(d3d_context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)) || !mapped.pData) return;
    std::vector<uint8_t> pbgra(static_cast<size_t>(out_w) * out_h * 4u);
    for (uint32_t y = 0; y < out_h; ++y) {
        std::memcpy(pbgra.data() + static_cast<size_t>(y) * out_w * 4u,
                    static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
                    static_cast<size_t>(out_w) * 4u);
    }
    d3d_context_->Unmap(staging.Get(), 0);
    backdrop_.SetMenuUnderlaySnapshot(std::move(pbgra), out_w, out_h, cap_scr);
}

model::CardMaterial DesktopRenderer::ResolveMaterial(model::CardMaterial requested,
                                                     const model::BeautifySettings& settings,
                                                     const DesktopInteractionState& interaction) const noexcept {
    if (!interaction.transparency_enabled) return model::CardMaterial::Solid;
    model::CardMaterial m = requested;
    if (m == model::CardMaterial::FollowHost) m = settings.default_material;
    if (m == model::CardMaterial::FollowHost) m = interaction.host_material;
    if (m == model::CardMaterial::FollowHost) m = model::CardMaterial::Acrylic;
    return m;
}

uint32_t DesktopRenderer::ResolveAccentArgb(uint32_t item_accent,
                                            const model::BeautifySettings& settings,
                                            const DesktopInteractionState& interaction) const noexcept {
    if ((item_accent & 0x00FFFFFFu) != 0) return 0xFF000000u | (item_accent & 0x00FFFFFFu);
    if (settings.auto_wallpaper_accent) return backdrop_.VibrantAccentArgb();
    if ((settings.accent_argb & 0x00FFFFFFu) != 0) return 0xFF000000u | (settings.accent_argb & 0x00FFFFFFu);
    return interaction.host_accent_argb;
}

ID2D1SolidColorBrush* DesktopRenderer::Brush(uint32_t argb, float opacity_mul) {
    solid_brush_->SetColor(ColorFromArgb(argb, opacity_mul));
    return solid_brush_.Get();
}

void DesktopRenderer::DrawScene(const model::DesktopModelState& state,
                                std::span<const model::GroupComputedView> group_views,
                                const model::RectI& tile_board_rect,
                                const LiveTileRuntimeData& live_data,
                                const DesktopInteractionState& interaction) {
    backdrop_.DrainCompletedUploads(d2d_dc_.Get());
    last_banner_action_rect_ = {};

    const D2D1_RECT_F full_rc = D2D1::RectF(0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_));
    const bool custom_preset = (state.settings.wallpaper_preset != model::WallpaperPreset::System);

    // A sparse desktop layer leaves Windows' real wallpaper visible. An opaque
    // snapshot must use that same wallpaper, never substitute a preset.
    if (!vsurf_ && custom_preset) {
        WallpaperBackdrop::DrawPresetScene(d2d_factory_.Get(), d2d_dc_.Get(), full_rc,
                                           state.settings.wallpaper_preset, state.settings.dark_mode);
    } else if (using_dcomp_ && !interaction.force_opaque_wallpaper) {
        d2d_dc_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    } else if (interaction.force_opaque_wallpaper && live_wallpaper_.HasFrame() && live_wallpaper_.SharpBitmap()) {
        const auto live_rect = D2D1::RectF(
            0.0f, 0.0f,
            static_cast<float>(live_wallpaper_.LogicalScreenWidth()) / dip_scale_,
            static_cast<float>(live_wallpaper_.LogicalScreenHeight()) / dip_scale_);
        d2d_dc_->DrawBitmap(live_wallpaper_.SharpBitmap(), live_rect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else if (auto* wp = backdrop_.QuarterWallpaperBitmap()) {
        // Same monitor-pixel -> surface-DIP mapping as the material brushes.
        const float left = static_cast<float>(monitor_px_.x - surface_px_.x) / dip_scale_;
        const float top = static_cast<float>(monitor_px_.y - surface_px_.y) / dip_scale_;
        const auto wallpaper_rect = D2D1::RectF(left, top,
            left + static_cast<float>(backdrop_.ScreenWidth()) / dip_scale_,
            top + static_cast<float>(backdrop_.ScreenHeight()) / dip_scale_);
        d2d_dc_->DrawBitmap(wp, wallpaper_rect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else {
        d2d_dc_->Clear(D2D1::ColorF(0.06f, 0.10f, 0.18f, 1.0f));
    }

    const bool focus_scene = (state.settings.active_scene == model::DesktopScene::Focus);
    const bool present_scene = (state.settings.active_scene == model::DesktopScene::Present);
    const bool show_desktop_files = !state.settings.icons_hidden && !present_scene;

    // 1. Loose icons & Group cards (hidden when icons_hidden or in Present scene)
    if (show_desktop_files) {
        if (!state.settings.organize_enabled) {
            DrawLooseIcons(state, {}, live_data, interaction);
        } else {
            if (!focus_scene) {
                DrawLooseIcons(state, group_views, live_data, interaction);
            }
            for (size_t i = 0; i < state.groups.size() && i < group_views.size(); ++i) {
                if (group_views[i].temporarily_expanded) continue;
                const auto& grp = state.groups[i];
                const auto& view = group_views[i];
                model::RectI actual_frame = view.frame;
                if (!view.expanded) {
                    actual_frame.h = model::kGroupHeaderHeight;
                }
                if (!IntersectsActiveClip(actual_frame, 24)) continue;
                const float op = (focus_scene && !grp.focus_visible) ? 0.40f : 1.0f;
                DrawGroupCard(grp, view, state, live_data, interaction, op);
            }
        }
    }

    // 2. Live Tiles (full-desktop free placement + resize; edit mode header anchored top-right)
    if (state.settings.tiles_enabled && state.settings.organize_enabled && !interaction.side_drawer_open) {
        const model::RectI wa = !state.monitors.empty()
            ? state.monitors.front().work_area
            : model::RectI{0, 0, static_cast<int>(width_ / dip_scale_), static_cast<int>(height_ / dip_scale_)};
        const model::RectI hdr_rect = !tile_board_rect.empty() ? tile_board_rect
                                                               : ComputeBoardRectFallback(wa, true);
        const bool any_free = std::any_of(state.tiles.begin(), state.tiles.end(),
                                          [](const model::DesktopTile& t) { return t.free_placed; });
        const bool show_idle_hdr = !any_free && !state.tiles.empty();
        if (!hdr_rect.empty() && (interaction.tile_edit_mode || show_idle_hdr)) {
            DrawTileBoardHeader(hdr_rect, state.settings.dark_mode,
                                ResolveAccentArgb(0, state.settings, interaction),
                                interaction.tile_edit_mode, show_idle_hdr, interaction.hovered_tile_header_btn,
                                focus_scene ? 0.38f : 1.0f);
        }

        // 编辑磁贴区: show 5x7 + slots only when tiles still use the default starter grid.
        if (interaction.tile_edit_mode && !any_free && !tile_board_rect.empty()) {
            bool occ[model::kTileRows][model::kTileCols]{};
            for (const auto& t : state.tiles)
                for (int r = t.row; r < t.row + t.size.rows && r < model::kTileRows; ++r)
                    for (int c = t.col; c < t.col + t.size.cols && c < model::kTileCols; ++c) occ[r][c] = true;
            for (int r = 0; r < model::kTileRows; ++r)
                for (int c = 0; c < model::kTileCols; ++c) {
                    if (occ[r][c]) continue;
                    model::DesktopTile cell{}; cell.col = c; cell.row = r; cell.size = {1, 1};
                    const D2D1_RECT_F cr = ToRectF(ComputeTileRectInline(tile_board_rect, cell));
                    d2d_dc_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cr.left + 1.0f, cr.top + 1.0f, cr.right - 1.0f, cr.bottom - 1.0f), 15.0f, 15.0f),
                                                  Brush(state.settings.dark_mode ? 0x48FFFFFFu : 0x38000000u), 1.5f, dashed_stroke_.Get());
                    const wchar_t plus[2] = {L'\xE710', 0};
                    DrawGlyph(d2d_dc_.Get(), plus, 1, fmt_fluent_21_.Get(), cr, Brush(state.settings.dark_mode ? 0xC0FFFFFFu : 0xA0000000u));
                }
        }
        for (const auto& tile : state.tiles) {
            const model::RectI px_rect = ComputeTileRectInline(tile_board_rect, tile);
            const bool active_drag_or_resize =
                (interaction.dragging_tile && interaction.dragging_tile_id == tile.id) ||
                interaction.resizing_tile_id == tile.id;
            // Jiggle while editing (a fraction of a degree, alternating between neighbours), as on a phone.
            const bool jiggle = interaction.tile_edit_mode && !active_drag_or_resize;
            const bool pressed = interaction.pressed_tile_id == tile.id && std::abs(interaction.tile_press) > 0.001f;
            if (!jiggle && !pressed && !active_drag_or_resize && !IntersectsActiveClip(px_rect, 28)) continue;
            const float tile_op = (focus_scene && !IsFocusKeepTileKind(model::ActiveTileView(tile).kind)) ? 0.38f : 1.0f;
            D2D1_MATRIX_3X2_F saved_tf{};
            if (jiggle || pressed) {
                d2d_dc_->GetTransform(&saved_tf);
                const D2D1_POINT_2F c = D2D1::Point2F(px_rect.x + px_rect.w * 0.5f, px_rect.y + px_rect.h * 0.5f);
                D2D1_MATRIX_3X2_F local = D2D1::Matrix3x2F::Identity();
                if (pressed) {
                    const float shrink = 5.0f * interaction.tile_press;
                    local = D2D1::Matrix3x2F::Scale(1.0f - 2.0f * shrink / std::max(1.0f, static_cast<float>(px_rect.w)),
                                                    1.0f - 2.0f * shrink / std::max(1.0f, static_cast<float>(px_rect.h)), c);
                }
                if (jiggle) {
                    const float sign = ((tile.col + tile.row + (tile.x / 64)) % 2) ? 1.0f : -1.0f;
                    const float deg = 0.45f * sign * std::sin(interaction.edit_phase * 2.0f * kPi);
                    local = local * D2D1::Matrix3x2F::Rotation(deg, c);
                }
                d2d_dc_->SetTransform(local * saved_tf);
            }
            if (tile_op < 0.99f) {
                d2d_dc_->PushLayer(
                    D2D1::LayerParameters1(ToRectF(model::RectI{px_rect.x - 16, px_rect.y - 16, px_rect.w + 32, px_rect.h + 32}),
                                           nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                           D2D1::IdentityMatrix(), tile_op, nullptr, D2D1_LAYER_OPTIONS1_NONE),
                    nullptr);
            }
            DrawLiveTile(tile, ToRectF(px_rect), state, live_data, interaction);
            if (tile_op < 0.99f) {
                d2d_dc_->PopLayer();
            }
            if (jiggle || pressed) d2d_dc_->SetTransform(saved_tf);
            if (interaction.tile_edit_mode && !(interaction.dragging_tile && interaction.dragging_tile_id == tile.id)) {
                DrawTileEditDecor(tile, ToRectF(px_rect), state.settings.dark_mode,
                                  interaction.hovered_tile_id == tile.id,
                                  interaction.resizing_tile_id == tile.id,
                                  ResolveAccentArgb(tile.accent_argb, state.settings, interaction));
            }
        }

        if (interaction.dragging_tile && !interaction.drag_tile_preview_rect.empty()) {
            const D2D1_RECT_F pr = ToRectF(interaction.drag_tile_preview_rect);
            const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(pr, 16.0f, 16.0f);
            const uint32_t preview_color = interaction.drag_tile_valid ? 0xFF4CC2FFu : 0xFFFF5F56u;
            d2d_dc_->FillRoundedRectangle(rr, Brush(preview_color, 0.14f));
            d2d_dc_->DrawRoundedRectangle(rr, Brush(preview_color, 0.92f), 2.0f, dashed_stroke_.Get());
        }
    }

    // Hover expansion overlays neighboring cards without moving their saved positions.
    if (show_desktop_files && state.settings.organize_enabled) {
        for (size_t i = 0; i < state.groups.size() && i < group_views.size(); ++i) {
            const auto& view = group_views[i];
            if (view.temporarily_expanded && IntersectsActiveClip(view.frame, 24))
                DrawGroupCard(state.groups[i], view, state, live_data, interaction, 1.0f);
        }
    }

    // 3. Rubber-band marquee selection rectangle
    if (interaction.marquee_active && !interaction.marquee_rect.empty()) {
        const uint32_t accent = ResolveAccentArgb(0, state.settings, interaction);
        const D2D1_RECT_F mr = ToRectF(interaction.marquee_rect);
        d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(mr, 4.0f, 4.0f), Brush(accent, 0.18f));
        d2d_dc_->DrawRoundedRectangle(D2D1::RoundedRect(mr, 4.0f, 4.0f), Brush(accent, 0.85f), 1.4f);
    }

    // 4. Multi-select item drag ghost (.dghost) with top-right count badge
    if (interaction.dragging_items && interaction.drag_item_count > 0) {
        const uint32_t accent = ResolveAccentArgb(0, state.settings, interaction);
        const float gx = static_cast<float>(interaction.drag_cursor_pos.x + 12);
        const float gy = static_cast<float>(interaction.drag_cursor_pos.y + 12);
        const D2D1_RECT_F ghost_rc = D2D1::RectF(gx, gy, gx + 64.0f, gy + 64.0f);
        DrawCardMaterial(ghost_rc, 12.0f, model::CardMaterial::Acrylic, 86,
                         state.settings.dark_mode, accent, true, true);
        DrawVectorFileIcon(d2d_factory_.Get(), d2d_dc_.Get(), dwrite_factory_.Get(),
                           D2D1::RectF(gx + 13.0f, gy + 13.0f, gx + 51.0f, gy + 51.0f),
                           interaction.drag_primary_ext, interaction.drag_primary_folder, 0.96f);
        if (interaction.drag_item_count > 1) {
            wchar_t cnt_buf[16]{};
            std::swprintf(cnt_buf, ARRAYSIZE(cnt_buf), L"%zu", interaction.drag_item_count);
            const D2D1_RECT_F brc = D2D1::RectF(ghost_rc.right - 14.0f, ghost_rc.top - 7.0f,
                                                ghost_rc.right + 9.0f, ghost_rc.top + 15.0f);
            d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(brc, 11.0f, 11.0f), Brush(accent));
            DrawWidgetText(cnt_buf, static_cast<UINT32>(std::wcslen(cnt_buf)), fmt_pill_.Get(),
                              brc, Brush(0xFFFFFFFFu));
        }
    }

    // 5. Pointer-following OLE Drop Hint Pill
    if (interaction.drop_hint_visible && !interaction.drop_hint_label.empty()) {
        const float px = static_cast<float>(interaction.drop_hint_pos.x + 16);
        const float py = static_cast<float>(interaction.drop_hint_pos.y + 18);
        const float pw = std::max(148.0f, MeasureSingleLineWidth(interaction.drop_hint_label, fmt_pill_.Get(), 360.0f) + 30.0f);
        const D2D1_RECT_F pill_rc = D2D1::RectF(px, py, px + pw, py + 30.0f);
        const uint32_t accent = ResolveAccentArgb(0, state.settings, interaction);
        DrawCardMaterial(pill_rc, 15.0f, model::CardMaterial::Acrylic, 90,
                         state.settings.dark_mode, accent, true);
        DrawWidgetText(interaction.drop_hint_label.c_str(),
                          static_cast<UINT32>(interaction.drop_hint_label.size()),
                          fmt_pill_.Get(), pill_rc,
                          Brush(state.settings.dark_mode ? 0xFFFFFFFFu : 0xFF141820u));
    }

    // 6. Bottom-left scene / hidden-icons #banner pill
    DrawSceneBanner(state, interaction);
}

ComPtr<ID2D1Bitmap1> CreateExteriorCardShadowBitmap(ID2D1DeviceContext* dc) {
    if (!dc) return nullptr;

    // 160x160 texels at 2x oversampling (80x80 DIPs):
    //   [0..14) DIP   = exterior shadow margin (28 texels)
    //   [14..28) DIP  = top-left / bottom-left rounded corner quadrant (r = 14 DIP, 28 texels)
    //   [28..52) DIP  = translation-invariant straight edge bridge (24 DIP, 48 texels)
    //   [52..66) DIP  = top-right / bottom-right rounded corner quadrant (r = 14 DIP, 28 texels)
    //   [66..80) DIP  = exterior shadow margin (28 texels)
    constexpr int kDim = 160;
    constexpr float kInvTexScale = 0.5f;
    constexpr float kCard0 = 14.0f;
    constexpr float kCard1 = 66.0f;
    constexpr float kRadius = 14.0f;

    auto rounded_sdf = [](float px, float py, float x0, float y0, float x1, float y1, float r) noexcept -> float {
        const float cx = (x0 + x1) * 0.5f;
        const float cy = (y0 + y1) * 0.5f;
        const float hx = (x1 - x0) * 0.5f - r;
        const float hy = (y1 - y0) * 0.5f - r;
        const float qx = std::abs(px - cx) - hx;
        const float qy = std::abs(py - cy) - hy;
        const float out_len = std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f));
        const float in_max = std::min(std::max(qx, qy), 0.0f);
        return out_len + in_max - r;
    };

    std::vector<float> amb(static_cast<size_t>(kDim * kDim), 0.0f);
    std::vector<float> key(static_cast<size_t>(kDim * kDim), 0.0f);
    std::vector<float> tmp(static_cast<size_t>(kDim * kDim), 0.0f);

    for (int ty = 0; ty < kDim; ++ty) {
        const float y = (static_cast<float>(ty) + 0.5f) * kInvTexScale;
        for (int tx = 0; tx < kDim; ++tx) {
            const float x = (static_cast<float>(tx) + 0.5f) * kInvTexScale;
            const size_t idx = static_cast<size_t>(ty * kDim + tx);
            amb[idx] = std::clamp(0.5f - rounded_sdf(x, y, kCard0, kCard0 + 0.8f, kCard1, kCard1 + 0.8f, kRadius),
                                  0.0f, 1.0f);
            key[idx] = std::clamp(0.5f - rounded_sdf(x, y, kCard0, kCard0 + 2.8f, kCard1, kCard1 + 2.8f, kRadius),
                                  0.0f, 1.0f);
        }
    }

    auto gaussian_blur = [&](std::vector<float>& buf, float sigma_dip) {
        const float sigma_tex = sigma_dip / kInvTexScale;
        const int rad = std::clamp(static_cast<int>(std::ceil(sigma_tex * 3.2f)), 1, 28);
        std::vector<float> kernel(static_cast<size_t>(2 * rad + 1));
        float ksum = 0.0f;
        const float inv_2s2 = 1.0f / (2.0f * sigma_tex * sigma_tex);
        for (int k = -rad; k <= rad; ++k) {
            const float w = std::exp(-static_cast<float>(k * k) * inv_2s2);
            kernel[static_cast<size_t>(k + rad)] = w;
            ksum += w;
        }
        for (float& w : kernel) w /= ksum;

        for (int y = 0; y < kDim; ++y) {
            for (int x = 0; x < kDim; ++x) {
                float acc = 0.0f;
                for (int k = -rad; k <= rad; ++k) {
                    const int sx = std::clamp(x + k, 0, kDim - 1);
                    acc += buf[static_cast<size_t>(y * kDim + sx)] * kernel[static_cast<size_t>(k + rad)];
                }
                tmp[static_cast<size_t>(y * kDim + x)] = acc;
            }
        }
        for (int y = 0; y < kDim; ++y) {
            for (int x = 0; x < kDim; ++x) {
                float acc = 0.0f;
                for (int k = -rad; k <= rad; ++k) {
                    const int sy = std::clamp(y + k, 0, kDim - 1);
                    acc += tmp[static_cast<size_t>(sy * kDim + x)] * kernel[static_cast<size_t>(k + rad)];
                }
                buf[static_cast<size_t>(y * kDim + x)] = acc;
            }
        }
    };

    gaussian_blur(amb, 1.5f);
    gaussian_blur(key, 3.3f);

    std::vector<uint8_t> pixels(static_cast<size_t>(kDim * kDim * 4), 0u);
    for (int ty = 0; ty < kDim; ++ty) {
        const float y = (static_cast<float>(ty) + 0.5f) * kInvTexScale;
        const float border_fade_y = std::clamp(std::min(y, 80.0f - y) / 2.0f, 0.0f, 1.0f);
        for (int tx = 0; tx < kDim; ++tx) {
            const float x = (static_cast<float>(tx) + 0.5f) * kInvTexScale;
            const float border_fade_x = std::clamp(std::min(x, 80.0f - x) / 2.0f, 0.0f, 1.0f);
            const float d_card = rounded_sdf(x, y, kCard0, kCard0, kCard1, kCard1, kRadius);
            if (d_card <= 0.0f) continue; // Strictly zero alpha inside the rounded card!

            const float ext_mask = std::clamp(d_card / 0.5f, 0.0f, 1.0f);
            const size_t idx = static_cast<size_t>(ty * kDim + tx);
            const float raw_a = (0.11f * amb[idx] + 0.30f * key[idx]) *
                                ext_mask * border_fade_x * border_fade_y;
            const uint8_t alpha = static_cast<uint8_t>(std::clamp(raw_a * 255.0f + 0.5f, 0.0f, 255.0f));
            pixels[idx * 4 + 3] = alpha; // Premultiplied black: RGB = 0, A = alpha.
        }
    }

    const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    ComPtr<ID2D1Bitmap1> out;
    dc->CreateBitmap(D2D1::SizeU(kDim, kDim), pixels.data(), kDim * 4u, &props, &out);
    return out;
}

void DrawExteriorCardShadow(ID2D1DeviceContext* dc, ID2D1Bitmap1* shadow_bitmap,
                            const D2D1_RECT_F& rect, float radius, float opacity) {
    if (!dc || !shadow_bitmap || opacity <= 0.004f) return;
    const float w = rect.right - rect.left;
    const float h = rect.bottom - rect.top;
    if (w <= 2.0f || h <= 2.0f) return;

    const float r_c = std::clamp(std::min({radius, w * 0.5f, h * 0.5f}), 1.0f, 16.0f);
    const float pad = r_c;
    const float op = std::clamp(opacity, 0.0f, 1.0f);

    const float cx0 = rect.left - pad;
    const float cx1 = rect.left + r_c;
    const float cx2 = rect.right - r_c;
    const float cx3 = rect.right + pad;

    const float cy0 = rect.top - pad;
    const float cy1 = rect.top + r_c;
    const float cy2 = rect.bottom - r_c;
    const float cy3 = rect.bottom + pad;

    // 8 pairwise-disjoint outer slices (center skipped; corner interiors have alpha == 0).
    auto quad = [&](float dl, float dt, float dr, float db, float sl, float st, float sr, float sb) {
        if (dr <= dl || db <= dt) return;
        dc->DrawBitmap(shadow_bitmap, D2D1::RectF(dl, dt, dr, db), op,
                       D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, D2D1::RectF(sl, st, sr, sb));
    };

    quad(cx0, cy0, cx1, cy1, 0.0f, 0.0f, 56.0f, 56.0f);       // Top-Left corner
    quad(cx2, cy0, cx3, cy1, 104.0f, 0.0f, 160.0f, 56.0f);    // Top-Right corner
    quad(cx0, cy2, cx1, cy3, 0.0f, 104.0f, 56.0f, 160.0f);    // Bottom-Left corner
    quad(cx2, cy2, cx3, cy3, 104.0f, 104.0f, 160.0f, 160.0f); // Bottom-Right corner
    quad(cx1, cy0, cx2, rect.top, 56.0f, 0.0f, 104.0f, 28.0f);       // Top edge
    quad(cx1, rect.bottom, cx2, cy3, 56.0f, 132.0f, 104.0f, 160.0f); // Bottom edge
    quad(cx0, cy1, rect.left, cy2, 0.0f, 56.0f, 28.0f, 104.0f);      // Left edge
    quad(rect.right, cy1, cx3, cy2, 132.0f, 56.0f, 160.0f, 104.0f);  // Right edge
}

void DesktopRenderer::DrawCardMaterial(const D2D1_RECT_F& rect, float radius,
                                       model::CardMaterial material,
                                       uint8_t opacity_percent,
                                       bool dark_mode,
                                       uint32_t accent_argb,
                                       bool emphasized_border,
                                       bool elevated_hover, float background_opacity,
                                       uint8_t /*dispersion_level*/,
                                       float press_amount,
                                       bool allow_lower_glass,
                                       bool /*is_group_card*/,
                                       std::string_view card_id,
                                       const model::BeautifySettings* glass_settings) {
    // 30..100 % thickens the material (CardSurfaceProfile); below 30 % the thinnest material fades
    // out, so 0 % leaves only the content and a faint outline over the wallpaper.
    const float fade = std::clamp(static_cast<float>(opacity_percent) / 30.0f, 0.0f, 1.0f);

    if (material == model::CardMaterial::LiquidGlass) {
        // 1) Settled desktop cards on `vsurf_` delegate shadow + refractive glass + 1px SDF rim
        //    to the lower `glass_vsurf_` DComp layer so dynamic wallpaper & cursor hover light
        //    updates never redraw foreground icons/text!
        if (allow_lower_glass && vsurf_ && !force_inline_glass_ && !glass_pool_.Unavailable() &&
            EnsureGlassVirtualSurface()) {
            if (emphasized_border) {
                const D2D1_RECT_F inset = D2D1::RectF(rect.left + 0.5f, rect.top + 0.5f, rect.right - 0.5f, rect.bottom - 0.5f);
                const float inset_r = std::max(0.5f, radius - 0.5f);
                d2d_dc_->DrawRoundedRectangle(D2D1::RoundedRect(inset, inset_r, inset_r),
                                              Brush(accent_argb, 0.88f), 1.5f);
            }
            return;
        }

        // 2) Inline Liquid Glass pass (offscreen PNG snapshots, benchmarks, dragged/jiggling cards, drag pills):
        GlassCardRequest req{};
        req.left_dip = rect.left;
        req.top_dip = rect.top;
        req.right_dip = rect.right;
        req.bottom_dip = rect.bottom;
        req.radius_dip = radius;
        const model::BeautifySettings& gs = glass_settings ? *glass_settings : current_glass_settings_;
        const bool cursor_active = (!card_id.empty() && hover_light_card_id_ == card_id);
        ApplyBeautifyGlassOptics(req, gs, cursor_active, hover_light_);
        req.opacity_percent = opacity_percent;
        req.dark_mode = dark_mode;
        req.elevated_hover = elevated_hover;
        req.allow_clear_passthrough = (vsurf_ != nullptr && !force_inline_glass_);
        req.energy_saver = energy_saver_cached_;
        req.draw_rim_in_shader = !emphasized_border;
        req.background_luminance = backdrop_.AverageLuminance(rect);
        req.background_opacity = background_opacity;
        req.press_amount = press_amount;
        const float glass_shadow = (dark_mode ? (elevated_hover ? 0.65f : 0.54f)
                                              : (elevated_hover ? 0.46f : 0.32f)) *
                                   fade * background_opacity;
        if (DrawSingleLiquidGlassCard(d2d_dc_.Get(), req, glass_shadow)) {
            if (emphasized_border) {
                const D2D1_RECT_F inset = D2D1::RectF(rect.left + 0.5f, rect.top + 0.5f, rect.right - 0.5f, rect.bottom - 0.5f);
                const float inset_r = std::max(0.5f, radius - 0.5f);
                d2d_dc_->DrawRoundedRectangle(D2D1::RoundedRect(inset, inset_r, inset_r),
                                              Brush(accent_argb, 0.88f), 1.5f);
            }
            return;
        }
        // 3) Graceful fallback (§4.8): if D3D FL < 10_0 or custom effect unavailable, fall back to Acrylic!
        material = model::CardMaterial::Acrylic;
    }

    if (fade > 0.0f) {
        if (!card_shadow_bitmap_) {
            card_shadow_bitmap_ = CreateExteriorCardShadowBitmap(d2d_dc_.Get());
        }
        const float shadow_strength = (dark_mode ? (elevated_hover ? 1.00f : 0.86f)
                                                 : (elevated_hover ? 0.72f : 0.50f)) *
                                      fade * background_opacity;
        DrawExteriorCardShadow(d2d_dc_.Get(), card_shadow_bitmap_.Get(), rect, radius, shadow_strength);
    }

    const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(rect, radius, radius);

    const auto theme = ui::MakeTheme(dark_mode, ui::HexColor(accent_argb));
    const bool mica = material == model::CardMaterial::Mica || material == model::CardMaterial::MicaAlt;
    const bool faded = fade < 0.999f;
    const bool live_host_backdrop =
        (material == model::CardMaterial::Acrylic && native_acrylic_allowed_ && host_backdrop_.Active());
    if (faded && fade > 0.0f && !live_host_backdrop) {
        d2d_dc_->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                  D2D1::IdentityMatrix(), fade, nullptr, D2D1_LAYER_OPTIONS1_NONE),
                           nullptr);
    }
    if (fade <= 0.0f) {
        // Fully transparent: no material at all.
    } else if (mica) {
        // Opaque wallpaper-only Mica uses the shared graph with a desktop profile.
        // User opacity is not a raw-wallpaper blend; this is sampled, not native DWM.
        auto* material_brush = backdrop_.SampledMicaBrush(d2d_dc_.Get(),
            material == model::CardMaterial::MicaAlt, dark_mode, opacity_percent);
        d2d_dc_->FillRoundedRectangle(rr, material_brush ? static_cast<ID2D1Brush*>(material_brush)
                                                       : Brush(ColorToArgb(theme.bg)));
    } else if (material == model::CardMaterial::Acrylic && native_acrylic_allowed_ &&
               host_backdrop_.SetMaterial(dark_mode, CardSurfaceProfile(dark_mode, opacity_percent).tint_opacity,
                   CardSurfaceProfile(dark_mode, opacity_percent).luminosity_opacity,
                   ColorToArgb(CardMaterialTint(dark_mode))) &&
               host_backdrop_.AddCard(rect, radius, dip_scale_, background_opacity * fade)) {
        // The OS supplies live behind-window blur in the lower WUC visual tree.
        // Tint/luminosity are part of the lower blend graph; no opaque foreground
        // wash. Noise, border and text remain in this sparse foreground surface.
    } else if (material == model::CardMaterial::Acrylic &&
               ((!using_dcomp_ && !vsurf_ && hwnd_ == nullptr) ||
                (pulse::os::TrueWindowsBuild() < 22000 && native_acrylic_allowed_ && !host_backdrop_.Active())) &&
               backdrop_.AcrylicBrush() != nullptr) {
        // Offscreen --render-snapshots path or Windows 10 (without DWMWA_USE_HOSTBACKDROPBRUSH):
        // sample blurred wallpaper + Acrylic tint wash with zero full-screen DWM Gaussian cost.
        d2d_dc_->FillRoundedRectangle(rr, backdrop_.AcrylicBrush());
        const auto profile = CardSurfaceProfile(dark_mode, opacity_percent);
        d2d_dc_->FillRoundedRectangle(
            rr, Brush(ColorToArgb(CardMaterialTint(dark_mode)),
                      std::clamp(profile.tint_opacity + 0.14f, 0.45f, 0.90f)));
    } else {
        // Unsupported/composition-disabled paths are explicitly solid,
        // never a static wallpaper approximation labelled as live acrylic.
        d2d_dc_->FillRoundedRectangle(rr, Brush(ColorToArgb(theme.bg)));
    }

    // 4. Crisp procedural monochrome snow-grain on Acrylic surfaces
    if (fade > 0.0f && material == model::CardMaterial::Acrylic) {
        if (auto* nb = backdrop_.EnsureNoiseBrush(d2d_dc_.Get())) {
            if (faded && live_host_backdrop) {
                d2d_dc_->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                          D2D1::IdentityMatrix(), fade, nullptr, D2D1_LAYER_OPTIONS1_NONE),
                                   nullptr);
                d2d_dc_->FillRoundedRectangle(rr, nb);
                d2d_dc_->PopLayer();
            } else {
                d2d_dc_->FillRoundedRectangle(rr, nb);
            }
        }
    }
    if (faded && fade > 0.0f && !live_host_backdrop) {
        d2d_dc_->PopLayer();
    }

    // 5. Single crisp 1px Windows 11 Fluent elevation border (WinUI 3 SurfaceStroke gradient, zero double lines)
    const D2D1_RECT_F inset = D2D1::RectF(rect.left + 0.5f, rect.top + 0.5f, rect.right - 0.5f, rect.bottom - 0.5f);
    const float inset_r = std::max(0.5f, radius - 0.5f);
    const float outline = emphasized_border ? 0.88f : (0.35f + 0.65f * fade);
    if (emphasized_border) {
        d2d_dc_->DrawRoundedRectangle(D2D1::RoundedRect(inset, inset_r, inset_r),
                                      Brush(accent_argb, outline), 1.5f);
    } else {
        const uint64_t key = (dark_mode ? 1ull : 0ull) | (elevated_hover ? 2ull : 0ull) | 0x100ull;
        if (!border_grad_brush_ || border_grad_key_ != key) {
            border_grad_brush_.Reset();
            const uint32_t top_col = dark_mode
                ? (elevated_hover ? 0x46FFFFFFu : 0x2EFFFFFFu)
                : (elevated_hover ? 0x20000000u : 0x12000000u);
            const uint32_t bot_col = dark_mode
                ? (elevated_hover ? 0x22FFFFFFu : 0x15FFFFFFu)
                : (elevated_hover ? 0x30000000u : 0x20000000u);
            D2D1_GRADIENT_STOP stops[2] = {
                {0.0f, ColorFromArgb(top_col)},
                {1.0f, ColorFromArgb(bot_col)},
            };
            ComPtr<ID2D1GradientStopCollection> col;
            if (SUCCEEDED(d2d_dc_->CreateGradientStopCollection(stops, 2, &col)) && col) {
                d2d_dc_->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.0f, 0.0f), D2D1::Point2F(0.0f, 1.0f)),
                    col.Get(), &border_grad_brush_);
                border_grad_key_ = key;
            }
        }
        if (border_grad_brush_) {
            border_grad_brush_->SetStartPoint(D2D1::Point2F(inset.left, inset.top));
            border_grad_brush_->SetEndPoint(D2D1::Point2F(inset.left, inset.bottom));
            border_grad_brush_->SetOpacity(outline);
            d2d_dc_->DrawRoundedRectangle(D2D1::RoundedRect(inset, inset_r, inset_r),
                                          border_grad_brush_.Get(), 1.0f);
        } else {
            const uint32_t fallback_col = dark_mode ? 0x24FFFFFFu : 0x18000000u;
            d2d_dc_->DrawRoundedRectangle(D2D1::RoundedRect(inset, inset_r, inset_r),
                                          Brush(fallback_col, outline), 1.0f);
        }
    }
}

void DesktopRenderer::DrawGroupCard(const model::DesktopGroup& group,
                                    const model::GroupComputedView& view,
                                    const model::DesktopModelState& state,
                                    const LiveTileRuntimeData& live_data,
                                    const DesktopInteractionState& interaction,
                                    float opacity_scale) {
    if (view.frame.empty()) return;

    const bool effective_collapsed = !view.expanded;
    model::RectI actual_frame = view.frame;
    if (effective_collapsed) {
        actual_frame.h = model::kGroupHeaderHeight;
    }

    const D2D1_RECT_F rc = ToRectF(actual_frame);
    const bool theme_dark = state.settings.dark_mode;
    // The ink (text, glyphs, icon labels) follows what shows through the card, not only the theme.
    const bool dark = CardInkLight(rc, ResolveMaterial(group.material, state.settings, interaction),
                                   state.settings.card_opacity_percent, theme_dark);
    const uint32_t accent = ResolveAccentArgb(group.accent_argb, state.settings, interaction);
    const auto blend_ch = [](uint32_t c, uint32_t target, float t) -> uint32_t {
        const auto ch = [t](uint32_t a, uint32_t b, int shift) -> uint32_t {
            const float va = static_cast<float>((a >> shift) & 0xFFu);
            const float vb = static_cast<float>((b >> shift) & 0xFFu);
            return static_cast<uint32_t>(std::clamp(va + (vb - va) * t + 0.5f, 0.0f, 255.0f)) << shift;
        };
        return 0xFF000000u | ch(c, target, 16) | ch(c, target, 8) | ch(c, target, 0);
    };
    const uint32_t accent_fg = dark ? blend_ch(accent, 0xFFFFFFFFu, 0.36f) : blend_ch(accent, 0xFF000000u, 0.12f);
    const model::CardMaterial mat = ResolveMaterial(group.material, state.settings, interaction);
    const bool group_hovered = (interaction.hovered_group_id == group.id);
    const bool drop_target = (interaction.drop_target_group_id == group.id);

    if (opacity_scale < 0.99f) {
        d2d_dc_->PushLayer(
            D2D1::LayerParameters1(D2D1::RectF(rc.left - 12.0f, rc.top - 12.0f, rc.right + 12.0f, rc.bottom + 16.0f),
                                   nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                   D2D1::IdentityMatrix(), opacity_scale, nullptr, D2D1_LAYER_OPTIONS1_NONE),
            nullptr);
    }

    const std::string grp_card_id = "grp:" + group.id;
    DrawCardMaterial(rc, 14.0f, mat, state.settings.card_opacity_percent, theme_dark, accent,
                     drop_target, group_hovered, opacity_scale,
                     state.settings.glass_dispersion, 0.0f,
                     /*allow_lower_glass=*/true, /*is_group_card=*/true, grp_card_id,
                     &state.settings);

    const float hdr_bottom = rc.top + static_cast<float>(model::kGroupHeaderHeight);
    const uint32_t text_pri = dark ? 0xFAFFFFFFu : 0xFA1B2028u;
    const uint32_t text_sec = dark ? 0xA6FFFFFFu : 0x9E1E2430u;
    const bool installer_cleanable = IsInstallerGroup(group) && !effective_collapsed &&
                                     interaction.cleanable_installer_bytes > 0;
    const uint32_t hdr_band_override = installer_cleanable ? (dark ? 0x26D07C4Cu : 0x1ED97706u) : 0u;
    const uint32_t hdr_line_override = installer_cleanable ? (dark ? 0x26E59A68u : 0x18B45309u) : 0u;
    // The title strip is its own layer, the content stays clear (fades with the material below 30 %).
    if (!effective_collapsed) {
        DrawHeaderBand(rc, hdr_bottom, 14.0f, dark, state.settings.card_opacity_percent,
                       hdr_band_override, hdr_line_override, mat == model::CardMaterial::Acrylic,
                       mat == model::CardMaterial::LiquidGlass);
    }

    const bool stacked = view.tab_group_ids.size() > 1;
    if (stacked) {
        const auto tabs = ComputeGroupTabLayout(view);
        for (const auto& tab : tabs.tabs) {
            const auto found = std::find_if(state.groups.begin(), state.groups.end(),
                [&](const auto& candidate) { return candidate.id == tab.group_id; });
            if (found == state.groups.end()) continue;
            const auto bounds = ToRectF(tab.rect);
            if (tab.active) d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(bounds, 6, 6), Brush(accent, 0.18f));
            DrawEllipsized(found->title, D2D1::RectF(bounds.left + 6, bounds.top, bounds.right - 6, bounds.bottom),
                fmt_group_title_.Get(), tab.active ? text_pri : text_sec);
            if (tab.active) d2d_dc_->FillRectangle(D2D1::RectF(bounds.left + 7, bounds.bottom - 2, bounds.right - 7, bounds.bottom), Brush(accent_fg));
        }
        const wchar_t previous[] = L"\xE76B", next[] = L"\xE76C";
        if (!tabs.previous.empty()) DrawGlyph(d2d_dc_.Get(), previous, 1, fmt_fluent_12_.Get(),
            ToRectF(tabs.previous), Brush(text_sec, tabs.previous_group_id.empty() ? 0.3f : 1.0f));
        if (!tabs.next.empty()) DrawGlyph(d2d_dc_.Get(), next, 1, fmt_fluent_12_.Get(),
            ToRectF(tabs.next), Brush(text_sec, tabs.next_group_id.empty() ? 0.3f : 1.0f));
    } else {
        // 1. Header Left: Fluent glyph (accent-fg) + Group Title + tightly adjacent Count Pill
        const wchar_t grp_glyph[2] = {group.icon_glyph != 0 ? group.icon_glyph : L'\xE8B7', 0};
        DrawGlyph(d2d_dc_.Get(), grp_glyph, 1, fmt_fluent_14_.Get(),
                          D2D1::RectF(rc.left + 12.0f, rc.top, rc.left + 30.0f, hdr_bottom),
                          Brush(accent_fg));

        const float title_x = rc.left + 34.0f;
        const float max_title_w = std::max(40.0f, (rc.right - rc.left) * 0.52f);
        const float title_w = std::min(max_title_w, MeasureSingleLineWidth(group.title, fmt_group_title_.Get(), max_title_w));
        if (mat == model::CardMaterial::LiquidGlass && state.settings.glass_frost < 35) {
            DrawWidgetText(group.title.c_str(), static_cast<UINT32>(group.title.size()),
                              fmt_group_title_.Get(),
                              D2D1::RectF(title_x, rc.top + 1.0f, title_x + title_w + 4.0f, hdr_bottom + 1.0f),
                              Brush(dark ? 0x5A000000u : 0x55FFFFFFu));
        }
        DrawWidgetText(group.title.c_str(), static_cast<UINT32>(group.title.size()),
                          fmt_group_title_.Get(),
                          D2D1::RectF(title_x, rc.top, title_x + title_w + 4.0f, hdr_bottom),
                          Brush(text_pri));

        wchar_t count_buf[24]{};
        std::swprintf(count_buf, ARRAYSIZE(count_buf), L"%zu", view.total_item_count);
        const float count_txt_w = MeasureSingleLineWidth(count_buf, fmt_pill_.Get(), 60.0f);
        const float count_pill_w = std::max(18.0f, count_txt_w + 10.0f);
        const float count_x = title_x + title_w + 7.0f;
        const D2D1_RECT_F count_rc = D2D1::RectF(count_x, rc.top + 10.0f, count_x + count_pill_w, rc.top + 28.0f);
        d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(count_rc, 9.0f, 9.0f),
                                      Brush(dark ? 0x1AFFFFFFu : 0x12000000u));
        DrawWidgetText(count_buf, static_cast<UINT32>(std::wcslen(count_buf)), fmt_pill_.Get(),
                          count_rc, Brush(text_sec));

    }

    // 2. Header Right: Action buttons (on hover or collapsed) + CleanChip / RuleChip
    const bool show_right_btns = group_hovered || effective_collapsed;
    if (show_right_btns) {
        // Collapse / Expand button (.gh .gb)
        const D2D1_RECT_F chev_rc = D2D1::RectF(rc.right - 62.0f, rc.top + 6.0f, rc.right - 36.0f, rc.top + 32.0f);
        if (interaction.hovered_group_zone == GroupHeaderHitZone::CollapseChevron && group_hovered) {
            d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(chev_rc, 7.0f, 7.0f),
                                          Brush(dark ? 0x1EFFFFFFu : 0x12000000u));
        }
        const wchar_t chev_glyph[2] = {effective_collapsed ? L'\xE70D' : L'\xE70E', 0};
        DrawGlyph(d2d_dc_.Get(), chev_glyph, 1, fmt_fluent_12_.Get(), chev_rc, Brush(text_sec));

        // More (...) button (.gh .gb)
        const D2D1_RECT_F menu_rc = D2D1::RectF(rc.right - 34.0f, rc.top + 6.0f, rc.right - 8.0f, rc.top + 32.0f);
        if (interaction.hovered_group_zone == GroupHeaderHitZone::MenuButton && group_hovered) {
            d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(menu_rc, 7.0f, 7.0f),
                                          Brush(dark ? 0x1EFFFFFFu : 0x12000000u));
        }
        const wchar_t more_glyph[2] = {L'\xE712', 0};
        DrawGlyph(d2d_dc_.Get(), more_glyph, 1, fmt_fluent_12_.Get(), menu_rc, Brush(text_sec));
    }

    if (!stacked) {
        const float chip_right = rc.right - (actual_frame.w >= 260 ? 68.0f : (show_right_btns ? 68.0f : 12.0f));
        if (installer_cleanable && actual_frame.w >= 260) {
            const std::wstring chip_txt = L"可清理 " + FormatBytesCompact(interaction.cleanable_installer_bytes);
            const float txt_w = MeasureSingleLineWidth(chip_txt, fmt_pill_.Get(), 200.0f);
            const float chip_w = txt_w + 28.0f;
            const D2D1_RECT_F chip_rc = D2D1::RectF(chip_right - chip_w, rc.top + 8.0f, chip_right, rc.top + 30.0f);
            const bool chip_hot = group_hovered && (interaction.hovered_group_zone == GroupHeaderHitZone::CleanChip);
            d2d_dc_->FillRoundedRectangle(
                D2D1::RoundedRect(chip_rc, 8.0f, 8.0f),
                chip_hot ? Brush(accent, 0.22f) : Brush(dark ? 0x18FFFFFFu : 0x10000000u));
            const wchar_t trash_g[2] = {L'\xE74D', 0};
            DrawGlyph(d2d_dc_.Get(), trash_g, 1, fmt_fluent_12_.Get(),
                      D2D1::RectF(chip_rc.left + 6.0f, chip_rc.top, chip_rc.left + 20.0f, chip_rc.bottom),
                      Brush(accent_fg));
            DrawWidgetText(chip_txt.c_str(), static_cast<UINT32>(chip_txt.size()), fmt_pill_.Get(),
                              D2D1::RectF(chip_rc.left + 20.0f, chip_rc.top, chip_rc.right - 6.0f, chip_rc.bottom),
                              Brush(accent_fg));
        } else if (!effective_collapsed && !view.single_subfolder_path.empty() && actual_frame.w >= 260) {
            const std::wstring chip_txt = L"展开文件夹内容";
            const float txt_w = MeasureSingleLineWidth(chip_txt, fmt_pill_.Get(), 160.0f);
            const float chip_w = txt_w + 28.0f;
            const D2D1_RECT_F chip_rc = D2D1::RectF(chip_right - chip_w, rc.top + 8.0f, chip_right, rc.top + 30.0f);
            const bool chip_hot = group_hovered && (interaction.hovered_group_zone == GroupHeaderHitZone::MapSingleFolderChip);
            d2d_dc_->FillRoundedRectangle(
                D2D1::RoundedRect(chip_rc, 8.0f, 8.0f),
                chip_hot ? Brush(accent, 0.24f) : Brush(accent, 0.14f));
            const wchar_t fld_g[2] = {L'\xE8B7', 0};
            DrawGlyph(d2d_dc_.Get(), fld_g, 1, fmt_fluent_12_.Get(),
                      D2D1::RectF(chip_rc.left + 6.0f, chip_rc.top, chip_rc.left + 20.0f, chip_rc.bottom),
                      Brush(accent_fg));
            DrawWidgetText(chip_txt.c_str(), static_cast<UINT32>(chip_txt.size()), fmt_pill_.Get(),
                              D2D1::RectF(chip_rc.left + 20.0f, chip_rc.top, chip_rc.right - 6.0f, chip_rc.bottom),
                              Brush(accent_fg));
        } else if (group_hovered && !group.mapped_folder_path.empty() && actual_frame.w >= 260) {
            const std::wstring chip_txt = L"目录 · " + MappedFolderLeafName(group.mapped_folder_path);
            const float txt_w = std::min(152.0f, MeasureSingleLineWidth(chip_txt, fmt_pill_.Get(), 152.0f));
            const float chip_w = txt_w + 28.0f;
            const D2D1_RECT_F chip_rc = D2D1::RectF(chip_right - chip_w, rc.top + 8.0f, chip_right, rc.top + 30.0f);
            const bool chip_hot = (interaction.hovered_group_zone == GroupHeaderHitZone::RuleButton);
            d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(chip_rc, 8.0f, 8.0f),
                                          Brush(dark ? 0x20FFFFFFu : 0x12000000u, chip_hot ? 1.4f : 1.0f));
            const wchar_t fld_g[2] = {L'\xE8B7', 0};
            DrawGlyph(d2d_dc_.Get(), fld_g, 1, fmt_fluent_12_.Get(),
                      D2D1::RectF(chip_rc.left + 6.0f, chip_rc.top, chip_rc.left + 20.0f, chip_rc.bottom),
                      Brush(accent_fg));
            DrawWidgetText(chip_txt.c_str(), static_cast<UINT32>(chip_txt.size()),
                              fmt_caption_.Get(),
                              D2D1::RectF(chip_rc.left + 21.0f, chip_rc.top, chip_rc.right - 7.0f, chip_rc.bottom),
                              Brush(text_sec));
        } else if (group_hovered && !group.rule_summary.empty() && actual_frame.w >= 260) {
            const float txt_w = std::min(152.0f, MeasureSingleLineWidth(group.rule_summary, fmt_pill_.Get(), 152.0f));
            const float chip_w = txt_w + 28.0f;
            const D2D1_RECT_F chip_rc = D2D1::RectF(chip_right - chip_w, rc.top + 8.0f, chip_right, rc.top + 30.0f);
            const bool chip_hot = (interaction.hovered_group_zone == GroupHeaderHitZone::RuleButton);
            d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(chip_rc, 8.0f, 8.0f),
                                          Brush(dark ? 0x20FFFFFFu : 0x12000000u, chip_hot ? 1.4f : 1.0f));
            const wchar_t filter_g[2] = {L'\xE71C', 0};
            DrawGlyph(d2d_dc_.Get(), filter_g, 1, fmt_fluent_12_.Get(),
                      D2D1::RectF(chip_rc.left + 6.0f, chip_rc.top, chip_rc.left + 20.0f, chip_rc.bottom),
                      Brush(text_sec));
            DrawWidgetText(group.rule_summary.c_str(), static_cast<UINT32>(group.rule_summary.size()),
                              fmt_caption_.Get(),
                              D2D1::RectF(chip_rc.left + 21.0f, chip_rc.top, chip_rc.right - 7.0f, chip_rc.bottom),
                              Brush(text_sec));
        }

    }

    if (!effective_collapsed) {
        if (view.view_mode == model::GroupViewMode::Details) {
            const auto& content = view.content_rect;
            const auto header = D2D1::RectF(static_cast<float>(content.x), static_cast<float>(content.y),
                static_cast<float>(content.right()), static_cast<float>(content.y + view.details_header_height));
            const auto columns = DetailsColumns(header);
            DrawEllipsized(l10n::Get(l10n::StringId::ColumnName), columns.name, fmt_row_meta_.Get(), text_sec);
            DrawEllipsized(l10n::Get(l10n::StringId::ColumnSize), columns.size, fmt_row_meta_.Get(), text_sec);
            DrawEllipsized(l10n::Get(l10n::StringId::ColumnModified), columns.modified, fmt_row_meta_.Get(), text_sec);
            d2d_dc_->DrawLine(D2D1::Point2F(header.left, header.bottom), D2D1::Point2F(header.right, header.bottom),
                Brush(text_sec, 0.2f));
        }

        if (view.total_item_count == 0) {
            const wchar_t* empty_msg = L"把文件拖到这里";
            if (!group.mapped_folder_path.empty()) {
                const DWORD attrs = GetFileAttributesW(group.mapped_folder_path.c_str());
                empty_msg = (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0)
                                ? L"关联的文件夹不存在或已移动"
                                : L"文件夹为空 · 拖入文件将存入此目录";
            }
            DrawWidgetText(empty_msg, static_cast<UINT32>(std::wcslen(empty_msg)), fmt_empty_hint_.Get(),
                              D2D1::RectF(rc.left + 12.0f, hdr_bottom, rc.right - 12.0f, rc.bottom - 8.0f),
                              Brush(text_sec, 0.72f));
        } else {
            const bool has_search = !interaction.search_highlight_query.empty();

            std::unordered_set<std::wstring> staged_paths;
            for (const auto& te : live_data.tray_entries) staged_paths.insert(te.path);

            for (size_t cell_i = 0; cell_i < view.visible_item_indices.size(); ++cell_i) {
                const size_t item_idx = view.visible_item_indices[cell_i];
                if (item_idx >= state.items.size()) continue;
                const auto& item = state.items[item_idx];
                const auto item_rect = model::GroupItemRect(view, cell_i);
                if (item_rect.empty()) continue;
                const auto cell_rc = ToRectF(item_rect);

                const bool selected = interaction.selected_paths.contains(item.path);
                const bool hovered = (interaction.hovered_path == item.path);
                const bool matched = has_search && ContainsCaseInsensitive(item.name, interaction.search_highlight_query);
                const bool is_staged = staged_paths.contains(item.path);
                if (view.view_mode == model::GroupViewMode::Tiles || view.view_mode == model::GroupViewMode::Details) {
                    DrawGroupRow(cell_rc, item, view.view_mode, dark, accent, selected, hovered, matched, has_search && !matched);
                    continue;
                }
                DrawIconCell(cell_rc, item, /*in_group=*/true, is_staged,
                             view.view_mode == model::GroupViewMode::LargeIcons ? model::DesktopIconSize::Large : state.settings.icon_size,
                             state.settings.label_style,
                             dark, accent, selected, hovered, matched, has_search && !matched,
                             &state.settings, mat);
            }
        }

        if (const GroupFooterLayout footer = ComputeGroupFooterLayout(actual_frame, view); footer.active) {
            d2d_dc_->DrawLine(D2D1::Point2F(rc.left + 12.0f, footer.fy),
                              D2D1::Point2F(rc.right - 12.0f, footer.fy),
                              Brush(dark ? 0x12FFFFFFu : 0x0B000000u), 1.0f);

            const bool prev_enabled = footer.cur_page > 0;
            const bool prev_hot = group_hovered && prev_enabled &&
                                  (interaction.hovered_group_zone == GroupHeaderHitZone::FooterPrevPage);
            if (prev_hot) {
                d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(footer.prev_rc, 6.0f, 6.0f),
                                              Brush(dark ? 0x1EFFFFFFu : 0x12000000u));
            }
            const wchar_t lchev[2] = {L'\xE76B', 0};
            DrawGlyph(d2d_dc_.Get(), lchev, 1, fmt_fluent_12_.Get(), footer.prev_rc,
                      Brush(prev_enabled ? text_sec : text_sec, prev_enabled ? 0.92f : 0.30f));

            for (int k = 0; k < footer.shown_dots; ++k) {
                const int p = footer.start_page + k;
                const bool is_cur = (p == footer.cur_page);
                const bool dot_hot = group_hovered &&
                                     (interaction.hovered_group_zone == GroupHeaderHitZone::FooterPagerDots) &&
                                     (interaction.hovered_group_dot == p);
                const float alpha = is_cur ? 0.90f : (dot_hot ? 0.62f : 0.30f);
                d2d_dc_->FillRoundedRectangle(
                    D2D1::RoundedRect(footer.dot_rects[static_cast<size_t>(k)], 3.0f, 3.0f),
                    Brush(is_cur ? accent : text_pri, alpha));
            }

            if (footer.show_fraction) {
                DrawWidgetText(footer.fraction_text.c_str(),
                                  static_cast<UINT32>(footer.fraction_text.size()),
                                  fmt_pill_.Get(), footer.fraction_rc, Brush(text_sec, 0.85f));
            }

            const bool next_enabled = (footer.cur_page + 1 < footer.page_count);
            const bool next_hot = group_hovered && next_enabled &&
                                  (interaction.hovered_group_zone == GroupHeaderHitZone::FooterNextPage);
            if (next_hot) {
                d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(footer.next_rc, 6.0f, 6.0f),
                                              Brush(dark ? 0x1EFFFFFFu : 0x12000000u));
            }
            const wchar_t rchev[2] = {L'\xE76C', 0};
            DrawGlyph(d2d_dc_.Get(), rchev, 1, fmt_fluent_12_.Get(), footer.next_rc,
                      Brush(next_enabled ? text_sec : text_sec, next_enabled ? 0.92f : 0.30f));

            const bool view_all_hot = group_hovered &&
                                      (interaction.hovered_group_zone == GroupHeaderHitZone::FooterViewAll);
            if (view_all_hot) {
                d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(footer.view_all_rc, 6.0f, 6.0f),
                                              Brush(accent, 0.14f));
            }
            if (!footer.view_all_text.empty()) {
                DrawWidgetText(footer.view_all_text.c_str(),
                                  static_cast<UINT32>(footer.view_all_text.size()),
                                  fmt_caption_.Get(),
                                  D2D1::RectF(footer.view_all_rc.left + 4.0f, footer.view_all_rc.top,
                                              footer.view_all_rc.right - 16.0f, footer.view_all_rc.bottom),
                                  Brush(accent_fg, view_all_hot ? 1.0f : 0.88f));
            }
            DrawGlyph(d2d_dc_.Get(), rchev, 1, fmt_fluent_12_.Get(),
                      D2D1::RectF(footer.view_all_rc.right - 16.0f, footer.view_all_rc.top,
                                  footer.view_all_rc.right, footer.view_all_rc.bottom),
                      Brush(accent_fg, view_all_hot ? 1.0f : 0.88f));
        }

        // Subtle resize grip cue only when pointer hovers the bottom-right resize zone
        if (group_hovered && interaction.hovered_group_zone == GroupHeaderHitZone::ResizeGrip) {
            DrawCircularArc(d2d_factory_.Get(), d2d_dc_.Get(),
                            Brush(accent, 0.85f),
                            rc.right - 14.0f, rc.bottom - 14.0f, 8.5f, 15.0f, 65.0f, 1.8f, round_stroke_.Get());
        }
    }

    if (opacity_scale < 0.99f) {
        d2d_dc_->PopLayer();
    }
}

void DesktopRenderer::DrawIconCell(const D2D1_RECT_F& cell_rect,
                                   const model::DesktopItem& item,
                                   bool in_group,
                                   bool is_staged,
                                   model::DesktopIconSize icon_size,
                                   model::IconLabelStyle label_style,
                                   bool dark_mode,
                                   uint32_t accent_argb,
                                   bool selected,
                                   bool hovered,
                                   bool search_matched,
                                   bool search_dimmed, const model::BeautifySettings* surface_settings,
                                   model::CardMaterial surface_material) {
    const float alpha_mul = search_dimmed ? 0.25f : 1.0f;
    const D2D1_RECT_F pad_rc = D2D1::RectF(cell_rect.left + 3.0f, cell_rect.top + 2.0f,
                                           cell_rect.right - 3.0f, cell_rect.bottom - 2.0f);
    if (selected || hovered || search_matched) {
        const uint32_t fill = (selected || search_matched)
                                  ? accent_argb
                                  : (dark_mode ? 0xFFFFFFFFu : 0xFF000000u);
        const float fill_a = selected ? 0.22f : (search_matched ? 0.26f : 0.08f);
        d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(pad_rc, 8.0f, 8.0f), Brush(fill, fill_a));
        if (selected || search_matched) {
            d2d_dc_->DrawRoundedRectangle(D2D1::RoundedRect(pad_rc, 8.0f, 8.0f),
                                          Brush(accent_argb, 0.80f), 1.1f);
        }
    }

    float icon_dim = 44.0f;
    if (icon_size == model::DesktopIconSize::Small) icon_dim = 32.0f;
    else if (icon_size == model::DesktopIconSize::Large) icon_dim = 60.0f;

    const float cx = (cell_rect.left + cell_rect.right) * 0.5f;
    const float icon_top = cell_rect.top + (icon_size == model::DesktopIconSize::Small ? 6.0f : 8.0f);
    const D2D1_RECT_F icon_rc = D2D1::RectF(cx - icon_dim * 0.5f, icon_top,
                                            cx + icon_dim * 0.5f, icon_top + icon_dim);

    // icon_dim is DIPs; the bitmap is requested at the pixels it will cover.
    if (ID2D1Bitmap1* bmp = backdrop_.GetItemBitmap(d2d_dc_.Get(), item.path, item.is_folder,
                                                    static_cast<uint32_t>(std::lround(icon_dim * dip_scale_)))) {
        const D2D1_SIZE_F sz = bmp->GetSize();
        float dw = icon_dim;
        float dh = icon_dim;
        if (sz.width > 0.0f && sz.height > 0.0f) {
            const float scale = std::min(icon_dim / sz.width, icon_dim / sz.height);
            dw = sz.width * scale;
            dh = sz.height * scale;
        }
        const D2D1_RECT_F dst = D2D1::RectF(
            cx - dw * 0.5f, icon_top + (icon_dim - dh) * 0.5f,
            cx + dw * 0.5f, icon_top + (icon_dim + dh) * 0.5f);
        d2d_dc_->DrawBitmap(bmp, dst, alpha_mul, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else {
        DrawVectorFileIcon(d2d_factory_.Get(), d2d_dc_.Get(), dwrite_factory_.Get(),
                           icon_rc, item.extension.empty() ? std::wstring_view(item.name) : std::wstring_view(item.extension),
                           item.is_folder, alpha_mul);
    }

    // Staged indicator capsule (top-left of icon)
    if (is_staged) {
        const D2D1_RECT_F st_rc = D2D1::RectF(icon_rc.left - 3.0f, icon_rc.top - 2.0f,
                                              icon_rc.left + 15.0f, icon_rc.top + 15.0f);
        d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(st_rc, 5.0f, 5.0f),
                                      Brush(dark_mode ? 0xE61B1E28u : 0xF0FFFFFFu, alpha_mul));
        const wchar_t tray_g[2] = {L'\xE7B8', 0};
        DrawGlyph(d2d_dc_.Get(), tray_g, 1, fmt_fluent_12_.Get(), st_rc, Brush(accent_argb, alpha_mul));
    }

    // Pulse color tag dot (top-right of icon with 2px halo ring)
    if ((item.tag_argb & 0x00FFFFFFu) != 0) {
        const D2D1_POINT_2F dot_pt = D2D1::Point2F(icon_rc.right - 2.0f, icon_rc.top + 4.0f);
        d2d_dc_->FillEllipse(D2D1::Ellipse(dot_pt, 6.0f, 6.0f),
                             Brush(dark_mode ? 0xD81E2028u : 0xE6F6F7F9u, alpha_mul));
        d2d_dc_->FillEllipse(D2D1::Ellipse(dot_pt, 4.5f, 4.5f),
                             Brush(0xFF000000u | (item.tag_argb & 0x00FFFFFFu), alpha_mul));
    }

    if (item.is_shortcut) {
        const D2D1_RECT_F badge = D2D1::RectF(icon_rc.left, icon_rc.bottom - 13.0f,
                                              icon_rc.left + 13.0f, icon_rc.bottom);
        d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(badge, 3.5f, 3.5f),
                                      Brush(item.link_target_missing ? 0xFFE53935u : 0xE61E2430u, alpha_mul));
        const wchar_t sc_g[2] = {item.link_target_missing ? L'\xE7BA' : L'\xE71B', 0};
        DrawGlyph(d2d_dc_.Get(), sc_g, 1, fmt_fluent_12_.Get(), badge, Brush(0xFFFFFFFFu, alpha_mul));
    }

    const D2D1_RECT_F label_rc = D2D1::RectF(cell_rect.left + 4.0f, icon_rc.bottom + 4.0f,
                                             cell_rect.right - 4.0f, cell_rect.bottom - 3.0f);

    const auto background = surface_settings ? CardBackgroundSamples(label_rc, surface_material,
        surface_settings->card_opacity_percent, surface_settings->dark_mode) : backdrop_.SampleColors(label_rc);
    DrawIconLabel(item.name, label_rc, fmt_item_label_.Get(), label_style,
                  dark_mode, in_group, hovered || selected || search_matched, alpha_mul, &background);
}

void DesktopRenderer::DrawEllipsized(std::wstring_view text, const D2D1_RECT_F& rect,
                                      IDWriteTextFormat* format, uint32_t color, float opacity) {
    if (rect.right <= rect.left || rect.bottom <= rect.top || text.empty()) return;
    widget_text_.Bind(dwrite_factory_.Get(),d2d_dc_.Get());
    auto* layout=widget_text_.Layout(dwrite_factory_.Get(),text,format,rect.right-rect.left,rect.bottom-rect.top,true);
    widget_text_.DrawLayout(dwrite_factory_.Get(),d2d_dc_.Get(),{rect.left,rect.top},layout,Brush(color,opacity),D2D1_DRAW_TEXT_OPTIONS_CLIP);

}

void DesktopRenderer::DrawGroupRow(const D2D1_RECT_F& rect, const model::DesktopItem& item,
                                    model::GroupViewMode mode, bool dark, uint32_t accent,
                                    bool selected, bool hovered, bool matched, bool dimmed) {
    const float opacity = dimmed ? 0.35f : 1.0f;
    const auto row = D2D1::RectF(rect.left + 2, rect.top + 1, rect.right - 2, rect.bottom - 1);
    if (selected || hovered || matched) {
        d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(row, 6, 6),
            Brush(selected ? accent : (dark ? 0x20FFFFFFu : 0x14000000u), selected ? 0.22f : 1.0f));
        if (selected || matched) d2d_dc_->DrawRoundedRectangle(D2D1::RoundedRect(row, 6, 6), Brush(accent, 0.65f), 1);
    }
    const bool details = mode == model::GroupViewMode::Details;
    const float size = details ? 20.0f : 36.0f;
    const float cy = (rect.top + rect.bottom) * 0.5f;
    const auto icon = D2D1::RectF(rect.left + 6, cy - size / 2, rect.left + 6 + size, cy + size / 2);
    if (auto* bitmap = backdrop_.GetItemBitmap(d2d_dc_.Get(), item.path, item.is_folder,
        static_cast<uint32_t>(std::lround(size * dip_scale_)))) {
        const auto bitmap_size = bitmap->GetSize();
        const float ratio = std::min(size / std::max(1.0f, bitmap_size.width), size / std::max(1.0f, bitmap_size.height));
        const float width = bitmap_size.width * ratio, height = bitmap_size.height * ratio;
        const float cx = (icon.left + icon.right) / 2;
        d2d_dc_->DrawBitmap(bitmap, D2D1::RectF(cx - width / 2, cy - height / 2, cx + width / 2, cy + height / 2),
            opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else {
        DrawVectorFileIcon(d2d_factory_.Get(), d2d_dc_.Get(), dwrite_factory_.Get(), icon,
            item.extension, item.is_folder, opacity);
    }
    const uint32_t primary = dark ? 0xFAFFFFFFu : 0xFA1B2028u;
    const uint32_t secondary = dark ? 0xBFFFFFFFu : 0xB01E2430u;
    if (details) {
        const auto columns = DetailsColumns(rect);
        DrawEllipsized(item.name, columns.name, fmt_row_name_.Get(), primary, opacity);
        if (!item.is_folder) DrawEllipsized(FormatBytesCompact(item.size_bytes), columns.size, fmt_row_meta_.Get(), secondary, opacity);
        DrawEllipsized(GroupModifiedDate(item.modified_unix_ms), columns.modified, fmt_row_meta_.Get(), secondary, opacity);
    } else {
        const float left = icon.right + 8;
        DrawEllipsized(item.name, D2D1::RectF(left, rect.top + 4, rect.right - 6, cy + 1), fmt_row_name_.Get(), primary, opacity);
        std::wstring metadata = item.is_folder ? l10n::Get(l10n::StringId::TypeFolder) : item.extension;
        if (!item.is_folder) metadata += (metadata.empty() ? L"" : L" · ") + FormatBytesCompact(item.size_bytes);
        DrawEllipsized(metadata, D2D1::RectF(left, cy + 1, rect.right - 6, rect.bottom - 4), fmt_row_meta_.Get(), secondary, opacity);
    }
}

void DesktopRenderer::DrawWidgetText(const wchar_t* text,UINT32 length,IDWriteTextFormat* format,
    const D2D1_RECT_F& bounds,ID2D1SolidColorBrush* brush,D2D1_DRAW_TEXT_OPTIONS options) {
    widget_text_.DrawText(dwrite_factory_.Get(),d2d_dc_.Get(),text,length,format,bounds,brush,options);
}
void DesktopRenderer::DrawLabelLayout(D2D1_POINT_2F origin, IDWriteTextLayout* layout,
                                       uint32_t color, float opacity) {
    widget_text_.DrawLayout(dwrite_factory_.Get(),d2d_dc_.Get(),origin,layout,Brush(color,opacity),
        D2D1_DRAW_TEXT_OPTIONS_CLIP | D2D1_DRAW_TEXT_OPTIONS_NO_SNAP);
}

void DesktopRenderer::DrawIconLabel(std::wstring_view name, const D2D1_RECT_F& label_rc,
                                    IDWriteTextFormat* format, model::IconLabelStyle style,
                                    bool dark_mode, bool card_backed, bool revealed, float alpha_mul,
                                    const text_contrast::Samples* background) {
    if (style == model::IconLabelStyle::HiddenUntilHover && !revealed) return;
    if (label_rc.right <= label_rc.left || label_rc.bottom <= label_rc.top || name.empty()) return;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_factory_->CreateTextLayout(name.data(), static_cast<UINT32>(name.size()), format,
        label_rc.right - label_rc.left, label_rc.bottom - label_rc.top, &layout))) return;

    // All passes share one layout and one device-pixel origin. A dirty-surface
    // translation or fractional DPI must not independently round shadow and ink.
    auto origin = D2D1::Point2F(label_rc.left, label_rc.top);
    float dpi_x = 96, dpi_y = 96;
    d2d_dc_->GetDpi(&dpi_x, &dpi_y);
    D2D1_MATRIX_3X2_F transform{};
    d2d_dc_->GetTransform(&transform);
    const float units_x = d2d_dc_->GetUnitMode() == D2D1_UNIT_MODE_PIXELS ? 1.0f : dpi_x / 96.0f;
    const float units_y = d2d_dc_->GetUnitMode() == D2D1_UNIT_MODE_PIXELS ? 1.0f : dpi_y / 96.0f;
    float pixel_x = 1.0f / units_x, pixel_y = 1.0f / units_y;
    if (std::abs(transform._12) < 0.0001f && std::abs(transform._21) < 0.0001f &&
        transform._11 > 0.0001f && transform._22 > 0.0001f) {
        pixel_x /= transform._11;
        pixel_y /= transform._22;
        origin.x = (std::round((origin.x * transform._11 + transform._31) * units_x) / units_x - transform._31) / transform._11;
        origin.y = (std::round((origin.y * transform._22 + transform._32) * units_y) / units_y - transform._32) / transform._22;
    }
    auto samples = background ? *background : backdrop_.SampleColors(label_rc);
    const auto ink_alpha = static_cast<uint32_t>(std::lround(255.0f * std::clamp(alpha_mul, 0.0f, 1.0f))) << 24;
    const auto light_ink = ink_alpha | (text_contrast::kLightInk & 0xFFFFFFu);
    const auto dark_ink = ink_alpha | (text_contrast::kDarkInk & 0xFFFFFFu);
    auto ink = text_contrast::ChooseInk(samples, light_ink, dark_ink, dark_mode);
    dark_mode = ink.light;
    if (style == model::IconLabelStyle::FrostedPill) {
        const auto pill = D2D1::RectF(label_rc.left, label_rc.top - 1, label_rc.right,
            std::min(label_rc.bottom + 1, label_rc.top + 31));
        const uint32_t plate = dark_mode ? 0xA8161922u : 0xD8FFFFFFu;
        d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(pill, 6, 6), Brush(plate, alpha_mul));
        text_contrast::TransformSamples(samples, [plate, alpha_mul](auto color) {
            return text_contrast::SourceOver(color, plate, alpha_mul);
        });
        ink = text_contrast::ChooseInk(samples, light_ink, dark_ink, dark_mode);
    } else if (style != model::IconLabelStyle::Normal) {
        if (ink.valid && std::max(ink.light_contrast, ink.dark_contrast) < 4.5f) {
            for (const auto delta : {D2D1::Point2F(-pixel_x, 0), D2D1::Point2F(pixel_x, 0),
                                     D2D1::Point2F(0, -pixel_y), D2D1::Point2F(0, pixel_y)}) {
                DrawLabelLayout(D2D1::Point2F(origin.x + delta.x, origin.y + delta.y), layout.Get(),
                    ink.light ? 0xE6000000u : 0xE6FFFFFFu, alpha_mul);
            }
        } else {
            DrawLabelLayout(D2D1::Point2F(origin.x + pixel_x, origin.y + pixel_y), layout.Get(),
                ink.light ? 0xCC000000u : (card_backed ? 0x70FFFFFFu : 0xCCFFFFFFu), alpha_mul);
        }
    }
    DrawLabelLayout(origin, layout.Get(), ink.light ? text_contrast::kLightInk : text_contrast::kDarkInk, alpha_mul);
}

void DesktopRenderer::DrawLooseIcons(const model::DesktopModelState& state,
                                     std::span<const model::GroupComputedView> group_views,
                                     const LiveTileRuntimeData& live_data,
                                     const DesktopInteractionState& interaction) {
    std::vector<uint8_t> grouped(state.items.size(), 0);
    if (state.settings.organize_enabled) {
        for (const auto& gv : group_views) {
            for (size_t idx : gv.member_item_indices) {
                if (idx < grouped.size()) grouped[idx] = 1;
            }
        }
        for (size_t i = 0; i < state.items.size(); ++i) {
            const auto& it = state.items[i];
            if (it.hidden_by_mapped_group || !it.mapped_owner_group_id.empty() ||
                it.in_apps_tile || (!it.force_loose && !it.pinned_group_id.empty() &&
                                    it.pinned_group_id != model::kAppsTileGroupId)) {
                grouped[i] = 1;
            }
        }
    } else {
        for (size_t i = 0; i < state.items.size(); ++i) {
            const auto& it = state.items[i];
            if (it.hidden_by_mapped_group || !it.mapped_owner_group_id.empty()) {
                grouped[i] = 1;
            }
        }
    }

    std::unordered_set<std::wstring> staged_paths;
    for (const auto& te : live_data.tray_entries) staged_paths.insert(te.path);

    const model::PointI cell_sz = model::IconCellMetrics(state.settings.icon_size);
    const float cw = static_cast<float>(std::max(cell_sz.x, model::kLooseCellW));
    const float ch = static_cast<float>(std::max(cell_sz.y, model::kLooseCellH));
    const uint32_t accent = ResolveAccentArgb(0, state.settings, interaction);
    const bool has_search = !interaction.search_highlight_query.empty();

    for (size_t i = 0; i < state.items.size(); ++i) {
        if (grouped[i]) continue;
        const auto& item = state.items[i];
        const model::RectI item_rc{item.loose_pos.x, item.loose_pos.y, static_cast<int>(cw), static_cast<int>(ch)};
        if (!IntersectsActiveClip(item_rc, 8)) continue;
        const float x = static_cast<float>(item.loose_pos.x);
        const float y = static_cast<float>(item.loose_pos.y);
        const D2D1_RECT_F cell_rc = D2D1::RectF(x, y, x + cw, y + ch);

        const bool selected = interaction.selected_paths.contains(item.path);
        const bool hovered = (interaction.hovered_path == item.path);
        const bool matched = has_search && ContainsCaseInsensitive(item.name, interaction.search_highlight_query);
        const bool is_staged = staged_paths.contains(item.path);
        DrawIconCell(cell_rc, item, /*in_group=*/false, is_staged,
                     state.settings.icon_size, state.settings.label_style,
                     state.settings.dark_mode, accent, selected, hovered, matched, has_search && !matched);
    }
}

// The − badge outside the top-left corner, subtle corner resize arc, and hover/resize size pill.
void DesktopRenderer::DrawTileEditDecor(const model::DesktopTile& tile, const D2D1_RECT_F& r, bool dark,
                                        bool hovered, bool resizing, uint32_t accent_argb) {
    const D2D1_RECT_F badge = TileRemoveBadgeRect(r);
    d2d_dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F((badge.left + badge.right) * 0.5f, (badge.top + badge.bottom) * 0.5f), 11.0f, 11.0f),
                         Brush(dark ? 0xF5262A33u : 0xF8FFFFFFu));
    d2d_dc_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F((badge.left + badge.right) * 0.5f, (badge.top + badge.bottom) * 0.5f), 11.0f, 11.0f),
                         Brush(dark ? 0x30FFFFFFu : 0x22000000u), 1.0f);
    const wchar_t minus[2] = {L'\xE738', 0};
    DrawGlyph(d2d_dc_.Get(), minus, 1, fmt_fluent_12_.Get(), badge, Brush(dark ? 0xFFFFFFFFu : 0xFF1B1B1Bu));

    DrawCircularArc(d2d_factory_.Get(), d2d_dc_.Get(),
                    Brush(accent_argb, (hovered || resizing) ? 0.90f : 0.45f),
                    r.right - 14.0f, r.bottom - 14.0f, 8.5f, 15.0f, 65.0f, 1.8f, round_stroke_.Get());

    if (!hovered && !resizing) return;
    wchar_t sz[24]{};
    const int pw = static_cast<int>(std::lround(r.right - r.left));
    const int ph = static_cast<int>(std::lround(r.bottom - r.top));
    const bool show_px = resizing || tile.w > 0 || tile.h > 0;
    if (show_px) {
        std::swprintf(sz, ARRAYSIZE(sz), L"%d×%d", pw, ph);
    } else {
        std::swprintf(sz, ARRAYSIZE(sz), L"%d×%d", tile.size.cols, tile.size.rows);
    }
    const D2D1_RECT_F pill = TileSizePillRect(r, show_px);
    d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(pill, 11.0f, 11.0f), Brush(dark ? 0xF5262A33u : 0xF8FFFFFFu));
    DrawWidgetText(sz, static_cast<UINT32>(std::wcslen(sz)), fmt_pill_.Get(), pill, Brush(dark ? 0xFFFFFFFFu : 0xFF1B1B1Bu));
}
D2D1_RECT_F DesktopRenderer::TileRemoveBadgeRect(const D2D1_RECT_F& r) noexcept {
    return D2D1::RectF(r.left - 9.0f, r.top - 9.0f, r.left + 13.0f, r.top + 13.0f);
}
DesktopRenderer::AppsPaging DesktopRenderer::AppsTilePaging(const model::DesktopTile& tile, const D2D1_RECT_F& rect,
                                                            size_t count) noexcept {
    AppsPaging p;
    const float w = std::max(1.0f, rect.right - rect.left);
    p.top = rect.top + (tile.look.show_title ? 48.0f : 12.0f);
    if (w >= 390.0f) {
        p.cols = std::clamp(static_cast<int>((w - 16.0f) / 80.0f), 5, 12);
    } else if (w >= 260.0f) {
        p.cols = std::clamp(static_cast<int>((w - 16.0f) / 82.0f), 3, 4);
    } else {
        p.cols = 2;
    }
    // As many rows as fit at the compact cell height (icon 36 + one label line), then the cells share the
    // height out evenly so a page fills the tile instead of leaving an empty band under one row.
    constexpr float kMinCell = 62.0f, kMaxCell = 80.0f;
    const auto fit = [&](float bottom_pad) {
        const float avail = std::max(0.0f, rect.bottom - bottom_pad - p.top);
        p.rows = std::max(1, static_cast<int>(avail / kMinCell));
        p.cell_h = std::clamp(avail / static_cast<float>(p.rows), kMinCell, kMaxCell);
        p.room = static_cast<size_t>(p.cols * p.rows);
    };
    fit(8.0f);
    if (count > p.room) fit(22.0f);  // leave room for the page dots under the grid
    p.pages = std::max(1, static_cast<int>((count + p.room - 1) / p.room));
    return p;
}

size_t DesktopRenderer::TrayChipCapacity(const model::DesktopTile& tile, const D2D1_RECT_F& rect) noexcept {
    const float top = rect.top + (tile.look.show_title ? 48.0f : 12.0f);
    if (tile.size.rows < 2) return static_cast<size_t>(std::max(0.0f, (rect.right - 48.0f - (rect.left + 14.0f) + 7.0f) / 47.0f));
    const int cols = std::max(1, static_cast<int>((rect.right - rect.left - 20.0f) / 66.0f));
    const int rows = std::max(0, static_cast<int>((rect.bottom - 8.0f - top) / 72.0f));
    return static_cast<size_t>(cols * rows);
}
D2D1_RECT_F DesktopRenderer::TrayChipRect(const model::DesktopTile& tile, const D2D1_RECT_F& rect, size_t index) noexcept {
    if (index >= TrayChipCapacity(tile, rect)) return {};
    const float top = rect.top + (tile.look.show_title ? 48.0f : 12.0f);
    if (tile.size.rows < 2) {
        const float sx = rect.left + 14.0f + 47.0f * static_cast<float>(index);
        return D2D1::RectF(sx, top, sx + 40.0f, top + 40.0f);
    }
    const int cols = std::max(1, static_cast<int>((rect.right - rect.left - 20.0f) / 66.0f));
    const float cell_w = (rect.right - rect.left - 20.0f) / static_cast<float>(cols);
    const float x = rect.left + 10.0f + cell_w * static_cast<float>(index % static_cast<size_t>(cols));
    const float y = top + 72.0f * static_cast<float>(index / static_cast<size_t>(cols));
    return D2D1::RectF(x, y, x + cell_w, y + 68.0f);  // the whole cell: icon plate above, name below
}

D2D1_RECT_F DesktopRenderer::StackDotRect(const D2D1_RECT_F& r, size_t count, int active, size_t index) noexcept {
    const float n = static_cast<float>(count);
    const float total_h = (n - 1.0f) * 5.0f + 11.0f + (n - 1.0f) * 4.0f;
    float y = (r.top + r.bottom - total_h) * 0.5f;
    const float x = r.right - 8.5f;
    for (size_t i = 0; i < count; ++i) {
        const float dh = static_cast<int>(i) == active ? 11.0f : 5.0f;
        if (i == index) return D2D1::RectF(x - 2.5f, y, x + 2.5f, y + dh);
        y += dh + 4.0f;
    }
    return {};
}
D2D1_RECT_F DesktopRenderer::StackArrowRect(const D2D1_RECT_F& r, size_t count, bool up) noexcept {
    // A one-row card has no room for the [✏] button, both arrows and the dots: there the dots alone switch.
    if (r.bottom - r.top < 110.0f) return {};
    const float n = static_cast<float>(count);
    const float total_h = (n - 1.0f) * 5.0f + 11.0f + (n - 1.0f) * 4.0f;
    const float top = (r.top + r.bottom - total_h) * 0.5f;
    const float x = r.right - 8.5f;
    return up ? D2D1::RectF(x - 8.0f, top - 22.0f, x + 8.0f, top - 6.0f)
              : D2D1::RectF(x - 8.0f, top + total_h + 6.0f, x + 8.0f, top + total_h + 22.0f);
}

D2D1_RECT_F DesktopRenderer::PhotoNavRect(const model::DesktopTile& tile, const D2D1_RECT_F& r, bool next) noexcept {
    if (tile.size.cols < 2) return {};
    const float cy = (r.top + r.bottom) * 0.5f;
    const float right_pad = tile.stack.size() >= 2 ? 30.0f : 12.0f;
    const float cx = next ? r.right - right_pad - 14.0f : r.left + 26.0f;
    return D2D1::RectF(cx - 14.0f, cy - 14.0f, cx + 14.0f, cy + 14.0f);
}

D2D1_RECT_F DesktopRenderer::ListRowRect(model::TileKind kind, const D2D1_RECT_F& r, size_t index, bool show_title) noexcept {
    const float top = r.top + (show_title ? 46.0f : 12.0f);
    D2D1_RECT_F row{};
    if (kind == model::TileKind::Downloads) {
        // The newest download leads, taller and plated.
        row = index == 0 ? D2D1::RectF(r.left + 10.0f, top, r.right - 10.0f, top + 32.0f)
                         : D2D1::RectF(r.left + 10.0f, top + 36.0f + 28.0f * static_cast<float>(index - 1), r.right - 10.0f,
                                       top + 36.0f + 28.0f * static_cast<float>(index - 1) + 26.0f);
    } else {
        row = D2D1::RectF(r.left + 9.0f, top + 29.0f * static_cast<float>(index), r.right - 9.0f, top + 29.0f * static_cast<float>(index) + 26.0f);
    }
    return row.bottom <= r.bottom - 8.0f + (index == 0 ? 6.0f : 0.0f) ? row : D2D1_RECT_F{};
}
D2D1_RECT_F DesktopRenderer::ListRowActionRect(const D2D1_RECT_F& row) noexcept {
    const float cy = (row.top + row.bottom) * 0.5f;
    return D2D1::RectF(row.right - 30.0f, cy - 11.0f, row.right - 6.0f, cy + 11.0f);
}
D2D1_RECT_F DesktopRenderer::TodoDeleteRect(const model::DesktopTile& tile, const D2D1_RECT_F& r, size_t index) noexcept {
    const float top = (tile.look.show_title ? r.top + 46.0f : r.top + 12.0f) + 29.0f * static_cast<float>(index);
    return D2D1::RectF(r.right - 34.0f, top + 3.0f, r.right - 12.0f, top + 24.0f);
}
D2D1_RECT_F DesktopRenderer::RecycleEmptyRect(const D2D1_RECT_F& r) noexcept {
    const float cx = (r.left + r.right) * 0.5f;
    return D2D1::RectF(cx - 26.0f, r.bottom - 28.0f, cx + 26.0f, r.bottom - 8.0f);
}

D2D1_RECT_F DesktopRenderer::TileSizePillRect(const D2D1_RECT_F& r, bool pixel_mode) noexcept {
    const float cx = (r.left + r.right) * 0.5f;
    const float half_w = pixel_mode ? 34.0f : 24.0f;
    return D2D1::RectF(cx - half_w, r.bottom - 29.0f, cx + half_w, r.bottom - 7.0f);
}
D2D1_RECT_F DesktopRenderer::TileEditButtonRect(const D2D1_RECT_F& r, bool small_tile) noexcept {
    const float sz = small_tile ? 22.0f : 26.0f;
    const float pad = small_tile ? 6.0f : 7.0f;
    return D2D1::RectF(r.right - pad - sz, r.top + pad, r.right - pad, r.top + pad + sz);
}
model::RectI DesktopRenderer::TileHeaderDoneRect(const model::RectI& board_rect) noexcept {
    return model::RectI{board_rect.right() - 52, board_rect.y - 30, 52, 24};
}
model::RectI DesktopRenderer::TileHeaderAddWidgetRect(const model::RectI& board_rect) noexcept {
    return model::RectI{board_rect.right() - 52 - 8 - 102, board_rect.y - 30, 102, 24};
}

void DesktopRenderer::DrawTileBoardHeader(const model::RectI& board_rect, bool dark_mode, uint32_t accent_argb,
                                          bool edit_mode, bool show_idle_caption, int hovered_header_btn, float opacity) {
    if (!edit_mode && !show_idle_caption) return;
    const float x = static_cast<float>(board_rect.x + 2);
    const float y = static_cast<float>(board_rect.y - 28);
    const uint32_t pri = dark_mode ? 0xE8FFFFFFu : 0xE81E2430u;
    const uint32_t sec = dark_mode ? 0x8AFFFFFFu : 0x8A202834u;

    const wchar_t* title = edit_mode ? L"磁贴" : L"固定";
    DrawWidgetText(title, static_cast<UINT32>(std::wcslen(title)), fmt_tile_hdr_.Get(),
                      D2D1::RectF(x, y, x + 42.0f, y + 20.0f), Brush(pri, opacity));
    const wchar_t* hint = edit_mode ? L"全桌面自由拖动 · 拖边角调宽高 · 拖到同卡上叠放"
                                    : L"全桌面自由拖动 · 边缘调宽高 · 右键自定义";
    const float hint_right = edit_mode ? static_cast<float>(TileHeaderAddWidgetRect(board_rect).x - 8)
                                       : static_cast<float>(board_rect.right());
    if (hint_right > x + 46.0f) {
        DrawWidgetText(hint, static_cast<UINT32>(std::wcslen(hint)), fmt_caption_.Get(),
                          D2D1::RectF(x + 38.0f, y, hint_right, y + 20.0f),
                          Brush(sec, opacity));
    }

    if (edit_mode) {
        const D2D1_RECT_F add_rc = ToRectF(TileHeaderAddWidgetRect(board_rect));
        const D2D1_RECT_F done_rc = ToRectF(TileHeaderDoneRect(board_rect));
        const bool add_hot = (hovered_header_btn == 1);
        const bool done_hot = (hovered_header_btn == 2);

        d2d_dc_->FillRoundedRectangle(
            D2D1::RoundedRect(add_rc, 12.0f, 12.0f),
            Brush(dark_mode ? (add_hot ? 0xE02A303Cu : 0xC41E222Cu)
                            : (add_hot ? 0xF5FFFFFFu : 0xE0F4F6FAu),
                  opacity));
        if (auto* nb = backdrop_.EnsureNoiseBrush(d2d_dc_.Get())) {
            d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(add_rc, 12.0f, 12.0f), nb);
        }
        d2d_dc_->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(add_rc.left + 0.5f, add_rc.top + 0.5f, add_rc.right - 0.5f, add_rc.bottom - 0.5f), 11.5f, 11.5f),
            Brush(dark_mode ? 0x2EFFFFFFu : 0x1C000000u, opacity), 1.0f);
        const wchar_t plus_g[2] = {L'\xE710', 0};
        DrawGlyph(d2d_dc_.Get(), plus_g, 1, fmt_fluent_12_.Get(),
                  D2D1::RectF(add_rc.left + 8.0f, add_rc.top, add_rc.left + 22.0f, add_rc.bottom),
                  Brush(pri, opacity));
        const wchar_t* add_lbl = L"添加小组件";
        DrawWidgetText(add_lbl, static_cast<UINT32>(std::wcslen(add_lbl)), fmt_tile_hdr_.Get(),
                          D2D1::RectF(add_rc.left + 24.0f, add_rc.top, add_rc.right - 6.0f, add_rc.bottom),
                          Brush(pri, opacity));

        const uint32_t pri_fill = done_hot ? 0xFF2896EAu : (0xFF000000u | (accent_argb & 0x00FFFFFFu));
        d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(done_rc, 12.0f, 12.0f), Brush(pri_fill, opacity));
        const wchar_t* done_lbl = L"完成";
        DrawWidgetText(done_lbl, static_cast<UINT32>(std::wcslen(done_lbl)), fmt_pill_.Get(),
                          done_rc, Brush(0xFFFFFFFFu, opacity));
    }
}

namespace {
float BlendLum(text_contrast::Srgb c) {
    return 0.30f * c.r + 0.59f * c.g + 0.11f * c.b;
}

text_contrast::Srgb SetBlendLum(text_contrast::Srgb c, float lum) {
    const float delta = lum - BlendLum(c);
    c = {c.r + delta, c.g + delta, c.b + delta};
    const float low = std::min({c.r, c.g, c.b});
    if (low < 0.0f) {
        const float scale = lum / (lum - low);
        c = {lum + (c.r - lum) * scale, lum + (c.g - lum) * scale, lum + (c.b - lum) * scale};
    }
    const float high = std::max({c.r, c.g, c.b});
    if (high > 1.0f) {
        const float scale = (1.0f - lum) / (high - lum);
        c = {lum + (c.r - lum) * scale, lum + (c.g - lum) * scale, lum + (c.b - lum) * scale};
    }
    return c;
}

text_contrast::Srgb MaterialBlend(text_contrast::Srgb background, text_contrast::Srgb tint,
                                 CardMaterialProfile profile) {
    // Match the numeric D2D COLOR then LUMINOSITY modes used by the desktop
    // graphs, not their historical parameter names. Blend in encoded RGB.
    const auto color = text_contrast::SourceOver(background,
        SetBlendLum(tint, BlendLum(background)), profile.luminosity_opacity);
    return text_contrast::SourceOver(color, SetBlendLum(color, BlendLum(tint)), profile.tint_opacity);
}
}

text_contrast::Samples DesktopRenderer::CardBackgroundSamples(const D2D1_RECT_F& rect,
    model::CardMaterial material, uint8_t opacity_percent, bool theme_dark) const {
    const auto theme = ui::MakeTheme(theme_dark, ui::HexColor(0x0078D4u));
    const text_contrast::Srgb solid{theme.bg.r, theme.bg.g, theme.bg.b};
    auto samples = backdrop_.SampleColors(rect);
    if (material == model::CardMaterial::LiquidGlass && live_wallpaper_.HasFrame() &&
        live_wallpaper_.AverageLuminance() >= 0.0f) {
        const float enc = std::pow(std::clamp(live_wallpaper_.AverageLuminance(), 0.0f, 1.0f), 1.0f / 2.2f);
        samples = text_contrast::Fill(text_contrast::Srgb{enc, enc, enc});
    }
    if (!samples.count) samples = text_contrast::Fill(solid);
    const float fade = std::clamp(static_cast<float>(opacity_percent) / 30.0f, 0.0f, 1.0f);
    if (fade <= 0.0f) return samples;
    const bool liquid_glass = (material == model::CardMaterial::LiquidGlass && !glass_pool_.Unavailable());
    const bool mica = material == model::CardMaterial::Mica || material == model::CardMaterial::MicaAlt;
    const bool live_acrylic = material == model::CardMaterial::Acrylic && native_acrylic_allowed_ && host_backdrop_.Active();
    const bool sampled_acrylic = material == model::CardMaterial::Acrylic &&
        ((!using_dcomp_ && !vsurf_ && hwnd_ == nullptr) ||
         (pulse::os::TrueWindowsBuild() < 22000 && native_acrylic_allowed_ && !host_backdrop_.Active()));
    text_contrast::TransformSamples(samples, [&](auto wallpaper) {
        auto surface = solid;
        if (liquid_glass) {
            surface = TransformLiquidGlassSample(wallpaper, theme_dark,
                                                 std::max<uint8_t>(opacity_percent, 30u),
                                                 current_glass_settings_.glass_frost);
        } else if (mica) {
            // Match WindowMaterial's Mica/MicaAlt tint used by SampledMicaBrush.
            const auto tint = text_contrast::FromArgb(material == model::CardMaterial::MicaAlt ?
                (theme_dark ? 0xFF0A0A0Au : 0xFFDADADAu) : (theme_dark ? 0xFF202020u : 0xFFF3F3F3u));
            surface = MaterialBlend(wallpaper, tint,
                                    CardMicaProfile(material == model::CardMaterial::MicaAlt, theme_dark));
        } else if (live_acrylic || sampled_acrylic) {
            const auto tint = CardMaterialTint(theme_dark);
            const auto profile = CardSurfaceProfile(theme_dark, opacity_percent);
            if (live_acrylic) {
                surface = MaterialBlend(wallpaper, {tint.r, tint.g, tint.b}, profile);
            } else {
                const float lum = 0.299f * wallpaper.r + 0.587f * wallpaper.g + 0.114f * wallpaper.b;
                const text_contrast::Srgb saturated{
                    std::clamp(lum + (wallpaper.r - lum) * 1.28f, 0.0f, 1.0f),
                    std::clamp(lum + (wallpaper.g - lum) * 1.28f, 0.0f, 1.0f),
                    std::clamp(lum + (wallpaper.b - lum) * 1.28f, 0.0f, 1.0f)};
                surface = text_contrast::SourceOver(saturated, {tint.r, tint.g, tint.b},
                    std::clamp(profile.tint_opacity + 0.14f, 0.45f, 0.90f));
            }
        }
        return text_contrast::SourceOver(wallpaper, surface, fade);
    });
    return samples;
}

bool DesktopRenderer::CardInkLight(const D2D1_RECT_F& rect, model::CardMaterial material, uint8_t opacity_percent,
                                   bool theme_dark) const {
    return text_contrast::ChooseInk(CardBackgroundSamples(rect, material, opacity_percent, theme_dark),
        0xFAFFFFFFu, 0xFA1B2028u, theme_dark).light;
}

text_contrast::Samples DesktopRenderer::TileBackgroundSamples(const model::DesktopTile& tile,
    const D2D1_RECT_F& rect, const D2D1_RECT_F& text_rect, model::CardMaterial material,
    const model::BeautifySettings& settings) const {
    auto samples = CardBackgroundSamples(text_rect, material, settings.card_opacity_percent, settings.dark_mode);
    if (model::ActiveTileView(tile).kind == model::TileKind::Photos) return samples;
    if (tile.look.background == model::TileBackground::Color) {
        const float opacity = material == model::CardMaterial::Solid ? 1.0f :
            std::min(1.0f, 0.38f + 0.56f * settings.card_opacity_percent / 100.0f);
        text_contrast::TransformSamples(samples, [&](auto color) {
            return text_contrast::SourceOver(color, tile.look.color_argb | 0xFF000000u, opacity);
        });
    } else if (tile.look.background == model::TileBackground::Image && !tile.look.image_path.empty()) {
        const auto photo = backdrop_.SamplePhotoColors(tile.look.image_path, rect, text_rect);
        if (photo.colors.count) {
            const auto base = samples;
            samples = photo.colors;
            for (size_t i = 0; i < samples.count; ++i) {
                const auto behind = base.colors[base.count ? i * base.count / samples.count : 0];
                samples.colors[i] = text_contrast::SourceOver(
                    text_contrast::SourceOver(behind, samples.colors[i], photo.alpha[i]), 0x30000000u);
            }
        }
        if (tile.size.cols >= 2 && text_rect.top >= rect.top + 44.0f) {
            text_contrast::TransformSamples(samples, [](auto color) { return text_contrast::SourceOver(color, 0x40000000u); });
        }
    }
    return samples;
}

void DesktopRenderer::DrawHeaderBand(const D2D1_RECT_F& card, float band_bottom, float radius, bool dark,
                                     uint8_t opacity_percent, uint32_t band_override, uint32_t line_override,
                                     bool with_grain, bool liquid_glass_minimal) {
    const float fade = std::clamp(static_cast<float>(opacity_percent) / 30.0f, 0.0f, 1.0f);
    if (fade <= 0.0f && !band_override) return;
    const float k = band_override ? 1.0f : fade;
    // LiquidGlass leaves top corners & meniscus highlight uncovered, keeping only an airy hairline separator.
    if (liquid_glass_minimal && !band_override) {
        const uint32_t hair = line_override ? line_override : (dark ? 0x22FFFFFFu : 0x16000000u);
        d2d_dc_->FillRectangle(D2D1::RectF(card.left + 12.0f, band_bottom - 1.0f, card.right - 12.0f, band_bottom),
                               Brush(hair, k));
        return;
    }
    const uint32_t band = band_override ? band_override : (dark ? 0x17FFFFFFu : 0x80FFFFFFu);
    const uint32_t line = line_override ? line_override : (dark ? 0x1AFFFFFFu : 0x12000000u);
    d2d_dc_->PushAxisAlignedClip(D2D1::RectF(card.left, card.top, card.right, band_bottom), D2D1_ANTIALIAS_MODE_ALIASED);
    d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(card, radius, radius), Brush(band, k));
    if (with_grain) {
        if (auto* nb = backdrop_.EnsureNoiseBrush(d2d_dc_.Get())) {
            const float band_alpha = std::clamp(static_cast<float>((band >> 24) & 0xFFu) / 255.0f * k, 0.0f, 1.0f);
            nb->SetOpacity(band_alpha);
            d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(card, radius, radius), nb);
            nb->SetOpacity(1.0f);
        }
    }
    d2d_dc_->PopAxisAlignedClip();
    d2d_dc_->FillRectangle(D2D1::RectF(card.left + 1.0f, band_bottom - 1.0f, card.right - 1.0f, band_bottom), Brush(line, k));
}

// WMO weather code -> a drawn icon (no font glyphs): sun, sun behind cloud, cloud, rain, snow, fog, storm.
void DesktopRenderer::DrawWeatherIcon(const D2D1_RECT_F& r, int code, bool dark) {
    const float s = std::min(r.right - r.left, r.bottom - r.top) / 24.0f;
    const float ox = r.left + ((r.right - r.left) - 24.0f * s) * 0.5f, oy = r.top + ((r.bottom - r.top) - 24.0f * s) * 0.5f;
    auto P = [&](float x, float y) { return D2D1::Point2F(ox + x * s, oy + y * s); };
    const uint32_t sun = dark ? 0xFFFFC53Du : 0xFFF5A524u;
    const uint32_t cloud = dark ? 0xFFE8EEF6u : 0xFFAEBBCBu;
    const uint32_t cloud_dark = dark ? 0xFFC9D3DFu : 0xFF8E9DB0u;
    const uint32_t drop = 0xFF5AA9F0u;
    auto draw_sun = [&](float cx, float cy, float rr) {
        d2d_dc_->FillEllipse(D2D1::Ellipse(P(cx, cy), rr * s, rr * s), Brush(sun));
        for (int i = 0; i < 8; ++i) {
            const float a = static_cast<float>(i) * kPi / 4.0f;
            d2d_dc_->DrawLine(P(cx + std::cos(a) * (rr + 2.4f), cy + std::sin(a) * (rr + 2.4f)),
                              P(cx + std::cos(a) * (rr + 4.6f), cy + std::sin(a) * (rr + 4.6f)), Brush(sun), 1.7f * s, round_stroke_.Get());
        }
    };
    auto draw_cloud = [&](float dx, float dy, uint32_t c) {
        d2d_dc_->FillEllipse(D2D1::Ellipse(P(10.0f + dx, 13.5f + dy), 4.6f * s, 4.6f * s), Brush(c));
        d2d_dc_->FillEllipse(D2D1::Ellipse(P(14.6f + dx, 12.0f + dy), 5.4f * s, 5.4f * s), Brush(c));
        d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(ox + (5.0f + dx) * s, oy + (13.0f + dy) * s,
                                                                    ox + (20.5f + dx) * s, oy + (19.0f + dy) * s), 3.0f * s, 3.0f * s), Brush(c));
    };
    if (code == 0 || code == 1) { draw_sun(12.0f, 12.0f, 4.6f); return; }
    if (code == 2) { draw_sun(8.5f, 8.5f, 3.6f); draw_cloud(1.0f, 1.5f, cloud); return; }
    if (code == 45 || code == 48) {
        for (int i = 0; i < 3; ++i) d2d_dc_->DrawLine(P(4.0f, 9.0f + i * 4.0f), P(20.0f, 9.0f + i * 4.0f), Brush(cloud_dark), 1.8f * s, round_stroke_.Get());
        return;
    }
    const bool snow = (code >= 71 && code <= 77) || code == 85 || code == 86;
    const bool rain = (code >= 51 && code <= 67) || (code >= 80 && code <= 82);
    const bool storm = code >= 95;
    draw_cloud(0.0f, -2.0f, (rain || snow || storm) ? cloud_dark : cloud);
    if (rain) for (int i = 0; i < 3; ++i) d2d_dc_->DrawLine(P(8.5f + i * 3.5f, 19.0f), P(7.5f + i * 3.5f, 22.0f), Brush(drop), 1.6f * s, round_stroke_.Get());
    if (snow) for (int i = 0; i < 3; ++i) d2d_dc_->FillEllipse(D2D1::Ellipse(P(8.5f + i * 3.5f, 20.5f), 1.1f * s, 1.1f * s), Brush(cloud));
    if (storm) {
        ComPtr<ID2D1PathGeometry> g; ComPtr<ID2D1GeometrySink> k;
        if (SUCCEEDED(d2d_factory_->CreatePathGeometry(&g)) && SUCCEEDED(g->Open(&k))) {
            k->BeginFigure(P(12.5f, 16.0f), D2D1_FIGURE_BEGIN_FILLED);
            const D2D1_POINT_2F pts[] = {P(10.0f, 20.0f), P(12.0f, 20.0f), P(10.8f, 23.0f), P(14.5f, 18.5f), P(12.5f, 18.5f)};
            k->AddLines(pts, 5); k->EndFigure(D2D1_FIGURE_END_CLOSED); k->Close();
            d2d_dc_->FillGeometry(g.Get(), Brush(sun));
        }
    }
}

namespace {
// What the weather widgets show before the first forecast arrives (and in snapshots).
LiveTileRuntimeData::WeatherView PreviewWeather(const std::wstring& city) {
    LiveTileRuntimeData::WeatherView w{};
    w.valid = true;
    w.city = city.empty() ? L"北京" : city;
    w.temp_c = 12;
    w.code = 2;
    w.lo_c = 7;
    w.hi_c = 15;
    w.hours = {{L"15时", 2, 15}, {L"16时", 0, 15}, {L"17时", 0, 14}, {L"18时", 3, 12}, {L"19时", 3, 11}};
    w.days = {{L"明天", 61, 6, 11}, {L"周日", 2, 5, 13}, {L"周一", 0, 7, 16}};
    return w;
}
const wchar_t* WeatherLabel(int code) {
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
} // namespace

void DesktopRenderer::DrawLiveTile(const model::DesktopTile& tile_in,
                                   const D2D1_RECT_F& raw_rect,
                                   const model::DesktopModelState& state,
                                   const LiveTileRuntimeData& live_data,
                                   const DesktopInteractionState& interaction) {
    // A stack draws its active widget; the place, size, look and effect are the tile's.
    model::DesktopTile tile = model::ActiveTileView(tile_in);
    const model::TileLook& look = tile_in.look;
    if (!look.title_override.empty()) tile.title = look.title_override;
    const bool show_title = look.show_title;
    const bool theme_dark = state.settings.dark_mode;
    const bool hovered = (interaction.hovered_tile_id == tile_in.id);
    const bool drop_target = (interaction.drop_target_tile_id == tile_in.id) || (interaction.stack_drop_target_tile_id == tile_in.id);
    const D2D1_RECT_F rect = raw_rect;
    // `dark` below is the ink: light text where the wallpaper behind the card is dark, whatever the theme.
    bool dark = CardInkLight(rect, ResolveMaterial(tile.material, state.settings, interaction),
                                   state.settings.card_opacity_percent, theme_dark);

    uint32_t hue = TileHueArgb(tile.kind, dark);
    if (tile.kind == model::TileKind::StickyNote) {
        // The note's title and icon take its paper colour (便笺颜色), not always yellow.
        static constexpr uint32_t kNoteHueDark[6] = {0xFFF5C84Bu, 0xFF6FD58Fu, 0xFF7DB9F5u, 0xFFF59AC6u, 0xFFB9A3F5u, 0xFFC2C8D2u};
        static constexpr uint32_t kNoteHueLight[6] = {0xFFB07D00u, 0xFF23843Fu, 0xFF2368B8u, 0xFFB83A78u, 0xFF6A4FC4u, 0xFF5B6475u};
        const int nc = std::clamp(tile_in.note_color, 0, 5);
        hue = dark ? kNoteHueDark[nc] : kNoteHueLight[nc];
    }
    const uint32_t accent = ResolveAccentArgb(tile.accent_argb, state.settings, interaction);
    const model::CardMaterial mat = ResolveMaterial(tile.material, state.settings, interaction);
    const bool with_grain = (mat == model::CardMaterial::Acrylic);

    const bool active_drag = (interaction.dragging_tile && interaction.dragging_tile_id == tile_in.id);
    const bool jiggle = interaction.tile_edit_mode && !active_drag && (interaction.resizing_tile_id != tile_in.id);
    const float press_amt = (interaction.pressed_tile_id == tile_in.id) ? interaction.tile_press : 0.0f;
    const std::string tile_card_id = "tile:" + tile_in.id;
    DrawCardMaterial(rect, 16.0f, mat, state.settings.card_opacity_percent, theme_dark, accent,
                     drop_target, hovered, 1.0f,
                     state.settings.glass_dispersion, press_amt,
                     /*allow_lower_glass=*/(!active_drag && !jiggle),
                     /*is_group_card=*/false, tile_card_id, &state.settings);

    uint32_t text_pri = dark ? 0xFAFFFFFFu : 0xFA1B2028u;
    uint32_t text_sec = dark ? 0x99FFFFFFu : 0x99242C38u;
    // 自定义磁贴: a colour over the effect, or a picture; the text follows the background's lightness.
    uint32_t band_override = 0, line_override = 0;
    bool frosted_content = false;
    const bool is_photo_kind = tile.kind == model::TileKind::Photos;
    if (look.background == model::TileBackground::Color && !is_photo_kind) {
        const float op = mat == model::CardMaterial::Solid ? 1.0f
                         : std::min(1.0f, 0.38f + 0.56f * static_cast<float>(state.settings.card_opacity_percent) / 100.0f);
        d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(rect, 16.0f, 16.0f), Brush(look.color_argb | 0xFF000000u, op));
        if (with_grain) {
            if (auto* nb = backdrop_.EnsureNoiseBrush(d2d_dc_.Get())) {
                nb->SetOpacity(op);
                d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(rect, 16.0f, 16.0f), nb);
                nb->SetOpacity(1.0f);
            }
        }
        const auto background = TileBackgroundSamples(tile_in, rect, rect, mat, state.settings);
        dark = text_contrast::ChooseInk(background, 0xFAFFFFFFu, 0xFA1B2028u, dark).light;
        const bool light_bg = !dark;
        text_pri = dark ? 0xFAFFFFFFu : 0xFA1B2028u;
        text_sec = dark ? 0xCCFFFFFFu : 0xCC1B2028u;
        hue = text_pri;
        band_override = light_bg ? 0x60FFFFFFu : 0x29000000u;
        line_override = light_bg ? 0x14000000u : 0x24FFFFFFu;
    } else if (look.background == model::TileBackground::Image && !is_photo_kind && !look.image_path.empty()) {
        ComPtr<ID2D1RoundedRectangleGeometry> clip;
        d2d_factory_->CreateRoundedRectangleGeometry(D2D1::RoundedRect(rect, 16.0f, 16.0f), &clip);
        if (auto* bmp = backdrop_.GetPhotoTileBitmap(d2d_dc_.Get(), look.image_path)) {
            if (clip) d2d_dc_->PushLayer(D2D1::LayerParameters1(rect, clip.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                                D2D1::IdentityMatrix(), 1.0f, nullptr, D2D1_LAYER_OPTIONS1_NONE), nullptr);
            // Cover: fill the tile, keep the aspect.
            const D2D1_SIZE_F bs = bmp->GetSize();
            const float tw = rect.right - rect.left, th = rect.bottom - rect.top;
            const float sc = (bs.width > 0 && bs.height > 0) ? std::max(tw / bs.width, th / bs.height) : 1.0f;
            const float dw = bs.width * sc, dh = bs.height * sc;
            d2d_dc_->DrawBitmap(bmp, D2D1::RectF(rect.left + (tw - dw) * 0.5f, rect.top + (th - dh) * 0.5f,
                                                 rect.left + (tw + dw) * 0.5f, rect.top + (th + dh) * 0.5f), 1.0f);
            d2d_dc_->FillRectangle(rect, Brush(0x30000000u));
            if (clip) d2d_dc_->PopLayer();
        }
        const auto content_rect = tile.size.cols >= 2 ?
            D2D1::RectF(rect.left + 8.0f, rect.top + 44.0f, rect.right - 8.0f, rect.bottom - 8.0f) : rect;
        const auto background = TileBackgroundSamples(tile_in, rect, content_rect, mat, state.settings);
        dark = text_contrast::ChooseInk(background, 0xFAFFFFFFu, 0xFA1B2028u, dark).light;
        text_pri = dark ? 0xFAFFFFFFu : 0xFA1B2028u;
        text_sec = dark ? 0xCCFFFFFFu : 0xCC1B2028u;
        hue = text_pri;
        band_override = 0x52000000u; line_override = 0x1FFFFFFFu;
        frosted_content = tile.size.cols >= 2;
    }
    if (frosted_content) {
        d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(rect.left + 8.0f, rect.top + 44.0f, rect.right - 8.0f, rect.bottom - 8.0f),
                                                        10.0f, 10.0f), Brush(0x40000000u));
    }
    const bool present_veil = (state.settings.active_scene == model::DesktopScene::Present) &&
                              IsPersonalTileKind(tile.kind);

    if (present_veil) {
        ComPtr<ID2D1RoundedRectangleGeometry> veil_geo;
        d2d_factory_->CreateRoundedRectangleGeometry(D2D1::RoundedRect(rect, 16.0f, 16.0f), &veil_geo);
        if (veil_geo) {
            d2d_dc_->PushLayer(
                D2D1::LayerParameters1(rect, veil_geo.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                       D2D1::IdentityMatrix(), 1.0f, nullptr, D2D1_LAYER_OPTIONS1_NONE),
                nullptr);
        }
        if (backdrop_.MicaBrush()) {
            d2d_dc_->FillRectangle(rect, backdrop_.MicaBrush());
        }
        d2d_dc_->FillRectangle(rect, Brush(dark ? 0xC81C1E26u : 0xD8F4F6FAu));

        const float cy = (rect.top + rect.bottom) * 0.5f;
        if (tile.size.cols == 1) {
            const wchar_t eye_g[2] = {L'\xED1A', 0};
            DrawGlyph(d2d_dc_.Get(), eye_g, 1, fmt_fluent_14_.Get(),
                              D2D1::RectF(rect.left, cy - 18.0f, rect.right, cy + 2.0f), Brush(text_sec));
            const wchar_t* short_msg = L"已隐藏";
            DrawWidgetText(short_msg, static_cast<UINT32>(std::wcslen(short_msg)), fmt_pill_.Get(),
                              D2D1::RectF(rect.left + 4.0f, cy + 2.0f, rect.right - 4.0f, cy + 20.0f),
                              Brush(text_sec));
        } else {
            const std::wstring msg = L"演示中已隐藏";
            const float mw = MeasureSingleLineWidth(msg, fmt_caption_.Get(), 200.0f);
            const float total_w = mw + 22.0f;
            const float sx = (rect.left + rect.right - total_w) * 0.5f;
            const wchar_t eye_g[2] = {L'\xED1A', 0};
            DrawGlyph(d2d_dc_.Get(), eye_g, 1, fmt_fluent_14_.Get(),
                              D2D1::RectF(sx, cy - 12.0f, sx + 18.0f, cy + 12.0f), Brush(text_sec));
            DrawWidgetText(msg.c_str(), static_cast<UINT32>(msg.size()), fmt_caption_.Get(),
                              D2D1::RectF(sx + 22.0f, cy - 12.0f, sx + total_w + 8.0f, cy + 12.0f), Brush(text_sec));
        }
        if (veil_geo) d2d_dc_->PopLayer();
        return;
    }

    const bool show_hover_edit = hovered && !interaction.tile_edit_mode;

    // Helper lambda to draw wide tile top bar (left HUE Fluent icon + HUE semibold title, right muted subtitle)
    auto draw_wide_header = [&](const std::wstring& title, const std::wstring& right_sub) {
        if (!show_title) return;
        DrawHeaderBand(rect, rect.top + 40.0f, 16.0f, dark, state.settings.card_opacity_percent,
                       band_override, line_override, with_grain, mat == model::CardMaterial::LiquidGlass);
        const wchar_t gl[2] = {TileFluentGlyph(tile), 0};
        DrawGlyph(d2d_dc_.Get(), gl, 1, fmt_fluent_12_.Get(),
                          D2D1::RectF(rect.left + 13.0f, rect.top + 11.0f, rect.left + 28.0f, rect.top + 29.0f),
                          Brush(hue));
        const float sub_right = show_hover_edit ? (rect.right - 42.0f) : (rect.right - 14.0f);
        const float title_right = right_sub.empty() ? (show_hover_edit ? rect.right - 40.0f : rect.right - 10.0f)
                                                    : (sub_right - 76.0f);
        if (mat == model::CardMaterial::LiquidGlass && state.settings.glass_frost < 35 && !band_override) {
            DrawWidgetText(title.c_str(), static_cast<UINT32>(title.size()), fmt_tile_hdr_.Get(),
                              D2D1::RectF(rect.left + 31.0f, rect.top + 12.0f, title_right, rect.top + 30.0f),
                              Brush(dark ? 0x5A000000u : 0x55FFFFFFu));
        }
        DrawWidgetText(title.c_str(), static_cast<UINT32>(title.size()), fmt_tile_hdr_.Get(),
                          D2D1::RectF(rect.left + 31.0f, rect.top + 11.0f, title_right, rect.top + 29.0f),
                          Brush(hue));
        if (!right_sub.empty() && sub_right > rect.left + 94.0f) {
            DrawWidgetText(right_sub.c_str(), static_cast<UINT32>(right_sub.size()), fmt_tile_sub_.Get(),
                              D2D1::RectF(rect.left + 90.0f, rect.top + 11.0f, sub_right, rect.top + 29.0f),
                              Brush(text_sec));
        }
    };

    // 此电脑 and 回收站 wear Explorer's own icons (the Recycle Bin's follows empty / full).
    const wchar_t* shell_icon_item = tile.kind == model::TileKind::ThisPC ? L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}"
                                   : tile.kind == model::TileKind::RecycleBin ? L"::{645FF040-5081-101B-9F08-00AA002F954E}"
                                   : nullptr;
    const wchar_t* shell_icon_salt = tile.kind == model::TileKind::RecycleBin && live_data.recycle_bin.item_count == 0 ? L":empty" : L"";

    // Helper lambda to draw 1x1 46x46 rounded app badge + optional top-right count pill + bottom centered title
    auto draw_small_app_tile = [&](const std::wstring& title, uint64_t badge_count, bool show_count) {
        const float cx = (rect.left + rect.right) * 0.5f;
        const float badge_top = show_title ? rect.top + 16.0f : (rect.top + rect.bottom - 46.0f) * 0.5f;
        const D2D1_RECT_F brc = D2D1::RectF(cx - 23.0f, badge_top, cx + 23.0f, badge_top + 46.0f);
        ID2D1Bitmap1* sys_icon = shell_icon_item
            ? backdrop_.GetItemBitmap(d2d_dc_.Get(), shell_icon_item, true, static_cast<uint32_t>(std::lround(48.0f * dip_scale_)), shell_icon_salt)
            : nullptr;
        if (sys_icon) {
            const D2D1_SIZE_F bs = sys_icon->GetSize();
            const float k = (bs.width > 0 && bs.height > 0) ? std::min(48.0f / bs.width, 48.0f / bs.height) : 1.0f;
            const float dw = bs.width * k, dh = bs.height * k;
            d2d_dc_->DrawBitmap(sys_icon, D2D1::RectF(cx - dw * 0.5f, badge_top - 1.0f + (48.0f - dh) * 0.5f,
                                                      cx + dw * 0.5f, badge_top - 1.0f + (48.0f + dh) * 0.5f),
                                1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }

        D2D1_GRADIENT_STOP gstops[3] = {
            {0.00f, ColorFromArgb(MixArgb(hue, 0xFFFFFFFFu, 0.28f))},
            {0.55f, ColorFromArgb(hue)},
            {1.00f, ColorFromArgb(MixArgb(hue, 0xFF000000u, 0.24f))},
        };
        ComPtr<ID2D1GradientStopCollection> gcol;
        if (sys_icon) {
            // Explorer's icon replaces the badge.
        } else if (SUCCEEDED(d2d_dc_->CreateGradientStopCollection(gstops, 3, &gcol)) && gcol) {
            ComPtr<ID2D1LinearGradientBrush> gbr;
            if (SUCCEEDED(d2d_dc_->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(
                        D2D1::Point2F(cx, brc.top), D2D1::Point2F(cx, brc.bottom)),
                    gcol.Get(), &gbr)) && gbr) {
                d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(brc, 12.0f, 12.0f), gbr.Get());
            }
        } else {
            d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(brc, 12.0f, 12.0f), Brush(hue));
        }
        if (!sys_icon) {
            d2d_dc_->DrawLine(D2D1::Point2F(brc.left + 10.0f, brc.top + 1.0f),
                              D2D1::Point2F(brc.right - 10.0f, brc.top + 1.0f),
                              Brush(0x58FFFFFFu), 1.0f);
            const wchar_t gl[2] = {TileFluentGlyph(tile), 0};
            DrawGlyph(d2d_dc_.Get(), gl, 1, fmt_fluent_21_.Get(), brc, Brush(0xFFFFFFFFu));
        }

        if (show_count) {
            wchar_t cbuf[24]{};
            std::swprintf(cbuf, ARRAYSIZE(cbuf), L"%llu", static_cast<unsigned long long>(badge_count));
            const float cw = std::max(20.0f, MeasureSingleLineWidth(cbuf, fmt_pill_.Get(), 60.0f) + 10.0f);
            // Hovered, the [✏] button takes the top-right corner: the count moves to the top-left.
            const bool edit_corner = hovered && !interaction.tile_edit_mode;
            const D2D1_RECT_F pill = edit_corner ? D2D1::RectF(rect.left + 8.0f, rect.top + 8.0f, rect.left + 8.0f + cw, rect.top + 26.0f)
                                                 : D2D1::RectF(rect.right - 8.0f - cw, rect.top + 8.0f, rect.right - 8.0f, rect.top + 26.0f);
            d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(pill, 9.0f, 9.0f),
                                          Brush(dark ? 0xD8202430u : 0xEAFFFFFFu));
            d2d_dc_->DrawRoundedRectangle(D2D1::RoundedRect(pill, 9.0f, 9.0f),
                                          Brush(dark ? 0x2EFFFFFFu : 0x1E000000u), 1.0f);
            DrawWidgetText(cbuf, static_cast<UINT32>(std::wcslen(cbuf)), fmt_pill_.Get(), pill, Brush(text_pri));
        }

        if (show_title)
            DrawWidgetText(title.c_str(), static_cast<UINT32>(title.size()), fmt_tile_small_title_.Get(),
                              D2D1::RectF(rect.left + 6.0f, rect.bottom - 28.0f, rect.right - 6.0f, rect.bottom - 6.0f),
                              Brush(text_pri));
    };
    // The dots on the right edge of a stack, hover/active resize cues, + the top-right hover [✏] (.tedit) button; drawn last over the widget.
    struct TilePostDecor {
        DesktopRenderer* self;
        const model::DesktopTile& t;
        D2D1_RECT_F r;
        uint32_t c;
        uint32_t accent;
        bool dark;
        bool hovered;
        bool show_edit;
        bool edit_hot;
        bool edit_mode;
        TileResizeZone resize_zone;
        bool resizing;
        ~TilePostDecor() {
            if (t.stack.size() >= 2) {
                const size_t n = t.stack.size();
                const D2D1_RECT_F first = DesktopRenderer::StackDotRect(r, n, t.stack_index, 0);
                const D2D1_RECT_F last = DesktopRenderer::StackDotRect(r, n, t.stack_index, n - 1);
                if (hovered) {
                    // Hovered: a pill behind the dots (each one clickable) and arrows to step through the stack.
                    self->d2d_dc_->FillRoundedRectangle(
                        D2D1::RoundedRect(D2D1::RectF(first.left - 2.0f, first.top - 4.0f, first.right + 2.0f, last.bottom + 4.0f), 4.5f, 4.5f),
                        self->Brush(dark ? 0x72181C24u : 0x80FFFFFFu));
                    for (const bool up : {true, false}) {
                        const D2D1_RECT_F a = DesktopRenderer::StackArrowRect(r, n, up);
                        if (a.right <= a.left) continue;
                        const float cx = (a.left + a.right) * 0.5f, cy = (a.top + a.bottom) * 0.5f;
                        self->d2d_dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 8.0f, 8.0f),
                                                   self->Brush(dark ? 0x9A181C24u : 0xC8FFFFFFu));
                        const float dy = up ? -1.5f : 1.5f;
                        self->d2d_dc_->DrawLine(D2D1::Point2F(cx - 3.5f, cy - dy), D2D1::Point2F(cx, cy + dy), self->Brush(c, 0.9f), 1.5f, self->round_stroke_.Get());
                        self->d2d_dc_->DrawLine(D2D1::Point2F(cx, cy + dy), D2D1::Point2F(cx + 3.5f, cy - dy), self->Brush(c, 0.9f), 1.5f, self->round_stroke_.Get());
                    }
                }
                for (size_t i = 0; i < n; ++i) {
                    const bool on = static_cast<int>(i) == t.stack_index;
                    self->d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(DesktopRenderer::StackDotRect(r, n, t.stack_index, i), 2.5f, 2.5f),
                                                        self->Brush(c, on ? 0.88f : 0.30f));
                }
            }
            if (!edit_mode && (resizing || (hovered && resize_zone != TileResizeZone::None))) {
                if (resizing || resize_zone == TileResizeZone::BottomRight) {
                    DrawCircularArc(self->d2d_factory_.Get(), self->d2d_dc_.Get(),
                                    self->Brush(accent, 0.88f),
                                    r.right - 14.0f, r.bottom - 14.0f, 8.5f, 15.0f, 65.0f, 1.8f, self->round_stroke_.Get());
                } else if (resize_zone == TileResizeZone::Bottom) {
                    const float cx = (r.left + r.right) * 0.5f;
                    self->d2d_dc_->FillRoundedRectangle(
                        D2D1::RoundedRect(D2D1::RectF(cx - 16.0f, r.bottom - 6.0f, cx + 16.0f, r.bottom - 3.0f), 1.5f, 1.5f),
                        self->Brush(accent, 0.88f));
                } else if (resize_zone == TileResizeZone::Right) {
                    const float cy = (r.top + r.bottom) * 0.5f;
                    self->d2d_dc_->FillRoundedRectangle(
                        D2D1::RoundedRect(D2D1::RectF(r.right - 6.0f, cy - 16.0f, r.right - 3.0f, cy + 16.0f), 1.5f, 1.5f),
                        self->Brush(accent, 0.88f));
                }
                if (resizing) {
                    wchar_t dim_buf[24]{};
                    std::swprintf(dim_buf, ARRAYSIZE(dim_buf), L"%d×%d",
                                  static_cast<int>(std::lround(r.right - r.left)),
                                  static_cast<int>(std::lround(r.bottom - r.top)));
                    const D2D1_RECT_F pill = DesktopRenderer::TileSizePillRect(r, true);
                    self->d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(pill, 11.0f, 11.0f),
                                                        self->Brush(dark ? 0xF5262A33u : 0xF8FFFFFFu));
                    self->DrawWidgetText(dim_buf, static_cast<UINT32>(std::wcslen(dim_buf)),
                                            self->fmt_pill_.Get(), pill, self->Brush(dark ? 0xFFFFFFFFu : 0xFF1B1B1Bu));
                }
            }
            if (show_edit) {
                const D2D1_RECT_F eb = DesktopRenderer::TileEditButtonRect(r, t.size.cols == 1);
                const float rad = t.size.cols == 1 ? 6.0f : 7.0f;
                self->d2d_dc_->FillRoundedRectangle(
                    D2D1::RoundedRect(eb, rad, rad),
                    self->Brush(dark ? (edit_hot ? 0xEA2B3240u : 0xCC1C202Au)
                                     : (edit_hot ? 0xFAFFFFFFu : 0xE2F4F6FAu)));
                self->d2d_dc_->DrawRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(eb.left + 0.5f, eb.top + 0.5f, eb.right - 0.5f, eb.bottom - 0.5f),
                                      rad - 0.5f, rad - 0.5f),
                    self->Brush(dark ? 0x32FFFFFFu : 0x1E000000u), 1.0f);
                const wchar_t pen_g[2] = {L'\xE70F', 0};
                DrawGlyph(self->d2d_dc_.Get(), pen_g, 1, self->fmt_fluent_12_.Get(), eb,
                          self->Brush(dark ? 0xF5FFFFFFu : 0xF51B2028u));
            }
        }
    } post_decor{this, tile_in, rect, text_pri, accent, dark, hovered, show_hover_edit, interaction.hovered_tile_edit_btn,
                 interaction.tile_edit_mode,
                 hovered ? interaction.hovered_tile_resize : TileResizeZone::None,
                 interaction.resizing_tile_id == tile_in.id};

    // -----------------------------------------------------------------------
    // 1x1 Small Tile Renderers (when tile.size.cols == 1)
    // -----------------------------------------------------------------------
    if (tile.size.cols == 1 && tile.kind != model::TileKind::Photos &&
        tile.kind != model::TileKind::DiskRing && tile.kind != model::TileKind::PulseLauncher &&
        tile.kind != model::TileKind::Clock && tile.kind != model::TileKind::StickyNote &&
        tile.kind != model::TileKind::Weather && tile.kind != model::TileKind::Calendar &&
        tile.kind != model::TileKind::Todo && tile.kind != model::TileKind::System) {
        std::wstring lbl = tile.title;
        uint64_t cnt = 0;
        bool has_cnt = false;
        if (tile.kind == model::TileKind::RecycleBin) {
            if (lbl.empty()) lbl = L"回收站";
            cnt = live_data.recycle_bin.item_count;
            has_cnt = (cnt > 0);
        } else if (tile.kind == model::TileKind::ThisPC) {
            if (lbl.empty()) lbl = L"此电脑";
        } else if (tile.kind == model::TileKind::StagingTray) {
            if (lbl.empty()) lbl = L"暂存区";
            cnt = live_data.tray_entries.size();
            has_cnt = (cnt > 0);
        } else if (tile.kind == model::TileKind::Downloads) {
            if (lbl.empty()) lbl = L"下载";
        } else if (tile.kind == model::TileKind::Recent) {
            if (lbl.empty()) lbl = L"最近";
        } else if (tile.kind == model::TileKind::Apps) {
            if (lbl.empty()) lbl = L"应用";
            for (const auto& it : state.items) cnt += it.in_apps_tile ? 1u : 0u;
            has_cnt = cnt > 0;
        } else if (lbl.empty()) {
            lbl = L"文件夹";
        }
        // 回收站 hovered with something in it: 清空 in place of its name.
        const bool bin_empty_button = tile.kind == model::TileKind::RecycleBin && hovered && cnt > 0 && !interaction.tile_edit_mode;
        draw_small_app_tile(bin_empty_button ? std::wstring{} : lbl, cnt, has_cnt);
        if (bin_empty_button) {
            const D2D1_RECT_F b = RecycleEmptyRect(rect);
            d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(b, 10.0f, 10.0f), Brush(0xFFE5484Du, 0.92f));
            DrawWidgetText(L"清空", 2, fmt_pill_.Get(), b, Brush(0xFFFFFFFFu));
        }
        return;
    }

    // One-row sizes (2×1, 4×1): a glance strip — the widget's mark, its main value, two lines of detail, and on
    // 4×1 more of the same (hours, next to-dos, every meter) to the right.
    if (tile.size.rows == 1 && tile.size.cols >= 2 &&
        (tile.kind == model::TileKind::Weather || tile.kind == model::TileKind::Calendar ||
         tile.kind == model::TileKind::Todo || tile.kind == model::TileKind::System)) {
        const bool wide = tile.size.cols >= 4;
        const float cy = (rect.top + rect.bottom) * 0.5f;
        // One line of text vertically centred in r, trimmed with an ellipsis; returns its width.
        auto line = [&](const std::wstring& s, IDWriteTextFormat* fmt, D2D1_RECT_F r, uint32_t color,
                        DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) -> float {
            if (s.empty() || r.right <= r.left || !fmt) return 0.0f;
            widget_text_.Bind(dwrite_factory_.Get(),d2d_dc_.Get());
            auto* layout=widget_text_.Layout(dwrite_factory_.Get(),s,fmt,r.right-r.left,r.bottom-r.top,true,align);
            if(!layout)return 0.0f;
            widget_text_.DrawLayout(dwrite_factory_.Get(),d2d_dc_.Get(),{r.left,r.top},layout,Brush(color));
            DWRITE_TEXT_METRICS m{};
            layout->GetMetrics(&m);
            return std::min(m.widthIncludingTrailingWhitespace, r.right - r.left);
        };
        // Title over detail, to the right of x.
        auto two_lines = [&](float x, float right, const std::wstring& top, const std::wstring& bottom) {
            line(top, fmt_tile_hdr_.Get(), D2D1::RectF(x, cy - 20.0f, right, cy), text_pri);
            line(bottom, fmt_caption_.Get(), D2D1::RectF(x, cy, right, cy + 19.0f), text_sec);
        };
        const float left = rect.left + 14.0f;
        const float main_right = wide ? std::min(rect.right - 14.0f, rect.left + 200.0f) : rect.right - 12.0f;
        auto divider = [&](float x) {
            d2d_dc_->FillRectangle(D2D1::RectF(x, rect.top + 14.0f, x + 1.0f, rect.bottom - 14.0f), Brush(dark ? 0x1CFFFFFFu : 0x12000000u));
        };
        std::vector<const model::TodoItem*> open_todos;
        size_t done_todos = 0;
        for (const auto& td : state.todos) {
            if (td.done) ++done_todos;
            else if (!td.text.empty()) open_todos.push_back(&td);
        }
        switch (tile.kind) {
        case model::TileKind::Weather: {
            LiveTileRuntimeData::WeatherView fallback = PreviewWeather(state.settings.weather_city);
            const auto& w = live_data.weather.valid || !live_data.weather.status.empty() ? live_data.weather : fallback;
            const std::wstring city = w.city.empty() ? (state.settings.weather_city.empty() ? L"北京" : state.settings.weather_city) : w.city;
            if (!w.valid) {
                two_lines(left, rect.right - 12.0f, city, w.status.empty() ? std::wstring(L"正在获取天气…") : w.status);
                break;
            }
            DrawWeatherIcon(D2D1::RectF(left - 2.0f, cy - 17.0f, left + 32.0f, cy + 17.0f), w.code, dark);
            wchar_t tb[16]{};
            std::swprintf(tb, ARRAYSIZE(tb), L"%d°", w.temp_c);
            float x = left + 38.0f;
            x += line(tb, fmt_clock_big_.Get(), D2D1::RectF(x, rect.top, x + 80.0f, rect.bottom), text_pri) + 10.0f;
            // 2×1 has room for the condition only; 4×1 adds the day's range and gives the text more width.
            wchar_t range[48]{};
            if (wide) std::swprintf(range, ARRAYSIZE(range), L"%ls · %d°/%d°", WeatherLabel(w.code), w.lo_c, w.hi_c);
            else std::swprintf(range, ARRAYSIZE(range), L"%ls", WeatherLabel(w.code));
            const float wx_right = wide ? std::min(rect.right - 14.0f, x + 108.0f) : main_right;
            two_lines(x, wx_right, city, range);
            if (wide) {
                divider(wx_right + 6.0f);
                const float hx0 = wx_right + 14.0f, hx1 = rect.right - 10.0f;
                // As many hours as fit at 38 DIP each (up to five).
                const size_t n = std::min<size_t>({w.hours.size(), size_t{5}, static_cast<size_t>(std::max(0.0f, (hx1 - hx0) / 38.0f))});
                const float step = n ? (hx1 - hx0) / static_cast<float>(n) : 0.0f;
                for (size_t i = 0; i < n; ++i) {
                    const auto& h = w.hours[i];
                    const float hcx = hx0 + step * (static_cast<float>(i) + 0.5f);
                    line(h.label, fmt_pill_.Get(), D2D1::RectF(hcx - 24.0f, rect.top + 8.0f, hcx + 24.0f, rect.top + 24.0f), text_sec, DWRITE_TEXT_ALIGNMENT_CENTER);
                    DrawWeatherIcon(D2D1::RectF(hcx - 10.0f, cy - 10.0f, hcx + 10.0f, cy + 10.0f), h.code, dark);
                    wchar_t hb[12]{};
                    std::swprintf(hb, ARRAYSIZE(hb), L"%d°", h.temp_c);
                    line(hb, fmt_disk_pct_.Get(), D2D1::RectF(hcx - 24.0f, rect.bottom - 26.0f, hcx + 24.0f, rect.bottom - 8.0f), text_pri, DWRITE_TEXT_ALIGNMENT_CENTER);
                }
            }
            break;
        }
        case model::TileKind::Calendar: {
            SYSTEMTIME st{};
            GetLocalTime(&st);
            static constexpr const wchar_t* kWd[7] = {L"周日", L"周一", L"周二", L"周三", L"周四", L"周五", L"周六"};
            // A small calendar page: weekday band over the date.
            const D2D1_RECT_F page = D2D1::RectF(left, cy - 22.0f, left + 44.0f, cy + 22.0f);
            d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(page, 8.0f, 8.0f), Brush(dark ? 0x1FFFFFFFu : 0xD9FFFFFFu));
            d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(page.left, page.top, page.right, page.top + 14.0f), 8.0f, 8.0f), Brush(0xFFE5484Du));
            d2d_dc_->FillRectangle(D2D1::RectF(page.left, page.top + 8.0f, page.right, page.top + 14.0f), Brush(0xFFE5484Du));
            line(kWd[st.wDayOfWeek % 7], fmt_pill_.Get(), D2D1::RectF(page.left, page.top, page.right, page.top + 14.0f), 0xFFFFFFFFu, DWRITE_TEXT_ALIGNMENT_CENTER);
            line(std::to_wstring(st.wDay), fmt_tile_hdr_.Get(), D2D1::RectF(page.left, page.top + 14.0f, page.right, page.bottom), dark ? 0xFFFFFFFFu : 0xFF1F2430u, DWRITE_TEXT_ALIGNMENT_CENTER);
            wchar_t date[32]{};
            std::swprintf(date, ARRAYSIZE(date), L"%u月%u日 %ls", st.wMonth, st.wDay, kWd[st.wDayOfWeek % 7]);
            const std::wstring next = open_todos.empty() ? std::wstring(L"今天没有待办") : L"待办 · " + open_todos.front()->text;
            two_lines(page.right + 12.0f, main_right, date, next);
            if (wide) {
                divider(main_right + 6.0f);
                float ty = cy - 20.0f;
                for (size_t i = 1; i < open_todos.size() && i <= 2; ++i, ty += 20.0f)
                    line(L"·  " + open_todos[i]->text, fmt_caption_.Get(), D2D1::RectF(main_right + 16.0f, ty, rect.right - 12.0f, ty + 19.0f), text_sec);
                if (open_todos.size() <= 1)
                    line(L"其余时间都空着", fmt_caption_.Get(), D2D1::RectF(main_right + 16.0f, cy - 10.0f, rect.right - 12.0f, cy + 10.0f), text_sec);
            }
            break;
        }
        case model::TileKind::Todo: {
            const size_t total = state.todos.size();
            const float rcx = left + 18.0f, rad = 16.0f;
            d2d_dc_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(rcx, cy), rad, rad), Brush(dark ? 0x24FFFFFFu : 0x16000000u), 3.6f);
            if (total) DrawCircularArc(d2d_factory_.Get(), d2d_dc_.Get(), Brush(hue), rcx, cy, rad, -90.0f,
                                       360.0f * static_cast<float>(done_todos) / static_cast<float>(total), 3.6f, round_stroke_.Get());
            line(std::to_wstring(open_todos.size()), fmt_disk_pct_.Get(), D2D1::RectF(rcx - rad, cy - 10.0f, rcx + rad, cy + 10.0f), text_pri, DWRITE_TEXT_ALIGNMENT_CENTER);
            wchar_t head[40]{};
            std::swprintf(head, ARRAYSIZE(head), L"%ls · %zu/%zu", tile.title.empty() ? L"待办" : tile.title.c_str(), done_todos, total);
            two_lines(rcx + rad + 12.0f, main_right, head, open_todos.empty() ? std::wstring(L"都做完了") : open_todos.front()->text);
            if (wide) {
                divider(main_right + 6.0f);
                float ty = cy - 20.0f;
                for (size_t i = 1; i < open_todos.size() && i <= 2; ++i, ty += 20.0f) {
                    d2d_dc_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(main_right + 22.0f, ty + 9.5f), 5.0f, 5.0f), Brush(text_sec), 1.2f);
                    line(open_todos[i]->text, fmt_caption_.Get(), D2D1::RectF(main_right + 34.0f, ty, rect.right - 12.0f, ty + 19.0f), text_pri);
                }
                if (open_todos.size() <= 1)
                    line(L"单击添加待办…", fmt_caption_.Get(), D2D1::RectF(main_right + 16.0f, cy - 10.0f, rect.right - 12.0f, cy + 10.0f), text_sec);
            }
            break;
        }
        case model::TileKind::System: {
            const auto& sy = live_data.system;
            const int mem_pct = sy.mem_total ? static_cast<int>(sy.mem_used * 100 / sy.mem_total) : 0;
            const int net_pct = static_cast<int>(std::min<uint64_t>(100, sy.net_down_bps / (1024 * 1024 / 4)));
            struct Meter { const wchar_t* label; int pct; uint32_t color; std::wstring value; };
            std::vector<Meter> meters = {{L"CPU", sy.cpu_pct, hue, std::to_wstring(sy.cpu_pct) + L"%"},
                                         {L"内存", mem_pct, 0xFF30A46Cu, std::to_wstring(mem_pct) + L"%"}};
            if (wide) {
                meters.push_back({L"C:", sy.disk_used_pct, 0xFFF5A524u, L"剩 " + FormatBytesCompact(sy.disk_free)});
                meters.push_back({L"网络", net_pct, 0xFF8E6BE8u, L"↓ " + FormatBytesCompact(sy.net_down_bps) + L"/s"});
            }
            const float slot = (rect.right - rect.left - 12.0f) / static_cast<float>(meters.size());
            for (size_t i = 0; i < meters.size(); ++i) {
                const auto& m = meters[i];
                const float x0 = rect.left + 8.0f + slot * static_cast<float>(i);
                const float rcx = x0 + 22.0f, rad = 14.0f;
                d2d_dc_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(rcx, cy), rad, rad), Brush(dark ? 0x24FFFFFFu : 0x16000000u), 3.2f);
                DrawCircularArc(d2d_factory_.Get(), d2d_dc_.Get(), Brush(m.color), rcx, cy, rad, -90.0f,
                                3.6f * static_cast<float>(std::clamp(m.pct, 0, 100)), 3.2f, round_stroke_.Get());
                two_lines(rcx + rad + 8.0f, x0 + slot - 4.0f, m.label, m.value);
            }
            break;
        }
        default:
            break;
        }
        return;
    }

    switch (tile.kind) {
    case model::TileKind::Photos: {
        ComPtr<ID2D1RoundedRectangleGeometry> clip_geo;
        d2d_factory_->CreateRoundedRectangleGeometry(D2D1::RoundedRect(rect, 16.0f, 16.0f), &clip_geo);
        if (clip_geo) {
            d2d_dc_->PushLayer(
                D2D1::LayerParameters1(rect, clip_geo.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                       D2D1::IdentityMatrix(), 1.0f, nullptr, D2D1_LAYER_OPTIONS1_NONE),
                nullptr);
        }

        bool drew_bitmap = false;
        if (!live_data.photo_paths.empty()) {
            const size_t cur = live_data.active_photo_index % live_data.photo_paths.size();
            auto draw_cover = [&](ID2D1Bitmap1* bmp, float op) {
                const D2D1_SIZE_F bs = bmp->GetSize();
                const float tw = rect.right - rect.left, th = rect.bottom - rect.top;
                if (bs.width > 0.0f && bs.height > 0.0f && tw > 0.0f && th > 0.0f) {
                    const float sc = std::max(tw / bs.width, th / bs.height);
                    const float dw = bs.width * sc, dh = bs.height * sc;
                    d2d_dc_->DrawBitmap(bmp, D2D1::RectF(rect.left + (tw - dw) * 0.5f, rect.top + (th - dh) * 0.5f,
                                                         rect.left + (tw + dw) * 0.5f, rect.top + (th + dh) * 0.5f),
                                        op, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                } else {
                    d2d_dc_->DrawBitmap(bmp, rect, op);
                }
            };
            if (auto* cur_bmp = backdrop_.GetPhotoTileBitmap(d2d_dc_.Get(), live_data.photo_paths[cur])) {
                if (live_data.photo_crossfade < 0.99f && live_data.prev_photo_index < live_data.photo_paths.size()) {
                    if (auto* prev_bmp = backdrop_.GetPhotoTileBitmap(
                            d2d_dc_.Get(), live_data.photo_paths[live_data.prev_photo_index])) {
                        draw_cover(prev_bmp, 1.0f);
                    }
                }
                draw_cover(cur_bmp, std::clamp(live_data.photo_crossfade, 0.0f, 1.0f));
                drew_bitmap = true;
            }
        }
        if (!drew_bitmap) {
            const int sc_idx = static_cast<int>(live_data.active_photo_index % 3u);
            WallpaperBackdrop::DrawPhotoTileScene(d2d_factory_.Get(), d2d_dc_.Get(), rect, sc_idx, dark);
        }

        // Bottom 45%..100% linear dark scrim + crisp white labels
        const D2D1_RECT_F scrim_rc = D2D1::RectF(rect.left, rect.top + (rect.bottom - rect.top) * 0.45f,
                                                 rect.right, rect.bottom);
        D2D1_GRADIENT_STOP sstops[2] = {
            {0.0f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.00f)},
            {1.0f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.62f)},
        };
        ComPtr<ID2D1GradientStopCollection> scol;
        if (SUCCEEDED(d2d_dc_->CreateGradientStopCollection(sstops, 2, &scol)) && scol) {
            ComPtr<ID2D1LinearGradientBrush> sbr;
            if (SUCCEEDED(d2d_dc_->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(
                        D2D1::Point2F(rect.left, scrim_rc.top), D2D1::Point2F(rect.left, scrim_rc.bottom)),
                    scol.Get(), &sbr)) && sbr) {
                d2d_dc_->FillRectangle(scrim_rc, sbr.Get());
            }
        }

        const std::wstring cap = tile.title.empty() ? L"照片" : tile.title;
        DrawWidgetText(cap.c_str(), static_cast<UINT32>(cap.size()), fmt_group_title_.Get(),
                          D2D1::RectF(rect.left + 15.0f, rect.bottom - 32.0f, rect.right - 15.0f, rect.bottom - 8.0f),
                          Brush(0xFAFFFFFFu));

        if (tile.size.cols >= 2) {
            const size_t n_photos = !live_data.photo_paths.empty()
                                        ? live_data.photo_paths.size()
                                        : live_data.photo_total_count;
            wchar_t sub_buf[64]{};
            if (state.settings.live_tiles_enabled && tile.live_content) {
                // The tile's own slideshow setting (轮播间隔).
                const int every = tile_in.photo_interval_s;
                if (every <= 0) std::swprintf(sub_buf, ARRAYSIZE(sub_buf), L"%zu 项 · 手动切换", n_photos);
                else if (every < 60) std::swprintf(sub_buf, ARRAYSIZE(sub_buf), L"%zu 项 · 每 %d 秒换一张", n_photos, every);
                else std::swprintf(sub_buf, ARRAYSIZE(sub_buf), L"%zu 项 · 每 %d 分钟换一张", n_photos, every / 60);
            } else {
                std::swprintf(sub_buf, ARRAYSIZE(sub_buf), L"%zu 项", n_photos);
            }
            DrawWidgetText(sub_buf, static_cast<UINT32>(std::wcslen(sub_buf)), fmt_tile_sub_.Get(),
                              D2D1::RectF(rect.left + 80.0f, rect.bottom - 32.0f, rect.right - 15.0f, rect.bottom - 8.0f),
                              Brush(0xCCFFFFFFu));
        }

        // Hovered with more than one picture: frosted round buttons to step back and forth.
        if (hovered && live_data.photo_paths.size() > 1 && !interaction.tile_edit_mode) {
            for (const bool next : {false, true}) {
                const D2D1_RECT_F b = PhotoNavRect(tile_in, rect, next);
                if (b.right <= b.left) continue;
                const float cx = (b.left + b.right) * 0.5f, cy = (b.top + b.bottom) * 0.5f;
                d2d_dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 14.0f, 14.0f), Brush(0x66000000u));
                d2d_dc_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 13.5f, 13.5f), Brush(0x40FFFFFFu), 1.0f);
                const float dx = next ? 2.0f : -2.0f;
                d2d_dc_->DrawLine(D2D1::Point2F(cx - dx, cy - 5.0f), D2D1::Point2F(cx + dx, cy), Brush(0xF2FFFFFFu), 1.8f, round_stroke_.Get());
                d2d_dc_->DrawLine(D2D1::Point2F(cx + dx, cy), D2D1::Point2F(cx - dx, cy + 5.0f), Brush(0xF2FFFFFFu), 1.8f, round_stroke_.Get());
            }
        }
        if (clip_geo) d2d_dc_->PopLayer();
        // Crisp inner hairline border over photo scene
        d2d_dc_->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(rect.left + 0.5f, rect.top + 0.5f, rect.right - 0.5f, rect.bottom - 0.5f),
                              16.0f, 16.0f),
            Brush(hovered ? 0x55FFFFFFu : 0x28FFFFFFu), 1.0f);
        break;
    }

    case model::TileKind::Downloads: {
        const std::wstring title = tile.title.empty() ? L"下载" : tile.title;
        wchar_t cnt_sub[32]{};
        std::swprintf(cnt_sub, ARRAYSIZE(cnt_sub), L"%zu 项", live_data.download_items.size());
        draw_wide_header(title, cnt_sub);

        if (live_data.download_items.empty()) {
            const wchar_t* empty_txt = L"最近没有新下载";
            DrawWidgetText(empty_txt, static_cast<UINT32>(std::wcslen(empty_txt)), fmt_empty_hint_.Get(),
                              D2D1::RectF(rect.left + 14.0f, rect.top + (show_title ? 46.0f : 12.0f), rect.right - 14.0f, rect.bottom - 10.0f),
                              Brush(text_sec, 0.75f));
            break;
        }

        // A hovered row offers 在文件夹中显示 where its time was.
        auto row_action = [&](const D2D1_RECT_F& row) {
            const D2D1_RECT_F b = ListRowActionRect(row);
            d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(b, 7.0f, 7.0f), Brush(dark ? 0x22FFFFFFu : 0x12000000u));
            DrawVectorFileIcon(d2d_factory_.Get(), d2d_dc_.Get(), dwrite_factory_.Get(),
                               D2D1::RectF(b.left + 5.0f, b.top + 4.0f, b.right - 5.0f, b.bottom - 4.0f), L"", true);
        };
        {
            const auto& lead = live_data.download_items[0];
            const D2D1_RECT_F lrc = ListRowRect(model::TileKind::Downloads, rect, 0, show_title);
            const bool row_hot = hovered && interaction.hovered_tile_row == 0;
            d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(lrc, 10.0f, 10.0f),
                                          Brush(dark ? (row_hot ? 0x24FFFFFFu : 0x16FFFFFFu)
                                                     : (row_hot ? 0x16000000u : 0x0E000000u)));
            DrawVectorFileIcon(d2d_factory_.Get(), d2d_dc_.Get(), dwrite_factory_.Get(),
                               D2D1::RectF(lrc.left + 8.0f, lrc.top + 7.0f, lrc.left + 26.0f, lrc.top + 25.0f),
                               lead.extension.empty() ? std::wstring_view(lead.name) : std::wstring_view(lead.extension),
                               lead.is_folder);
            const std::wstring meta = FormatRelativeTimeOrSize(lead.modified_unix_ms, lead.size_bytes);
            DrawWidgetText(lead.name.c_str(), static_cast<UINT32>(lead.name.size()), fmt_row_lead_.Get(),
                              D2D1::RectF(lrc.left + 32.0f, lrc.top, lrc.right - 64.0f, lrc.bottom), Brush(text_pri));
            if (row_hot) row_action(lrc);
            else DrawWidgetText(meta.c_str(), static_cast<UINT32>(meta.size()), fmt_row_meta_.Get(),
                                   D2D1::RectF(lrc.right - 64.0f, lrc.top, lrc.right - 8.0f, lrc.bottom), Brush(text_sec));
        }

        for (size_t i = 1; i < live_data.download_items.size(); ++i) {
            const D2D1_RECT_F hrc = ListRowRect(model::TileKind::Downloads, rect, i, show_title);
            if (hrc.bottom <= hrc.top) break;
            const float ry = hrc.top;
            const auto& row = live_data.download_items[i];
            const bool row_hot = hovered && interaction.hovered_tile_row == static_cast<int>(i);
            if (row_hot) {
                d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(hrc, 8.0f, 8.0f),
                                              Brush(dark ? 0x16FFFFFFu : 0x0E000000u));
            }
            DrawVectorFileIcon(d2d_factory_.Get(), d2d_dc_.Get(), dwrite_factory_.Get(),
                               D2D1::RectF(rect.left + 16.0f, ry + 5.0f, rect.left + 32.0f, ry + 21.0f),
                               row.extension.empty() ? std::wstring_view(row.name) : std::wstring_view(row.extension),
                               row.is_folder);
            const std::wstring meta = FormatRelativeTimeOrSize(row.modified_unix_ms, row.size_bytes);
            DrawWidgetText(row.name.c_str(), static_cast<UINT32>(row.name.size()), fmt_row_name_.Get(),
                              D2D1::RectF(rect.left + 38.0f, ry, rect.right - 68.0f, ry + 26.0f), Brush(text_pri));
            if (row_hot) row_action(hrc);
            else DrawWidgetText(meta.c_str(), static_cast<UINT32>(meta.size()), fmt_row_meta_.Get(),
                                   D2D1::RectF(rect.right - 68.0f, ry, rect.right - 14.0f, ry + 26.0f), Brush(text_sec));
        }
        break;
    }

    case model::TileKind::Recent: {
        const std::wstring title = tile.title.empty() ? L"最近使用" : tile.title;
        draw_wide_header(title, L"Pulse 和应用");

        if (live_data.recent_items.empty()) {
            const wchar_t* empty_txt = L"暂无最近打开的文件";
            DrawWidgetText(empty_txt, static_cast<UINT32>(std::wcslen(empty_txt)), fmt_empty_hint_.Get(),
                              D2D1::RectF(rect.left + 14.0f, rect.top + (show_title ? 46.0f : 12.0f), rect.right - 14.0f, rect.bottom - 10.0f),
                              Brush(text_sec, 0.75f));
            break;
        }

        for (size_t i = 0; i < live_data.recent_items.size(); ++i) {
            const D2D1_RECT_F hrc = ListRowRect(model::TileKind::Recent, rect, i, show_title);
            if (hrc.bottom <= hrc.top) break;
            const float ry = hrc.top;
            const auto& row = live_data.recent_items[i];
            const bool row_hot = hovered && interaction.hovered_tile_row == static_cast<int>(i);
            if (row_hot) {
                d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(hrc, 8.0f, 8.0f),
                                              Brush(dark ? 0x16FFFFFFu : 0x0E000000u));
            }
            DrawVectorFileIcon(d2d_factory_.Get(), d2d_dc_.Get(), dwrite_factory_.Get(),
                               D2D1::RectF(rect.left + 15.0f, ry + 5.0f, rect.left + 31.0f, ry + 21.0f),
                               row.extension.empty() ? std::wstring_view(row.name) : std::wstring_view(row.extension),
                               row.is_folder);
            const std::wstring meta = FormatRelativeTimeOrSize(row.modified_unix_ms, row.size_bytes);
            DrawWidgetText(row.name.c_str(), static_cast<UINT32>(row.name.size()), fmt_row_name_.Get(),
                              D2D1::RectF(rect.left + 37.0f, ry, rect.right - 68.0f, ry + 26.0f), Brush(text_pri));
            if (row_hot) {
                const D2D1_RECT_F b = ListRowActionRect(hrc);
                d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(b, 7.0f, 7.0f), Brush(dark ? 0x22FFFFFFu : 0x12000000u));
                DrawVectorFileIcon(d2d_factory_.Get(), d2d_dc_.Get(), dwrite_factory_.Get(),
                                   D2D1::RectF(b.left + 5.0f, b.top + 4.0f, b.right - 5.0f, b.bottom - 4.0f), L"", true);
            } else {
                DrawWidgetText(meta.c_str(), static_cast<UINT32>(meta.size()), fmt_row_meta_.Get(),
                                  D2D1::RectF(rect.right - 68.0f, ry, rect.right - 14.0f, ry + 26.0f), Brush(text_sec));
            }
        }
        break;
    }

    case model::TileKind::PinnedFolder: {
        const std::wstring title = tile.title.empty() ? L"源代码" : tile.title;
        const bool is_code_repo = (TileFluentGlyph(tile) == L'\xE943');
        std::wstring right_sub = tile.target_path;
        if (is_code_repo && (right_sub.empty() || ContainsCaseInsensitive(right_sub, L"源"))) {
            right_sub = L"main · 3 处改动";
        }
        draw_wide_header(title, right_sub);

        auto it = live_data.pinned_folder_items.find(tile.target_path);
        if (it == live_data.pinned_folder_items.end() || it->second.empty()) {
            const wchar_t* empty_txt = L"单击在 Pulse 中打开此文件夹";
            DrawWidgetText(empty_txt, static_cast<UINT32>(std::wcslen(empty_txt)), fmt_empty_hint_.Get(),
                              D2D1::RectF(rect.left + 14.0f, rect.top + (show_title ? 46.0f : 12.0f), rect.right - 14.0f, rect.bottom - 10.0f),
                              Brush(text_sec, 0.75f));
            break;
        }

        float ry = rect.top + (show_title ? 46.0f : 12.0f);
        const auto& rows = it->second;
        for (size_t i = 0; i < rows.size() && ry + 26.0f <= rect.bottom - 8.0f; ++i) {
            const auto& row = rows[i];
            if (hovered && interaction.hovered_tile_row == static_cast<int>(i)) {
                const D2D1_RECT_F hrc = D2D1::RectF(rect.left + 9.0f, ry, rect.right - 9.0f, ry + 26.0f);
                d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(hrc, 8.0f, 8.0f),
                                              Brush(dark ? 0x16FFFFFFu : 0x0E000000u));
            }
            DrawVectorFileIcon(d2d_factory_.Get(), d2d_dc_.Get(), dwrite_factory_.Get(),
                               D2D1::RectF(rect.left + 15.0f, ry + 5.0f, rect.left + 31.0f, ry + 21.0f),
                               row.extension.empty() ? std::wstring_view(row.name) : std::wstring_view(row.extension),
                               row.is_folder);
            DrawWidgetText(row.name.c_str(), static_cast<UINT32>(row.name.size()), fmt_row_name_.Get(),
                              D2D1::RectF(rect.left + 37.0f, ry, rect.right - 88.0f, ry + 26.0f), Brush(text_pri));

            if (is_code_repo && i < 3 && !row.is_folder) {
                const bool is_new = (i == 2);
                const uint32_t dot_col = is_new ? 0xFF30A46Cu : 0xFFF5A524u;
                const wchar_t* st_txt = is_new ? L"新增" : L"已修改";
                d2d_dc_->FillEllipse(
                    D2D1::Ellipse(D2D1::Point2F(rect.right - (is_new ? 46.0f : 58.0f), ry + 13.0f), 3.0f, 3.0f),
                    Brush(dot_col));
                DrawWidgetText(st_txt, static_cast<UINT32>(std::wcslen(st_txt)), fmt_row_meta_.Get(),
                                  D2D1::RectF(rect.right - 70.0f, ry, rect.right - 14.0f, ry + 26.0f), Brush(text_sec));
            } else {
                const std::wstring meta = FormatRelativeTimeOrSize(row.modified_unix_ms, row.size_bytes);
                DrawWidgetText(meta.c_str(), static_cast<UINT32>(meta.size()), fmt_row_meta_.Get(),
                                  D2D1::RectF(rect.right - 80.0f, ry, rect.right - 14.0f, ry + 26.0f), Brush(text_sec));
            }
            ry += 29.0f;
        }
        break;
    }

    case model::TileKind::RecycleBin: {
        const std::wstring title = tile.title.empty() ? L"回收站" : tile.title;
        wchar_t stat_buf[64]{};
        if (live_data.recycle_bin.item_count == 0) {
            std::swprintf(stat_buf, ARRAYSIZE(stat_buf), L"已清空");
        } else {
            const std::wstring sz = FormatBytesCompact(live_data.recycle_bin.total_bytes);
            std::swprintf(stat_buf, ARRAYSIZE(stat_buf), L"%llu 项 · %ls",
                          static_cast<unsigned long long>(live_data.recycle_bin.item_count), sz.c_str());
        }
        draw_wide_header(title, stat_buf);
        if (auto* bmp = backdrop_.GetItemBitmap(d2d_dc_.Get(), shell_icon_item, true, static_cast<uint32_t>(std::lround(48.0f * dip_scale_)), shell_icon_salt))
            d2d_dc_->DrawBitmap(bmp, D2D1::RectF(rect.left + 16.0f, rect.top + (show_title ? 48.0f : 14.0f), rect.left + 64.0f, rect.top + (show_title ? 96.0f : 62.0f)), 1.0f,
                                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        const wchar_t* rb_hint = (live_data.recycle_bin.item_count == 0)
            ? L"回收站为空\n拖入文件可移至回收站"
            : L"单击打开回收站\n右键可一键清空";
        DrawWidgetText(rb_hint, static_cast<UINT32>(std::wcslen(rb_hint)), fmt_caption_.Get(),
                          D2D1::RectF(rect.left + 74.0f, rect.top + (show_title ? 52.0f : 18.0f), rect.right - 14.0f, rect.bottom - 8.0f),
                          Brush(text_sec));
        break;
    }

    case model::TileKind::ThisPC: {
        const std::wstring title = tile.title.empty() ? L"此电脑" : tile.title;
        draw_wide_header(title, L"设备与驱动器");
        if (auto* bmp = backdrop_.GetItemBitmap(d2d_dc_.Get(), shell_icon_item, true, static_cast<uint32_t>(std::lround(48.0f * dip_scale_))))
            d2d_dc_->DrawBitmap(bmp, D2D1::RectF(rect.left + 16.0f, rect.top + (show_title ? 48.0f : 14.0f), rect.left + 64.0f, rect.top + (show_title ? 96.0f : 62.0f)), 1.0f,
                                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        std::wstring pc_info = L"单击在 Pulse 中浏览驱动器";
        if (!live_data.drives.empty()) {
            const auto& d0 = live_data.drives.front();
            pc_info = L"共 " + std::to_wstring(live_data.drives.size()) + L" 个驱动器\n" +
                      (d0.root_path.size() >= 2 ? d0.root_path.substr(0, 2) : L"C:") +
                      L" 可用 " + FormatBytesCompact(d0.free_bytes);
        }
        DrawWidgetText(pc_info.c_str(), static_cast<UINT32>(pc_info.size()), fmt_caption_.Get(),
                          D2D1::RectF(rect.left + 74.0f, rect.top + (show_title ? 52.0f : 18.0f), rect.right - 14.0f, rect.bottom - 8.0f),
                          Brush(text_sec));
        break;
    }

    case model::TileKind::DiskRing: {
        mount::DriveSpaceStats ds{L"C:\\", 0, 0};
        if (!live_data.drives.empty()) {
            ds = live_data.drives.front();
        } else {
            ds = mount::QueryDriveSpaceStats(tile.target_path.empty() ? L"C:\\" : tile.target_path);
        }
        const uint64_t used = (ds.total_bytes > ds.free_bytes) ? (ds.total_bytes - ds.free_bytes) : 0;
        const float ratio = ds.total_bytes > 0
                                ? std::clamp(static_cast<float>(static_cast<double>(used) / static_cast<double>(ds.total_bytes)), 0.0f, 1.0f)
                                : 0.76f;

        const wchar_t drive_letter = (!ds.root_path.empty() && std::iswalpha(ds.root_path[0]))
                                         ? static_cast<wchar_t>(std::towupper(ds.root_path[0]))
                                         : (!tile.title.empty() && std::iswalpha(tile.title[0])
                                                ? static_cast<wchar_t>(std::towupper(tile.title[0]))
                                                : L'C');
        const std::wstring free_compact = ds.total_bytes > 0 ? FormatBytesCompact(ds.free_bytes) : L"140 GB";

        if (tile.size.cols == 1) {
            const float cx = (rect.left + rect.right) * 0.5f;
            const float cy = rect.top + 38.0f;
            const float rad = 22.0f;
            d2d_dc_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad),
                                 Brush(dark ? 0x24FFFFFFu : 0x16000000u), 4.2f);
            DrawCircularArc(d2d_factory_.Get(), d2d_dc_.Get(),
                            Brush(ratio > 0.90f ? 0xFFE5484Du : hue),
                            cx, cy, rad, -90.0f, ratio * 360.0f, 4.2f, round_stroke_.Get());

            wchar_t pct_buf[16]{};
            std::swprintf(pct_buf, ARRAYSIZE(pct_buf), L"%d%%", static_cast<int>(std::round(ratio * 100.0f)));
            DrawWidgetText(pct_buf, static_cast<UINT32>(std::wcslen(pct_buf)), fmt_disk_pct_.Get(),
                              D2D1::RectF(cx - rad, cy - 11.0f, cx + rad, cy + 11.0f), Brush(text_pri));

            wchar_t foot_buf[48]{};
            std::swprintf(foot_buf, ARRAYSIZE(foot_buf), L"%lc: 剩 %ls", drive_letter, free_compact.c_str());
            DrawWidgetText(foot_buf, static_cast<UINT32>(std::wcslen(foot_buf)), fmt_tile_small_title_.Get(),
                              D2D1::RectF(rect.left + 4.0f, rect.bottom - 26.0f, rect.right - 4.0f, rect.bottom - 6.0f),
                              Brush(text_pri));
        } else {
            wchar_t hdr_t[32]{};
            std::swprintf(hdr_t, ARRAYSIZE(hdr_t), L"本地磁盘 (%lc:)", drive_letter);
            draw_wide_header(hdr_t, L"剩 " + free_compact);
            const float cx = rect.left + 42.0f;
            const float cy = rect.top + 70.0f;
            d2d_dc_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 22.0f, 22.0f),
                                 Brush(dark ? 0x24FFFFFFu : 0x16000000u), 4.2f);
            DrawCircularArc(d2d_factory_.Get(), d2d_dc_.Get(), Brush(ratio > 0.90f ? 0xFFE5484Du : hue),
                            cx, cy, 22.0f, -90.0f, ratio * 360.0f, 4.2f, round_stroke_.Get());
            wchar_t pct_buf[16]{};
            std::swprintf(pct_buf, ARRAYSIZE(pct_buf), L"%d%%", static_cast<int>(std::round(ratio * 100.0f)));
            DrawWidgetText(pct_buf, static_cast<UINT32>(std::wcslen(pct_buf)), fmt_disk_pct_.Get(),
                              D2D1::RectF(cx - 22.0f, cy - 10.0f, cx + 22.0f, cy + 10.0f), Brush(text_pri));
            if (ds.total_bytes > 0) {
                const std::wstring used_line = L"已用 " + FormatBytesCompact(used);
                const std::wstring total_line = L"共 " + FormatBytesCompact(ds.total_bytes) + L" · 可用 " + free_compact;
                DrawWidgetText(used_line.c_str(), static_cast<UINT32>(used_line.size()), fmt_row_lead_.Get(),
                                  D2D1::RectF(rect.left + 76.0f, rect.top + 50.0f, rect.right - 12.0f, rect.top + 70.0f),
                                  Brush(text_pri));
                DrawWidgetText(total_line.c_str(), static_cast<UINT32>(total_line.size()), fmt_caption_.Get(),
                                  D2D1::RectF(rect.left + 76.0f, rect.top + 70.0f, rect.right - 12.0f, rect.top + 90.0f),
                                  Brush(text_sec));
            }
            if (tile.size.rows >= 2 && live_data.drives.size() > 1) {
                float dy = rect.top + 108.0f;
                for (size_t di = 1; di < live_data.drives.size() && dy + 24.0f <= rect.bottom - 8.0f; ++di) {
                    const auto& drv = live_data.drives[di];
                    const uint64_t du = drv.total_bytes > drv.free_bytes ? (drv.total_bytes - drv.free_bytes) : 0;
                    const float dr = drv.total_bytes > 0
                        ? std::clamp(static_cast<float>(static_cast<double>(du) / static_cast<double>(drv.total_bytes)), 0.0f, 1.0f)
                        : 0.0f;
                    const std::wstring dlbl = drv.root_path.size() >= 2 ? drv.root_path.substr(0, 2) : L"D:";
                    DrawWidgetText(dlbl.c_str(), static_cast<UINT32>(dlbl.size()), fmt_row_meta_.Get(),
                                      D2D1::RectF(rect.left + 16.0f, dy, rect.left + 44.0f, dy + 20.0f), Brush(text_pri));
                    const float bx0 = rect.left + 46.0f, bx1 = rect.right - 76.0f;
                    d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx0, dy + 7.0f, bx1, dy + 13.0f), 3.0f, 3.0f),
                                                  Brush(dark ? 0x1AFFFFFFu : 0x12000000u));
                    d2d_dc_->FillRoundedRectangle(
                        D2D1::RoundedRect(D2D1::RectF(bx0, dy + 7.0f, bx0 + (bx1 - bx0) * dr, dy + 13.0f), 3.0f, 3.0f),
                        Brush(dr > 0.90f ? 0xFFE5484Du : hue));
                    const std::wstring dfree = L"剩 " + FormatBytesCompact(drv.free_bytes);
                    DrawWidgetText(dfree.c_str(), static_cast<UINT32>(dfree.size()), fmt_row_meta_.Get(),
                                      D2D1::RectF(rect.right - 72.0f, dy, rect.right - 12.0f, dy + 20.0f), Brush(text_sec));
                    dy += 26.0f;
                }
            }
        }
        break;
    }

    case model::TileKind::PulseLauncher: {
        if (tile.size.cols == 1) {
            const float cx = (rect.left + rect.right) * 0.5f;
            const float btop = rect.top + 17.0f;
            const D2D1_RECT_F brc = D2D1::RectF(cx - 24.0f, btop - 2.0f, cx + 24.0f, btop + 46.0f);
            DrawPulseLogo(d2d_dc_.Get(), brc);

            const wchar_t* lbl = L"Pulse";
            DrawWidgetText(lbl, static_cast<UINT32>(std::wcslen(lbl)), fmt_tile_small_title_.Get(),
                              D2D1::RectF(rect.left + 6.0f, rect.bottom - 26.0f, rect.right - 6.0f, rect.bottom - 6.0f),
                              Brush(text_pri));
        } else {
            const D2D1_RECT_F brc = D2D1::RectF(rect.left + 14.0f, rect.top + 14.0f, rect.left + 52.0f, rect.top + 52.0f);
            DrawPulseLogo(d2d_dc_.Get(), brc);
            const wchar_t* title = L"Pulse 文件管理器";
            DrawWidgetText(title, static_cast<UINT32>(std::wcslen(title)), fmt_group_title_.Get(),
                              D2D1::RectF(rect.left + 62.0f, rect.top + 14.0f, rect.right - 14.0f, rect.top + 36.0f),
                              Brush(text_pri));
            const wchar_t* sub = L"极速浏览 · 秒搜全盘";
            DrawWidgetText(sub, static_cast<UINT32>(std::wcslen(sub)), fmt_caption_.Get(),
                              D2D1::RectF(rect.left + 62.0f, rect.top + 34.0f, rect.right - 14.0f, rect.top + 54.0f),
                              Brush(text_sec));
        }
        break;
    }

    case model::TileKind::Clock: {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        static constexpr const wchar_t* kWeekdays[7] = {L"周日", L"周一", L"周二", L"周三", L"周四", L"周五", L"周六"};

        auto draw_analog_dial = [&](float cx, float cy, float rad) {
            d2d_dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad),
                                 Brush(dark ? 0x12FFFFFFu : 0x0C000000u));
            d2d_dc_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad),
                                 Brush(dark ? 0x30FFFFFFu : 0x24000000u), 1.2f);
            for (int i = 0; i < 12; ++i) {
                const float ang = static_cast<float>(i * 30 - 90) * (kPi / 180.0f);
                const bool major = (i % 3 == 0);
                const float r_in = rad - (major ? (rad * 0.20f) : (rad * 0.12f));
                const float r_out = rad - 1.8f;
                d2d_dc_->DrawLine(
                    D2D1::Point2F(cx + std::cos(ang) * r_in, cy + std::sin(ang) * r_in),
                    D2D1::Point2F(cx + std::cos(ang) * r_out, cy + std::sin(ang) * r_out),
                    Brush(major ? text_pri : text_sec, major ? 0.78f : 0.45f),
                    major ? 1.5f : 1.0f, round_stroke_.Get());
            }
            const float hr_ang = (((st.wHour % 12) + st.wMinute / 60.0f) * 30.0f - 90.0f) * (kPi / 180.0f);
            const float mn_ang = ((st.wMinute + st.wSecond / 60.0f) * 6.0f - 90.0f) * (kPi / 180.0f);
            const float sc_ang = (st.wSecond * 6.0f - 90.0f) * (kPi / 180.0f);
            d2d_dc_->DrawLine(D2D1::Point2F(cx, cy),
                              D2D1::Point2F(cx + std::cos(hr_ang) * rad * 0.50f, cy + std::sin(hr_ang) * rad * 0.50f),
                              Brush(text_pri), rad >= 40.0f ? 3.2f : 2.8f, round_stroke_.Get());
            d2d_dc_->DrawLine(D2D1::Point2F(cx, cy),
                              D2D1::Point2F(cx + std::cos(mn_ang) * rad * 0.72f, cy + std::sin(mn_ang) * rad * 0.72f),
                              Brush(text_pri, 0.90f), rad >= 40.0f ? 2.2f : 1.9f, round_stroke_.Get());
            d2d_dc_->DrawLine(D2D1::Point2F(cx - std::cos(sc_ang) * 4.5f, cy - std::sin(sc_ang) * 4.5f),
                              D2D1::Point2F(cx + std::cos(sc_ang) * rad * 0.78f, cy + std::sin(sc_ang) * rad * 0.78f),
                              Brush(0xFFFF9F5Au), 1.1f, round_stroke_.Get());
            d2d_dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 2.4f, 2.4f), Brush(0xFFFF9F5Au));
        };

        if (tile.analog_clock) {
            if (tile.size.cols == 1) {
                draw_analog_dial((rect.left + rect.right) * 0.5f, rect.top + 40.0f, 29.5f);
                const wchar_t* wk = kWeekdays[st.wDayOfWeek % 7];
                DrawWidgetText(wk, static_cast<UINT32>(std::wcslen(wk)), fmt_pill_.Get(),
                                  D2D1::RectF(rect.left + 4.0f, rect.bottom - 24.0f, rect.right - 4.0f, rect.bottom - 5.0f),
                                  Brush(text_sec));
            } else if (tile.size.rows == 1) {
                draw_analog_dial(rect.left + 48.0f, (rect.top + rect.bottom) * 0.5f, 33.0f);
                wchar_t time_buf[16]{};
                std::swprintf(time_buf, ARRAYSIZE(time_buf), L"%02u:%02u", st.wHour, st.wMinute);
                DrawWidgetText(time_buf, static_cast<UINT32>(std::wcslen(time_buf)), fmt_clock_big_.Get(),
                                  D2D1::RectF(rect.left + 94.0f, rect.top + 12.0f, rect.right - 12.0f, rect.top + 60.0f),
                                  Brush(text_pri));
                wchar_t date_buf[48]{};
                std::swprintf(date_buf, ARRAYSIZE(date_buf), L"%u月%u日 %ls",
                              st.wMonth, st.wDay, kWeekdays[st.wDayOfWeek % 7]);
                DrawWidgetText(date_buf, static_cast<UINT32>(std::wcslen(date_buf)), fmt_caption_.Get(),
                                  D2D1::RectF(rect.left + 96.0f, rect.bottom - 30.0f, rect.right - 12.0f, rect.bottom - 8.0f),
                                  Brush(text_sec));
            } else {
                const float rw = rect.right - rect.left;
                const float rh = rect.bottom - rect.top;
                const float footer_h = 34.0f;
                const float rad = std::clamp(std::min(rw - 28.0f, rh - footer_h - 18.0f) * 0.5f, 24.0f, 120.0f);
                const float dial_cy = rect.top + (rh - footer_h) * 0.5f;
                draw_analog_dial((rect.left + rect.right) * 0.5f, dial_cy, rad);
                wchar_t date_buf[64]{};
                std::swprintf(date_buf, ARRAYSIZE(date_buf), L"%02u:%02u:%02u · %u月%u日 %ls",
                              st.wHour, st.wMinute, st.wSecond, st.wMonth, st.wDay, kWeekdays[st.wDayOfWeek % 7]);
                DrawWidgetText(date_buf, static_cast<UINT32>(std::wcslen(date_buf)), fmt_tile_small_title_.Get(),
                                  D2D1::RectF(rect.left + 12.0f, rect.bottom - 34.0f, rect.right - 12.0f, rect.bottom - 10.0f),
                                  Brush(text_sec));
            }
        } else {
            wchar_t time_buf[16]{};
            std::swprintf(time_buf, ARRAYSIZE(time_buf), L"%02u:%02u", st.wHour, st.wMinute);
            const float pad_x = tile.size.cols == 1 ? 8.0f : 16.0f;
            const float time_top = tile.size.cols == 1
                ? rect.top + 22.0f
                : rect.top + std::max(12.0f, (rect.bottom - rect.top - 72.0f) * 0.35f);
            DrawWidgetText(time_buf, static_cast<UINT32>(std::wcslen(time_buf)),
                              tile.size.cols == 1 ? fmt_group_title_.Get() : fmt_clock_big_.Get(),
                              D2D1::RectF(rect.left + pad_x, time_top,
                                          rect.right - pad_x, time_top + 50.0f),
                              Brush(text_pri));
            wchar_t date_buf[48]{};
            std::swprintf(date_buf, ARRAYSIZE(date_buf), L"%u月%u日 %ls",
                          st.wMonth, st.wDay, kWeekdays[st.wDayOfWeek % 7]);
            DrawWidgetText(date_buf, static_cast<UINT32>(std::wcslen(date_buf)),
                              tile.size.cols == 1 ? fmt_pill_.Get() : fmt_caption_.Get(),
                              D2D1::RectF(rect.left + pad_x, rect.bottom - 30.0f, rect.right - pad_x, rect.bottom - 8.0f),
                              Brush(text_sec));
        }
        break;
    }

    case model::TileKind::StickyNote: {
        // Paper and ink per colour (便笺颜色): dark theme keeps a deep tint, light theme a pastel.
        static constexpr uint32_t kNoteDark[6] = {0x6676621Eu, 0x5C2F6B3Eu, 0x5C2C5A86u, 0x5C7A3256u, 0x5C523A86u, 0x5C4A4F5Au};
        static constexpr uint32_t kNoteLight[6] = {0xB4FCE178u, 0xB4C8EFB4u, 0xB4BFDDF8u, 0xB4F8C9DCu, 0xB4DCCDF8u, 0xB4E3E6EBu};
        static constexpr uint32_t kInkLight[6] = {0xF23A2A0Du, 0xF2173A22u, 0xF2133250u, 0xF24A1733u, 0xF22E1F55u, 0xF2262A30u};
        static constexpr uint32_t kInkDark[6] = {0xF2FFF6D6u, 0xF2E3FFE8u, 0xF2E0F0FFu, 0xF2FFE3F0u, 0xF2EEE6FFu, 0xF2F0F2F5u};
        const int nc = std::clamp(tile_in.note_color, 0, 5);
        const uint32_t note_tint = dark ? kNoteDark[nc] : kNoteLight[nc];
        d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(rect, 16.0f, 16.0f), Brush(note_tint));
        if (with_grain) {
            if (auto* nb = backdrop_.EnsureNoiseBrush(d2d_dc_.Get())) {
                nb->SetOpacity(static_cast<float>((note_tint >> 24) & 0xFFu) / 255.0f);
                d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(rect, 16.0f, 16.0f), nb);
                nb->SetOpacity(1.0f);
            }
        }
        const std::wstring title = tile.title.empty() ? L"便笺" : tile.title;
        if (tile.size.cols >= 2) {
            draw_wide_header(title, L"自动保存");
        }

        const std::wstring body = tile.note_body.empty() ? L"单击写下待办与灵感…" : tile.note_body;
        const D2D1_RECT_F body_rc = D2D1::RectF(rect.left + 14.0f,
                                                rect.top + (tile.size.cols >= 2 ? 48.0f : 12.0f),
                                                rect.right - 14.0f,
                                                rect.bottom - (tile.size.cols >= 2 ? 10.0f : 26.0f));
        DrawWidgetText(body.c_str(), static_cast<UINT32>(body.size()), fmt_note_body_.Get(),
                          body_rc, Brush(dark ? kInkDark[nc] : kInkLight[nc]));
        if (tile.size.cols == 1 && show_title) {
            DrawWidgetText(title.c_str(), static_cast<UINT32>(title.size()), fmt_tile_small_title_.Get(),
                              D2D1::RectF(rect.left + 6.0f, rect.bottom - 24.0f, rect.right - 6.0f, rect.bottom - 4.0f),
                              Brush(hue));
        }
        break;
    }

    case model::TileKind::StagingTray: {
        const std::wstring title = tile.title.empty() ? L"暂存区" : tile.title;
        wchar_t sub_buf[48]{};
        if (live_data.tray_entries.empty()) {
            std::swprintf(sub_buf, ARRAYSIZE(sub_buf), L"和 Pulse 同步");
        } else {
            std::swprintf(sub_buf, ARRAYSIZE(sub_buf), L"%zu 项 · 单击展开", live_data.tray_entries.size());
        }
        draw_wide_header(title, sub_buf);

        if (live_data.tray_entries.empty()) {
            const std::wstring drop = L"把文件拖到这里，";
            const std::wstring press = L"或选中后按 ";
            const std::wstring kbd_txt = L"Ctrl+Shift+S";
            const float left = rect.left + 14.0f, room = rect.right - 12.0f - left;
            const float dw = MeasureSingleLineWidth(drop, fmt_caption_.Get(), 2000.0f);
            const float pw = MeasureSingleLineWidth(press, fmt_caption_.Get(), 2000.0f);
            const float kw = MeasureSingleLineWidth(kbd_txt, fmt_pill_.Get(), 400.0f) + 12.0f;
            const bool one_line = dw + pw + kw + 2.0f <= room;
            const bool with_key = pw + kw + 2.0f <= room;
            const float mid = (rect.top + (show_title ? 42.0f : 8.0f) + rect.bottom) * 0.5f;
            const float cy = one_line || !with_key ? mid : mid + 11.0f;  // the line that carries the key
            const auto caption = [&](const std::wstring& t, float x, float y, float w) {
                DrawWidgetText(t.c_str(), static_cast<UINT32>(t.size()), fmt_caption_.Get(),
                                  D2D1::RectF(x, y - 11.0f, x + w + 4.0f, y + 11.0f), Brush(text_sec, 0.85f));
            };
            float kx = left;
            if (one_line) {
                caption(drop, left, cy, dw);
                caption(press, left + dw, cy, pw);
                kx = left + dw + pw + 1.0f;
            } else if (with_key) {
                caption(drop.substr(0, drop.size() - 1), left, cy - 22.0f, dw);
                caption(press, left, cy, pw);
                kx = left + pw + 1.0f;
            } else {
                caption(drop.substr(0, drop.size() - 1), left, cy, room);
            }
            if (with_key) {
                const D2D1_RECT_F kbd_rc = D2D1::RectF(kx, cy - 9.5f, kx + kw, cy + 9.5f);
                d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(kbd_rc, 4.5f, 4.5f),
                                              Brush(dark ? 0x1EFFFFFFu : 0x10000000u));
                d2d_dc_->DrawRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(kbd_rc.left + 0.5f, kbd_rc.top + 0.5f, kbd_rc.right - 0.5f, kbd_rc.bottom - 0.5f), 4.0f, 4.0f),
                    Brush(dark ? 0x2EFFFFFFu : 0x1C000000u), 1.0f);
                DrawWidgetText(kbd_txt.c_str(), static_cast<UINT32>(kbd_txt.size()), fmt_pill_.Get(),
                                  kbd_rc, Brush(text_sec));
            }
        } else {
            const bool grid = tile.size.rows >= 2;
            const size_t room = TrayChipCapacity(tile_in, rect);
            // Keep the last cell for "+N" when the entries do not all fit.
            const size_t shown = live_data.tray_entries.size() > room ? (room > 0 ? room - 1 : 0) : live_data.tray_entries.size();
            size_t drawn = 0;
            float sx = rect.left + 14.0f;
            const float sy = rect.top + (show_title ? 48.0f : 12.0f);
            for (size_t i = 0; i < shown; ++i) {
                const auto& te = live_data.tray_entries[i];
                const D2D1_RECT_F cell = TrayChipRect(tile_in, rect, i);
                const float cx = (cell.left + cell.right) * 0.5f;
                const D2D1_RECT_F chip = grid ? D2D1::RectF(cx - 22.0f, cell.top, cx + 22.0f, cell.top + 44.0f) : cell;
                sx = cell.left;
                const bool chip_hot = hovered && interaction.hovered_tile_row == static_cast<int>(i);
                d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(chip, 10.0f, 10.0f),
                                              Brush(dark ? (chip_hot ? 0x28FFFFFFu : 0x1AFFFFFFu)
                                                         : (chip_hot ? 0x1A000000u : 0x10000000u)));
                d2d_dc_->DrawRoundedRectangle(D2D1::RoundedRect(chip, 10.0f, 10.0f),
                                              Brush(chip_hot ? accent : (dark ? 0x24FFFFFFu : 0x18000000u), chip_hot ? 0.65f : 1.0f), 1.0f);
                const float inset = grid ? 8.0f : 7.0f;
                DrawVectorFileIcon(d2d_factory_.Get(), d2d_dc_.Get(), dwrite_factory_.Get(),
                                   D2D1::RectF(chip.left + inset, chip.top + inset, chip.right - inset, chip.bottom - inset),
                                   te.name, te.is_folder);
                if (grid) {
                    DrawWidgetText(te.name.c_str(), static_cast<UINT32>(te.name.size()), fmt_tile_small_title_.Get(),
                                      D2D1::RectF(cell.left + 2.0f, chip.bottom + 4.0f, cell.right - 2.0f, cell.bottom), Brush(text_pri));
                }
                sx = cell.right + (grid ? 0.0f : 7.0f);
                ++drawn;
            }
            if (live_data.tray_entries.size() > drawn) {
                wchar_t more_buf[24]{};
                std::swprintf(more_buf, ARRAYSIZE(more_buf), L"+%zu", live_data.tray_entries.size() - drawn);
                const D2D1_RECT_F more = grid ? TrayChipRect(tile_in, rect, drawn) : D2D1::RectF(sx, sy, sx + 36.0f, sy + 40.0f);
                DrawWidgetText(more_buf, static_cast<UINT32>(std::wcslen(more_buf)), fmt_pill_.Get(),
                                  grid ? D2D1::RectF(more.left, more.top, more.right, more.top + 44.0f) : more, Brush(text_sec));
            }
        }
        break;
    }

    case model::TileKind::Apps: {
        std::vector<const model::DesktopItem*> apps;
        for (const auto& it : state.items) if (it.in_apps_tile) apps.push_back(&it);
        std::sort(apps.begin(), apps.end(), [](const auto* a, const auto* b) { return a->name < b->name; });
        wchar_t sub[32]{};
        if (!apps.empty()) std::swprintf(sub, ARRAYSIZE(sub), L"%zu 个快捷方式", apps.size());
        draw_wide_header(tile.title.empty() ? L"应用" : tile.title, sub);
        const float top = show_title ? rect.top + 48.0f : rect.top + 12.0f;
        if (apps.empty()) {
            const wchar_t* hint = L"把桌面上的应用快捷方式拖到这里";
            DrawWidgetText(hint, static_cast<UINT32>(std::wcslen(hint)), fmt_empty_hint_.Get(),
                              D2D1::RectF(rect.left + 14.0f, top, rect.right - 14.0f, rect.bottom - 10.0f), Brush(text_sec, 0.8f));
            break;
        }
        // Pages, as on a phone's home screen: the wheel turns them, dots at the bottom say where you are.
        const auto pg = AppsTilePaging(tile_in, rect, apps.size());
        const int cols = pg.cols;
        const float cell_w = (rect.right - rect.left - 16.0f) / static_cast<float>(cols);
        const float cell_h = pg.cell_h;
        const size_t first = static_cast<size_t>(std::min(interaction.apps_page, pg.pages - 1)) * pg.room;
        const size_t shown = std::min(pg.room, apps.size() - first);
        if (pg.pages > 1) {
            const float dot_y = rect.bottom - 11.0f;
            const int cur = std::min(interaction.apps_page, pg.pages - 1);
            float dx = (rect.left + rect.right) * 0.5f - (static_cast<float>(pg.pages) * 10.0f + 6.0f) * 0.5f;
            for (int p = 0; p < pg.pages; ++p) {
                const float w = p == cur ? 16.0f : 6.0f;
                d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(dx, dot_y - 3.0f, dx + w, dot_y + 3.0f), 3.0f, 3.0f),
                                              Brush(text_pri, p == cur ? 0.85f : 0.30f));
                dx += w + 4.0f;
            }
        }
        for (size_t k = 0; k < shown; ++k) {
            const size_t i = first + k;
            const float cx = rect.left + 8.0f + cell_w * (static_cast<float>(k % cols) + 0.5f);
            // Icon and label sit centred in the cell, whatever height the page gave it.
            const float cy = top + cell_h * static_cast<float>(k / cols) + (cell_h - 62.0f) * 0.5f;
            const float row_top = top + cell_h * static_cast<float>(k / cols);
            const D2D1_RECT_F cell = D2D1::RectF(cx - cell_w * 0.5f + 2.0f, row_top + 1.0f, cx + cell_w * 0.5f - 2.0f, row_top + cell_h - 1.0f);
            const bool pressed_cell = interaction.pressed_app_index == static_cast<int>(i);
            if (pressed_cell)
                d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(cell, 10.0f, 10.0f), Brush(dark ? 0x26FFFFFFu : 0x18000000u));
            else if (hovered && interaction.hovered_path == apps[i]->path)
                d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(cell, 10.0f, 10.0f), Brush(dark ? 0x16FFFFFFu : 0x0E000000u));
            // Pressed, the icon shrinks a little about its centre, as a phone's app icon does under a finger.
            const float dim = pressed_cell ? 32.0f : 36.0f;
            const float icon_dy = pressed_cell ? 2.0f : 0.0f;
            if (ID2D1Bitmap1* bmp = backdrop_.GetItemBitmap(d2d_dc_.Get(), apps[i]->path, false,
                                                            static_cast<uint32_t>(std::lround(dim * dip_scale_)))) {
                d2d_dc_->DrawBitmap(bmp, D2D1::RectF(cx - dim * 0.5f, cy + 4.0f + icon_dy, cx + dim * 0.5f, cy + 4.0f + icon_dy + dim), 1.0f,
                                    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            } else {
                DrawVectorFileIcon(d2d_factory_.Get(), d2d_dc_.Get(), dwrite_factory_.Get(),
                                   D2D1::RectF(cx - dim * 0.5f + 2.0f, cy + 6.0f + icon_dy, cx + dim * 0.5f - 2.0f, cy + 2.0f + icon_dy + dim), L".lnk", false);
            }
            std::wstring nm = apps[i]->name;
            if (nm.size() > 4 && _wcsicmp(nm.c_str() + nm.size() - 4, L".lnk") == 0) nm.resize(nm.size() - 4);
            const bool label_revealed = (hovered && interaction.hovered_path == apps[i]->path) ||
                interaction.selected_paths.contains(apps[i]->path) ||
                (!interaction.search_highlight_query.empty() &&
                 ContainsCaseInsensitive(apps[i]->name, interaction.search_highlight_query));
            const auto label_rect = D2D1::RectF(cell.left + 2.0f, cy + 42.0f, cell.right - 2.0f, cy + 60.0f);
            const auto background = TileBackgroundSamples(tile_in, rect, label_rect, mat, state.settings);
            DrawIconLabel(nm, label_rect, fmt_tile_small_title_.Get(), state.settings.label_style,
                          dark, true, label_revealed, 1.0f, &background);
        }
        break;
    }

    case model::TileKind::Weather: {
        LiveTileRuntimeData::WeatherView fallback_wx{};
        const LiveTileRuntimeData::WeatherView* wp = &live_data.weather;
        if (!wp->valid && wp->status.empty()) {
            fallback_wx = PreviewWeather(state.settings.weather_city);
            wp = &fallback_wx;
        }
        const auto& w = *wp;
        const std::wstring city = w.city.empty() ? (state.settings.weather_city.empty() ? L"北京" : state.settings.weather_city) : w.city;
        if (!w.valid) {
            draw_wide_header(tile.title.empty() ? L"天气" : tile.title, L"");
            const std::wstring msg = !w.status.empty()
                ? w.status
                : (city.empty() ? L"单击设置城市 · 查看实时天气" : L"正在获取天气…");
            DrawWidgetText(msg.c_str(), static_cast<UINT32>(msg.size()), tile.size.cols == 1 ? fmt_pill_.Get() : fmt_empty_hint_.Get(),
                              D2D1::RectF(rect.left + 12.0f, rect.top + (tile.size.cols == 1 ? 30.0f : 46.0f), rect.right - 12.0f, rect.bottom - 10.0f),
                              Brush(text_sec, 0.85f));
            break;
        }
        wchar_t tbuf[16]{};
        std::swprintf(tbuf, ARRAYSIZE(tbuf), L"%d°", w.temp_c);
        const wchar_t* cond_name = WeatherLabel(w.code);
        if (tile.size.cols == 1) {
            DrawWidgetText(city.c_str(), static_cast<UINT32>(city.size()), fmt_tile_hdr_.Get(),
                              D2D1::RectF(rect.left + 12.0f, rect.top + 8.0f, rect.right - 8.0f, rect.top + 26.0f), Brush(text_pri));
            DrawWeatherIcon(D2D1::RectF(rect.left + 10.0f, rect.top + 27.0f, rect.left + 34.0f, rect.top + 51.0f), w.code, dark);
            DrawWidgetText(cond_name, static_cast<UINT32>(std::wcslen(cond_name)), fmt_pill_.Get(),
                              D2D1::RectF(rect.left + 36.0f, rect.top + 31.0f, rect.right - 6.0f, rect.top + 49.0f), Brush(text_sec));
            DrawWidgetText(tbuf, static_cast<UINT32>(std::wcslen(tbuf)), fmt_clock_big_.Get(),
                              D2D1::RectF(rect.left + 12.0f, rect.top + 50.0f, rect.right - 6.0f, rect.bottom - 2.0f), Brush(text_pri));
            break;
        }
        const float left_w = tile.size.cols >= 4 ? 160.0f : (rect.right - rect.left);
        const wchar_t pin_g[2] = {L'\xE707', 0};
        DrawGlyph(d2d_dc_.Get(), pin_g, 1, fmt_fluent_12_.Get(),
                  D2D1::RectF(rect.left + 13.0f, rect.top + 12.0f, rect.left + 27.0f, rect.top + 29.0f),
                  Brush(text_sec));
        DrawWidgetText(city.c_str(), static_cast<UINT32>(city.size()), fmt_tile_hdr_.Get(),
                          D2D1::RectF(rect.left + 29.0f, rect.top + 11.0f, rect.left + left_w - 8.0f, rect.top + 29.0f), Brush(text_pri));
        const float temp_w = MeasureSingleLineWidth(tbuf, fmt_clock_big_.Get(), 110.0f);
        DrawWidgetText(tbuf, static_cast<UINT32>(std::wcslen(tbuf)), fmt_clock_big_.Get(),
                          D2D1::RectF(rect.left + 14.0f, rect.top + 30.0f, rect.left + 14.0f + temp_w + 8.0f, rect.top + 82.0f), Brush(text_pri));
        const float wx_ic_x = rect.left + 14.0f + temp_w + 6.0f;
        DrawWeatherIcon(D2D1::RectF(wx_ic_x, rect.top + 36.0f, wx_ic_x + 32.0f, rect.top + 68.0f), w.code, dark);

        const std::wstring cond_str(cond_name);
        const float cond_w = MeasureSingleLineWidth(cond_str, fmt_row_lead_.Get(), 96.0f);
        DrawWidgetText(cond_str.c_str(), static_cast<UINT32>(cond_str.size()), fmt_row_lead_.Get(),
                          D2D1::RectF(rect.left + 14.0f, rect.top + 83.0f, rect.left + 14.0f + cond_w + 4.0f, rect.top + 101.0f),
                          Brush(text_pri));
        wchar_t range_buf[64]{};
        if (tile.size.cols >= 4) {
            std::swprintf(range_buf, ARRAYSIZE(range_buf), L" · 最高 %d° 最低 %d°", w.hi_c, w.lo_c);
        } else {
            std::swprintf(range_buf, ARRAYSIZE(range_buf), L" · %d°/%d°", w.lo_c, w.hi_c);
        }
        DrawWidgetText(range_buf, static_cast<UINT32>(std::wcslen(range_buf)), fmt_caption_.Get(),
                          D2D1::RectF(rect.left + 14.0f + cond_w, rect.top + 83.0f, rect.left + left_w, rect.top + 101.0f),
                          Brush(text_sec));

        // Hours along the bottom (medium) or the right (wide).
        const size_t nh = std::min<size_t>(w.hours.size(), tile.size.cols >= 4 ? 5 : 4);
        if (nh) {
            const float hx0 = tile.size.cols >= 4 ? rect.left + 170.0f : rect.left + 12.0f;
            const float hx1 = rect.right - 14.0f;
            const float hy = tile.size.cols >= 4 ? rect.top + 14.0f : rect.bottom - 72.0f;
            if (tile.size.cols < 4)
                d2d_dc_->FillRectangle(D2D1::RectF(rect.left + 14.0f, hy - 8.0f, rect.right - 14.0f, hy - 7.0f), Brush(dark ? 0x1CFFFFFFu : 0x12000000u));
            const float step = (hx1 - hx0) / static_cast<float>(nh);
            for (size_t i = 0; i < nh; ++i) {
                const auto& h = w.hours[i];
                const float cx = hx0 + step * (static_cast<float>(i) + 0.5f);
                DrawWidgetText(h.label.c_str(), static_cast<UINT32>(h.label.size()), fmt_pill_.Get(),
                                  D2D1::RectF(cx - 24.0f, hy, cx + 24.0f, hy + 16.0f), Brush(text_sec));
                DrawWeatherIcon(D2D1::RectF(cx - 10.0f, hy + 18.0f, cx + 10.0f, hy + 38.0f), h.code, dark);
                wchar_t hb[12]{};
                std::swprintf(hb, ARRAYSIZE(hb), L"%d°", h.temp_c);
                DrawWidgetText(hb, static_cast<UINT32>(std::wcslen(hb)), fmt_disk_pct_.Get(),
                                  D2D1::RectF(cx - 24.0f, hy + 41.0f, cx + 24.0f, hy + 59.0f), Brush(text_pri));
            }
        }
        if ((tile.size.cols >= 4 && tile.size.rows >= 2) ||
            (tile.size.cols >= 2 && tile.size.cols < 4 && (rect.bottom - rect.top) >= 248.0f)) {
            const float x0 = tile.size.cols >= 4 ? (rect.left + 170.0f) : (rect.left + 14.0f);
            float dy = tile.size.cols >= 4 ? (rect.top + 92.0f) : (rect.top + 112.0f);
            const float max_dy = tile.size.cols >= 4 ? (rect.bottom - 6.0f) : (rect.bottom - 84.0f);
            int lo = 100, hi = -100;
            for (const auto& d : w.days) { lo = std::min(lo, d.lo_c); hi = std::max(hi, d.hi_c); }
            const float span = static_cast<float>(std::max(1, hi - lo));
            d2d_dc_->FillRectangle(D2D1::RectF(x0, dy - 6.0f, rect.right - 14.0f, dy - 5.0f), Brush(dark ? 0x1AFFFFFFu : 0x12000000u));
            for (size_t i = 0; i < w.days.size() && dy + 20.0f <= max_dy; ++i) {
                const auto& d = w.days[i];
                DrawWidgetText(d.label.c_str(), static_cast<UINT32>(d.label.size()), fmt_row_meta_.Get(),
                                  D2D1::RectF(x0, dy, x0 + 40.0f, dy + 20.0f), Brush(text_pri));
                DrawWeatherIcon(D2D1::RectF(x0 + 40.0f, dy + 1.0f, x0 + 58.0f, dy + 19.0f), d.code, dark);
                const float bx0 = x0 + 66.0f, bx1 = rect.right - 60.0f;
                if (bx1 > bx0 + 12.0f) {
                    const float a = bx0 + (bx1 - bx0) * static_cast<float>(d.lo_c - lo) / span;
                    const float b = bx0 + (bx1 - bx0) * static_cast<float>(d.hi_c - lo) / span;
                    d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx0, dy + 8.0f, bx1, dy + 12.0f), 2.0f, 2.0f), Brush(dark ? 0x1AFFFFFFu : 0x12000000u));
                    d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(a, dy + 8.0f, std::max(a + 4.0f, b), dy + 12.0f), 2.0f, 2.0f), Brush(0xFFF5B84Au, 0.9f));
                }
                wchar_t db[24]{};
                std::swprintf(db, ARRAYSIZE(db), L"%d°/%d°", d.lo_c, d.hi_c);
                DrawWidgetText(db, static_cast<UINT32>(std::wcslen(db)), fmt_row_meta_.Get(),
                                  D2D1::RectF(rect.right - 58.0f, dy, rect.right - 12.0f, dy + 20.0f), Brush(text_sec));
                dy += 24.0f;
            }
        }
        break;
    }

    case model::TileKind::Calendar: {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        static constexpr const wchar_t* kWd[7] = {L"周日", L"周一", L"周二", L"周三", L"周四", L"周五", L"周六"};
        wchar_t day[8]{};
        std::swprintf(day, ARRAYSIZE(day), L"%u", st.wDay);
        const wchar_t* wd = kWd[st.wDayOfWeek % 7];
        std::vector<const model::TodoItem*> open_todos;
        for (const auto& td : state.todos) {
            if (!td.done && !td.text.empty()) open_todos.push_back(&td);
        }
        static constexpr uint32_t kEvBarColors[3] = {0xFF4CA6FFu, 0xFF30A46Cu, 0xFFF5A524u};

        if (tile.size.cols == 1) {
            DrawWidgetText(wd, static_cast<UINT32>(std::wcslen(wd)), fmt_tile_hdr_.Get(),
                              D2D1::RectF(rect.left + 12.0f, rect.top + 10.0f, rect.right - 8.0f, rect.top + 26.0f), Brush(hue));
            DrawWidgetText(day, static_cast<UINT32>(std::wcslen(day)), fmt_clock_big_.Get(),
                              D2D1::RectF(rect.left + 12.0f, rect.top + 25.0f, rect.right - 8.0f, rect.top + 74.0f), Brush(text_pri));
            if (!open_todos.empty()) {
                DrawWidgetText(open_todos[0]->text.c_str(), static_cast<UINT32>(open_todos[0]->text.size()),
                                  fmt_caption_.Get(),
                                  D2D1::RectF(rect.left + 12.0f, rect.bottom - 24.0f, rect.right - 8.0f, rect.bottom - 8.0f),
                                  Brush(text_sec));
            } else {
                wchar_t md[32]{};
                std::swprintf(md, ARRAYSIZE(md), L"%u月%u日", st.wMonth, st.wDay);
                DrawWidgetText(md, static_cast<UINT32>(std::wcslen(md)), fmt_caption_.Get(),
                                  D2D1::RectF(rect.left + 12.0f, rect.bottom - 24.0f, rect.right - 8.0f, rect.bottom - 8.0f),
                                  Brush(text_sec));
            }
            break;
        }
        if (tile.size.cols < 4) {
            wchar_t md_wd[48]{};
            std::swprintf(md_wd, ARRAYSIZE(md_wd), L"%u月%u日 %ls", st.wMonth, st.wDay, wd);
            DrawWidgetText(md_wd, static_cast<UINT32>(std::wcslen(md_wd)), fmt_tile_hdr_.Get(),
                              D2D1::RectF(rect.left + 14.0f, rect.top + 12.0f, rect.right - 12.0f, rect.top + 30.0f), Brush(hue));
            if (!open_todos.empty()) {
                float ey = rect.top + 40.0f;
                size_t shown = 0;
                for (size_t i = 0; i < open_todos.size() && ey + 36.0f <= rect.bottom - 26.0f; ++i) {
                    d2d_dc_->FillRoundedRectangle(
                        D2D1::RoundedRect(D2D1::RectF(rect.left + 14.0f, ey + 2.0f, rect.left + 17.0f, ey + 34.0f), 1.5f, 1.5f),
                        Brush(kEvBarColors[i % 3]));
                    DrawWidgetText(open_todos[i]->text.c_str(), static_cast<UINT32>(open_todos[i]->text.size()),
                                      fmt_row_lead_.Get(),
                                      D2D1::RectF(rect.left + 24.0f, ey, rect.right - 12.0f, ey + 18.0f), Brush(text_pri));
                    DrawWidgetText(L"今日待办", 4, fmt_row_meta_.Get(),
                                      D2D1::RectF(rect.left + 24.0f, ey + 18.0f, rect.right - 12.0f, ey + 34.0f), Brush(text_sec));
                    ey += 44.0f;
                    ++shown;
                }
                if (open_todos.size() > shown) {
                    wchar_t more_buf[32]{};
                    std::swprintf(more_buf, ARRAYSIZE(more_buf), L"还有 %zu 项待办", open_todos.size() - shown);
                    DrawWidgetText(more_buf, static_cast<UINT32>(std::wcslen(more_buf)), fmt_caption_.Get(),
                                      D2D1::RectF(rect.left + 14.0f, rect.bottom - 26.0f, rect.right - 12.0f, rect.bottom - 8.0f),
                                      Brush(text_sec));
                }
            } else {
                DrawWidgetText(day, static_cast<UINT32>(std::wcslen(day)), fmt_clock_big_.Get(),
                                  D2D1::RectF(rect.left + 14.0f, rect.top + 38.0f, rect.right - 12.0f, rect.top + 96.0f), Brush(text_pri));
                DrawWidgetText(L"今天暂无待办事项", 8, fmt_caption_.Get(),
                                  D2D1::RectF(rect.left + 14.0f, rect.bottom - 28.0f, rect.right - 12.0f, rect.bottom - 10.0f),
                                  Brush(text_sec));
            }
            break;
        }
        // Wide: the month on the left, today & events on the right.
        const float gx = rect.left + 14.0f, gy = rect.top + 12.0f, cw = 26.0f, ch = 21.0f;
        wchar_t mt[16]{};
        std::swprintf(mt, ARRAYSIZE(mt), L"%u月", st.wMonth);
        DrawWidgetText(mt, static_cast<UINT32>(std::wcslen(mt)), fmt_tile_hdr_.Get(), D2D1::RectF(gx + 4.0f, gy, gx + 100.0f, gy + 18.0f), Brush(hue));
        static constexpr const wchar_t* kHd[7] = {L"一", L"二", L"三", L"四", L"五", L"六", L"日"};
        for (int c = 0; c < 7; ++c)
            DrawWidgetText(kHd[c], 1, fmt_pill_.Get(), D2D1::RectF(gx + c * cw, gy + 20.0f, gx + (c + 1) * cw, gy + 36.0f), Brush(text_sec));
        SYSTEMTIME first = st; first.wDay = 1;
        FILETIME ft{}; SystemTimeToFileTime(&first, &ft); FileTimeToSystemTime(&ft, &first);
        const int lead = (first.wDayOfWeek + 6) % 7;  // Monday first
        static constexpr int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        int mdays = kDays[(st.wMonth - 1) % 12];
        if (st.wMonth == 2 && ((st.wYear % 4 == 0 && st.wYear % 100 != 0) || st.wYear % 400 == 0)) mdays = 29;
        for (int d = 1; d <= mdays; ++d) {
            const int cell = lead + d - 1, c = cell % 7, r = cell / 7;
            const D2D1_RECT_F rc = D2D1::RectF(gx + c * cw, gy + 38.0f + r * ch, gx + (c + 1) * cw, gy + 38.0f + (r + 1) * ch);
            wchar_t db[4]{};
            std::swprintf(db, ARRAYSIZE(db), L"%d", d);
            if (d == st.wDay) {
                const float cx = (rc.left + rc.right) * 0.5f, cy = (rc.top + rc.bottom) * 0.5f;
                d2d_dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 9.5f, 9.5f), Brush(hue));
                DrawWidgetText(db, static_cast<UINT32>(std::wcslen(db)), fmt_pill_.Get(), rc, Brush(0xFFFFFFFFu));
            } else {
                DrawWidgetText(db, static_cast<UINT32>(std::wcslen(db)), fmt_pill_.Get(), rc, Brush(text_pri, 0.9f));
            }
        }
        const float rx = gx + 7 * cw + 18.0f;
        wchar_t today_hdr[32]{};
        std::swprintf(today_hdr, ARRAYSIZE(today_hdr), L"今天 · %ls", wd);
        DrawWidgetText(today_hdr, static_cast<UINT32>(std::wcslen(today_hdr)), fmt_tile_hdr_.Get(),
                          D2D1::RectF(rx, gy, rect.right - 12.0f, gy + 18.0f), Brush(hue));
        if (!open_todos.empty()) {
            float ey = gy + 28.0f;
            for (size_t i = 0; i < open_todos.size() && ey + 36.0f <= rect.bottom - 8.0f; ++i) {
                d2d_dc_->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(rx, ey + 2.0f, rx + 3.0f, ey + 34.0f), 1.5f, 1.5f),
                    Brush(kEvBarColors[i % 3]));
                DrawWidgetText(open_todos[i]->text.c_str(), static_cast<UINT32>(open_todos[i]->text.size()),
                                  fmt_row_lead_.Get(),
                                  D2D1::RectF(rx + 10.0f, ey, rect.right - 12.0f, ey + 18.0f), Brush(text_pri));
                DrawWidgetText(L"待办事项", 4, fmt_row_meta_.Get(),
                                  D2D1::RectF(rx + 10.0f, ey + 18.0f, rect.right - 12.0f, ey + 34.0f), Brush(text_sec));
                ey += 44.0f;
            }
        } else {
            DrawWidgetText(day, static_cast<UINT32>(std::wcslen(day)), fmt_clock_big_.Get(), D2D1::RectF(rx, gy + 22.0f, rect.right - 12.0f, gy + 76.0f), Brush(text_pri));
            wchar_t ymd[32]{};
            std::swprintf(ymd, ARRAYSIZE(ymd), L"%u年%u月", st.wYear, st.wMonth);
            DrawWidgetText(ymd, static_cast<UINT32>(std::wcslen(ymd)), fmt_caption_.Get(), D2D1::RectF(rx, gy + 78.0f, rect.right - 12.0f, gy + 96.0f), Brush(text_sec));
        }
        break;
    }

    case model::TileKind::Todo: {
        size_t done = 0;
        for (const auto& t : state.todos) done += t.done ? 1u : 0u;
        const size_t total = state.todos.size();
        if (tile.size.cols == 1) {
            const float cx = (rect.left + rect.right) * 0.5f, cy = rect.top + 38.0f, rad = 22.0f;
            d2d_dc_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad), Brush(dark ? 0x24FFFFFFu : 0x16000000u), 4.2f);
            if (total) DrawCircularArc(d2d_factory_.Get(), d2d_dc_.Get(), Brush(hue), cx, cy, rad, -90.0f,
                                       360.0f * static_cast<float>(done) / static_cast<float>(total), 4.2f, round_stroke_.Get());
            wchar_t b[16]{};
            std::swprintf(b, ARRAYSIZE(b), L"%zu/%zu", done, total);
            DrawWidgetText(b, static_cast<UINT32>(std::wcslen(b)), fmt_disk_pct_.Get(), D2D1::RectF(cx - rad, cy - 10.0f, cx + rad, cy + 10.0f), Brush(text_pri));
            if (show_title) {
                const std::wstring t = tile.title.empty() ? L"待办" : tile.title;
                DrawWidgetText(t.c_str(), static_cast<UINT32>(t.size()), fmt_tile_small_title_.Get(),
                                  D2D1::RectF(rect.left + 4.0f, rect.bottom - 26.0f, rect.right - 4.0f, rect.bottom - 6.0f), Brush(text_pri));
            }
            break;
        }
        wchar_t sub[24]{};
        std::swprintf(sub, ARRAYSIZE(sub), L"%zu/%zu 完成", done, total);
        draw_wide_header(tile.title.empty() ? L"待办" : tile.title, sub);
        float ry = show_title ? rect.top + 46.0f : rect.top + 12.0f;
        for (size_t i = 0; i < state.todos.size() && ry + 26.0f <= rect.bottom - 6.0f; ++i) {
            const auto& td = state.todos[i];
            const D2D1_RECT_F row = D2D1::RectF(rect.left + 8.0f, ry, rect.right - 8.0f, ry + 27.0f);
            if (interaction.hovered_tile_id == tile_in.id && interaction.hovered_todo_index == static_cast<int>(i))
                d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(row, 8.0f, 8.0f), Brush(dark ? 0x14FFFFFFu : 0x0C000000u));
            const float cx = row.left + 14.0f, cy = (row.top + row.bottom) * 0.5f;
            if (td.done) {
                d2d_dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 8.0f, 8.0f), Brush(hue));
                const wchar_t ck[2] = {L'\xE73E', 0};
                // On a coloured tile the circle takes the text colour, so the tick takes the tile's colour.
                const uint32_t tick = look.background == model::TileBackground::Color ? (look.color_argb | 0xFF000000u) : 0xFFFFFFFFu;
                DrawGlyph(d2d_dc_.Get(), ck, 1, fmt_fluent_12_.Get(), D2D1::RectF(cx - 7.0f, cy - 7.0f, cx + 7.0f, cy + 7.0f), Brush(tick));
            } else {
                d2d_dc_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 7.5f, 7.5f), Brush(text_sec), 1.5f);
            }
            const bool row_hot = interaction.hovered_tile_id == tile_in.id && interaction.hovered_todo_index == static_cast<int>(i);
            DrawWidgetText(td.text.c_str(), static_cast<UINT32>(td.text.size()), fmt_row_name_.Get(),
                              D2D1::RectF(row.left + 30.0f, row.top, row.right - (row_hot ? 34.0f : 6.0f), row.bottom), Brush(td.done ? text_sec : text_pri));
            if (row_hot) {
                // Hovered: × removes it (undoable), as a swipe would on a phone.
                const D2D1_RECT_F xb = TodoDeleteRect(tile_in, rect, i);
                const float xc = (xb.left + xb.right) * 0.5f, yc = (xb.top + xb.bottom) * 0.5f;
                d2d_dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(xc, yc), 9.0f, 9.0f), Brush(dark ? 0x26FFFFFFu : 0x14000000u));
                d2d_dc_->DrawLine(D2D1::Point2F(xc - 3.2f, yc - 3.2f), D2D1::Point2F(xc + 3.2f, yc + 3.2f), Brush(text_sec), 1.4f, round_stroke_.Get());
                d2d_dc_->DrawLine(D2D1::Point2F(xc + 3.2f, yc - 3.2f), D2D1::Point2F(xc - 3.2f, yc + 3.2f), Brush(text_sec), 1.4f, round_stroke_.Get());
            }
            if (td.done) {
                const float tw = std::min(row.right - row.left - 36.0f, MeasureSingleLineWidth(td.text, fmt_row_name_.Get(), 400.0f));
                d2d_dc_->FillRectangle(D2D1::RectF(row.left + 30.0f, cy, row.left + 30.0f + tw, cy + 1.0f), Brush(text_sec));
            }
            ry += 29.0f;
        }
        if (ry + 25.0f <= rect.bottom - 6.0f) {
            const D2D1_RECT_F add_row = D2D1::RectF(rect.left + 8.0f, ry, rect.right - 8.0f, ry + 26.0f);
            const bool add_hot = interaction.hovered_tile_id == tile_in.id && interaction.hovered_tile_action;
            if (add_hot) {
                d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(add_row, 8.0f, 8.0f),
                                              Brush(dark ? 0x16FFFFFFu : 0x0E000000u));
            }
            const wchar_t plus_g[2] = {L'\xE710', 0};
            const float cx = add_row.left + 14.0f, cy = (add_row.top + add_row.bottom) * 0.5f;
            DrawGlyph(d2d_dc_.Get(), plus_g, 1, fmt_fluent_12_.Get(),
                      D2D1::RectF(cx - 7.0f, cy - 7.0f, cx + 7.0f, cy + 7.0f), Brush(hue));
            const wchar_t* add_txt = L"添加待办…";
            DrawWidgetText(add_txt, static_cast<UINT32>(std::wcslen(add_txt)), fmt_row_name_.Get(),
                              D2D1::RectF(add_row.left + 30.0f, add_row.top, add_row.right - 6.0f, add_row.bottom),
                              Brush(add_hot ? text_pri : text_sec));
        } else if (state.todos.empty()) {
            const wchar_t* hint = L"单击添加待办事项…";
            DrawWidgetText(hint, static_cast<UINT32>(std::wcslen(hint)), fmt_empty_hint_.Get(),
                              D2D1::RectF(rect.left + 14.0f, ry, rect.right - 14.0f, rect.bottom - 8.0f), Brush(text_sec, 0.8f));
        }
        break;
    }

    case model::TileKind::System: {
        const auto& sy = live_data.system;
        const int mem_pct = sy.mem_total ? static_cast<int>(sy.mem_used * 100 / sy.mem_total) : 0;
        if (tile.size.cols == 1) {
            constexpr float r = 14.0f, stroke = 3.2f;
            auto ring = [&](float cy, int pct, uint32_t c, const wchar_t* lbl) {
                const float cx = rect.left + 12.0f + r;
                d2d_dc_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), Brush(dark ? 0x24FFFFFFu : 0x16000000u), stroke);
                DrawCircularArc(d2d_factory_.Get(), d2d_dc_.Get(), Brush(c), cx, cy, r, -90.0f, 3.6f * static_cast<float>(std::clamp(pct, 0, 100)), stroke, round_stroke_.Get());
                wchar_t b[8]{};
                std::swprintf(b, ARRAYSIZE(b), L"%d", pct);
                DrawWidgetText(b, static_cast<UINT32>(std::wcslen(b)), fmt_pill_.Get(), D2D1::RectF(cx - r, cy - 8.0f, cx + r, cy + 8.0f), Brush(text_pri));
                // Label beside the ring; the right edge leaves room for a stack's page dots.
                DrawWidgetText(lbl, static_cast<UINT32>(std::wcslen(lbl)), fmt_pill_.Get(),
                                  D2D1::RectF(cx + r + 6.0f, cy - 8.0f, rect.right - 14.0f, cy + 8.0f), Brush(text_sec));
            };
            const float mid = (rect.top + rect.bottom) * 0.5f;
            const float gap = std::min(21.0f, (rect.bottom - rect.top) * 0.5f - r - 6.0f);
            ring(mid - gap, sy.cpu_pct, hue, L"CPU");
            ring(mid + gap, mem_pct, 0xFF30A46Cu, L"内存");
            break;
        }
        draw_wide_header(tile.title.empty() ? L"系统" : tile.title, sy.valid ? L"" : L"读取中");
        if (tile.size.cols == 2 && tile.size.rows >= 2) {
            // 2x2: four gauges in a grid — the value inside each ring, its name below — as a phone's battery widget.
            const int net_pct = static_cast<int>(std::min<uint64_t>(100, sy.net_down_bps / (1024 * 1024 / 4)));
            struct Gauge { const wchar_t* label; int pct; uint32_t color; std::wstring inside; };
            const Gauge gauges[4] = {{L"CPU", sy.cpu_pct, hue, std::to_wstring(sy.cpu_pct) + L"%"},
                                     {L"内存", mem_pct, 0xFF30A46Cu, std::to_wstring(mem_pct) + L"%"},
                                     {L"C: 盘", sy.disk_used_pct, 0xFFF5A524u, std::to_wstring(sy.disk_used_pct) + L"%"},
                                     {L"网络 ↓", net_pct, 0xFF8E6BE8u, FormatBytesCompact(sy.net_down_bps)}};
            const float gtop = show_title ? rect.top + 44.0f : rect.top + 8.0f;
            const float cell_w = (rect.right - rect.left - 16.0f) * 0.5f, cell_h = (rect.bottom - 6.0f - gtop) * 0.5f;
            const float rad = std::max(10.0f, std::min(cell_w, cell_h - 16.0f) * 0.5f - 6.0f);
            for (int i = 0; i < 4; ++i) {
                const auto& g = gauges[i];
                const float cx = rect.left + 8.0f + cell_w * (static_cast<float>(i % 2) + 0.5f);
                const float cy = gtop + cell_h * static_cast<float>(i / 2) + rad + 4.0f;
                d2d_dc_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad), Brush(dark ? 0x24FFFFFFu : 0x16000000u), 4.0f);
                DrawCircularArc(d2d_factory_.Get(), d2d_dc_.Get(), Brush(g.color), cx, cy, rad, -90.0f,
                                3.6f * static_cast<float>(std::clamp(g.pct, 0, 100)), 4.0f, round_stroke_.Get());
                DrawWidgetText(g.inside.c_str(), static_cast<UINT32>(g.inside.size()), i == 3 ? fmt_pill_.Get() : fmt_disk_pct_.Get(),
                                  D2D1::RectF(cx - rad, cy - 9.0f, cx + rad, cy + 9.0f), Brush(text_pri));
                DrawWidgetText(g.label, static_cast<UINT32>(std::wcslen(g.label)), fmt_pill_.Get(),
                                  D2D1::RectF(cx - cell_w * 0.5f, cy + rad + 3.0f, cx + cell_w * 0.5f, cy + rad + 19.0f), Brush(text_sec));
            }
            break;
        }
        float my = show_title ? rect.top + 54.0f : rect.top + 16.0f;
        auto metric = [&](const wchar_t* lbl, int pct, const std::wstring& val, uint32_t c) {
            DrawWidgetText(lbl, static_cast<UINT32>(std::wcslen(lbl)), fmt_row_meta_.Get(), D2D1::RectF(rect.left + 14.0f, my - 8.0f, rect.left + 50.0f, my + 10.0f), Brush(text_sec));
            const float bx0 = rect.left + 52.0f, bx1 = rect.right - 82.0f;
            d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx0, my - 3.0f, bx1, my + 3.0f), 3.0f, 3.0f), Brush(dark ? 0x1AFFFFFFu : 0x12000000u));
            d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx0, my - 3.0f, bx0 + (bx1 - bx0) * std::clamp(pct, 0, 100) / 100.0f, my + 3.0f), 3.0f, 3.0f), Brush(c));
            DrawWidgetText(val.c_str(), static_cast<UINT32>(val.size()), fmt_row_name_.Get(), D2D1::RectF(rect.right - 78.0f, my - 9.0f, rect.right - 12.0f, my + 10.0f), Brush(text_pri));
            my += 30.0f;
        };
        wchar_t cpu[16]{};
        std::swprintf(cpu, ARRAYSIZE(cpu), L"%d%%", sy.cpu_pct);
        metric(L"CPU", sy.cpu_pct, cpu, hue);
        metric(L"内存", mem_pct, FormatBytesCompact(sy.mem_used), 0xFF30A46Cu);
        metric(L"C:", sy.disk_used_pct, L"剩 " + FormatBytesCompact(sy.disk_free), 0xFFF5A524u);
        if (tile.size.cols >= 4 && my + 10.0f <= rect.bottom) {
            const int net_pct = static_cast<int>(std::min<uint64_t>(100, sy.net_down_bps / (1024 * 1024 / 4)));
            metric(L"网络", net_pct, L"↓ " + FormatBytesCompact(sy.net_down_bps) + L"/s", 0xFF8E6BE8u);
        }
        break;
    }

    case model::TileKind::PinnedItem: {
        const std::wstring title = tile.title.empty() ? L"固定项目" : tile.title;
        draw_wide_header(title, L"双击打开");
        DrawVectorFileIcon(d2d_factory_.Get(), d2d_dc_.Get(), dwrite_factory_.Get(),
                           D2D1::RectF(rect.left + 16.0f, rect.top + 42.0f, rect.left + 52.0f, rect.top + 78.0f),
                           tile.target_path.empty() ? std::wstring_view(title) : std::wstring_view(tile.target_path),
                           false);
        DrawWidgetText(tile.target_path.c_str(), static_cast<UINT32>(tile.target_path.size()),
                          fmt_caption_.Get(),
                          D2D1::RectF(rect.left + 60.0f, rect.top + 48.0f, rect.right - 14.0f, rect.top + 72.0f),
                          Brush(text_sec));
        break;
    }
    }
}

void DesktopRenderer::DrawSceneBanner(const model::DesktopModelState& state,
                                      const DesktopInteractionState& interaction) {
    if (interaction.banner_suppressed) return;

    wchar_t icon_glyph = 0;
    std::wstring msg;
    std::wstring btn_txt;

    if (state.settings.active_scene == model::DesktopScene::Focus) {
        icon_glyph = L'\xE7C4';
        msg = L"专注：只展开「进行中」，其余分组和磁贴先淡下去";
        btn_txt = L"退出专注";
    } else if (state.settings.active_scene == model::DesktopScene::Present) {
        icon_glyph = L'\xE7F4';
        msg = L"演示：桌面图标、最近文件、暂存区和便笺已隐藏";
        btn_txt = L"退出演示";
    } else if (state.settings.icons_hidden) {
        icon_glyph = L'\xE890';
        msg = L"桌面图标已隐藏 · 双击空白处恢复";
        btn_txt = L"显示图标";
    } else {
        return;
    }

    const model::RectI wa = !state.monitors.empty()
                                ? state.monitors.front().work_area
                                : model::RectI{0, 0, static_cast<int>(width_), static_cast<int>(height_) - 48};

    const bool dark = state.settings.dark_mode;
    const uint32_t accent = ResolveAccentArgb(0, state.settings, interaction);

    const float msg_w = MeasureSingleLineWidth(msg, fmt_banner_text_.Get(), 640.0f);
    const float btn_w = MeasureSingleLineWidth(btn_txt, fmt_pill_.Get(), 160.0f) + 22.0f;
    const float total_w = 36.0f + msg_w + 16.0f + btn_w + 10.0f;

    const float bx = static_cast<float>(wa.x + 24);
    const float by = static_cast<float>(wa.bottom() - 20 - 36);
    const D2D1_RECT_F banner_rc = D2D1::RectF(bx, by, bx + total_w, by + 36.0f);

    DrawCardMaterial(banner_rc, 18.0f, model::CardMaterial::Acrylic, 88, dark, accent, false, false);

    const wchar_t gl[2] = {icon_glyph, 0};
    DrawGlyph(d2d_dc_.Get(), gl, 1, fmt_fluent_14_.Get(),
                      D2D1::RectF(bx + 12.0f, by, bx + 30.0f, by + 36.0f), Brush(accent));

    DrawWidgetText(msg.c_str(), static_cast<UINT32>(msg.size()), fmt_banner_text_.Get(),
                      D2D1::RectF(bx + 34.0f, by, bx + 36.0f + msg_w + 4.0f, by + 36.0f),
                      Brush(dark ? 0xF5FFFFFFu : 0xF51B2028u));

    const D2D1_RECT_F btn_rc = D2D1::RectF(banner_rc.right - btn_w - 6.0f, by + 6.0f,
                                           banner_rc.right - 6.0f, by + 30.0f);
    d2d_dc_->FillRoundedRectangle(D2D1::RoundedRect(btn_rc, 12.0f, 12.0f), Brush(accent, 0.22f));
    d2d_dc_->DrawRoundedRectangle(D2D1::RoundedRect(btn_rc, 12.0f, 12.0f), Brush(accent, 0.45f), 1.0f);
    DrawWidgetText(btn_txt.c_str(), static_cast<UINT32>(btn_txt.size()), fmt_pill_.Get(),
                      btn_rc, Brush(accent));

    last_banner_action_rect_ = model::RectI{
        static_cast<int>(btn_rc.left),
        static_cast<int>(btn_rc.top),
        static_cast<int>(btn_rc.right - btn_rc.left),
        static_cast<int>(btn_rc.bottom - btn_rc.top),
    };
}

} // namespace pulse::desktop::render

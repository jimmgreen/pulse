#include "../common/windows_compat.h"
#include "file_operation_dialog.h"
#include "../ops/operation_presentation.h"
#include "../common/localization.h"
#include "../common/text_format.h"
#include "typography.h"
#include "window_helpers.h"

#include <windowsx.h>
#include <commctrl.h>
#include <commoncontrols.h>
#include <shellapi.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <imm.h>
#include <wincodec.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <map>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace pulse::ui {
namespace {

// Footer bands: faint shading over Mica or solid, a see-through sheet over
// acrylic (对话框亚克力 ④).
D2D1_COLOR_F OpFooter(HWND hwnd, bool backdrop, bool dark) {
    if (backdrop && IsAcrylicBackdrop(hwnd)) return GlassSheet(dark);
    return dark ? HexColor(0x000000, 0.2f) : HexColor(0x000000, 0.045f);
}

constexpr wchar_t kTransferClass[] = L"PulseFileOperationWindow";
constexpr wchar_t kConflictClass[] = L"PulseFileConflictWindow";
constexpr wchar_t kDeleteClass[] = L"PulsePermanentDeleteWindow";
constexpr wchar_t kLockedClass[] = L"PulseLockedItemWindow";
constexpr UINT_PTR kRenderTimer = 1;
// Hides the card when its exit zoom ends, whatever the render timer does.
constexpr UINT_PTR kCloseTimer = 6;
// Progress window, Pulse_文件操作对话框_重设计.html §1 (DIPs). The footer
// stays anchored to the bottom edge; everything above it is laid out from the
// top: title 8, route 34, percent 62, graph 100–156, rows 166–202.
constexpr float kDlgW = 440.0f;
constexpr float kTitleH = 40.0f;      // drag strip
constexpr float kCaptionH = 36.0f;    // minimize / close buttons
constexpr float kPadX = 16.0f;
constexpr float kFooterH = 56.0f;
constexpr float kContentH = 216.0f;   // above the footer, collapsed
constexpr float kCollapsedH = kContentH + kFooterH;
constexpr float kDetailsBlockH = 150.0f;
constexpr float kDetailedH = kCollapsedH + kDetailsBlockH;
// Permission view (HTML 1j): no numbers, graph or rows, only the title, the
// route and the shield's message (at most three lines from 62), so the card
// shrinks to that while Windows asks and grows back afterwards.
constexpr float kAuthorizationH = 140.0f + kFooterH;
// Partial failure: the failed items are listed in place of the percent,
// graph and rows; the window grows or shrinks to fit the list.
constexpr float kFailListTop = 90.0f;
constexpr float kFailRowH = 36.0f;
constexpr size_t kFailMaxRows = 4;
constexpr float kFailMoreH = 22.0f;
constexpr int kHitOpen = 8;
constexpr int kHitFailRow = 20;   // + visible row
constexpr size_t kTimeGraphPoints = ops::OpSpeedGraph::kMaxTimePoints;
constexpr float kPi = 3.14159265f;

using pulse::ui::ScaleDip;

struct TransferChrome {
    D2D1_RECT_F minimize{};
    D2D1_RECT_F close{};
    D2D1_RECT_F details{};
    D2D1_RECT_F pause{};
    D2D1_RECT_F cancel{};
    D2D1_RECT_F retry{};
    D2D1_RECT_F skip{};
    D2D1_RECT_F open{};
};

struct TransferLabels {
    std::wstring details;
    std::wstring pause;
    const wchar_t* pause_glyph = L"\xF8AE";
    std::wstring cancel;
    std::wstring retry;
    std::wstring skip;
    bool show_details = true;
    bool show_retry = false;
    bool show_skip = false;
    bool actions_enabled = true;   // false while a retry is in flight
    std::wstring retry_reserve;    // widest retry label, keeps the layout fixed
    bool show_pause = true;
    // Partial failure: 打开目标文件夹, and the cancel slot becomes 重试失败项.
    bool show_open = false;
    std::wstring open;
    bool cancel_primary = false;
    bool cancel_retries = false;
};

// A pause shows from the click on: the worker reports Paused only at its next
// check, which a buffered write held by the cache throttle can delay.
bool ShowsPaused(const ops::OpStatus& status) {
    if (status.phase == ops::OpPhase::Paused) return true;
    return status.active && status.pause_requested &&
           (status.phase == ops::OpPhase::Running || status.phase == ops::OpPhase::Scanning);
}

// Asked for, not yet confirmed by the worker: the chip reads 正在暂停….
bool PauseInFlight(const ops::OpStatus& status) {
    return status.phase != ops::OpPhase::Paused && ShowsPaused(status);
}

TransferLabels MakeTransferLabels(const ops::OpStatus& status, bool detailed,
                                  const TransferFailureView& partial = {}) {
    TransferLabels labels;
    labels.details = detailed ? l10n::Get(l10n::StringId::OpSummary)
                              : l10n::Get(l10n::StringId::OpDetails);
    const bool paused = ShowsPaused(status);
    labels.pause = paused ? l10n::Get(l10n::StringId::OpResume) : l10n::Get(l10n::StringId::OpPause);
    labels.pause_glyph = paused ? L"\xEDDA" : L"\xF8AE";  // solid, as the HTML (F5B0 is the toolbar split icon)
    const bool emptying = status.type == ops::OpType::EmptyRecycle;
    const bool authorization = status.authorization != ops::AuthorizationState::None;
    labels.show_details = !authorization;
    labels.show_retry = status.active && status.authorization == ops::AuthorizationState::ActionRequired
        && status.phase != ops::OpPhase::Cancelling;
    labels.show_skip = labels.show_retry && status.can_skip_authorization;
    labels.actions_enabled = !status.authorization_retrying;
    labels.retry = l10n::Get(l10n::StringId::OpRetryAuthorization);
    const std::wstring retrying = l10n::Get(l10n::StringId::Retrying).c_str();
    labels.retry_reserve = retrying.size() > labels.retry.size() ? retrying : labels.retry;
    if (!labels.actions_enabled) labels.retry = retrying;
    labels.skip = l10n::Get(l10n::StringId::OpSkipAuthorization);
    labels.show_pause = !authorization && !emptying && status.active && status.can_pause;
    labels.cancel = status.active && !emptying
        ? l10n::Get(l10n::StringId::Cancel) : l10n::Get(l10n::StringId::Close);
    if (partial.on && !status.active) {
        labels.show_open = partial.can_open;
        labels.open = l10n::Get(l10n::StringId::OpOpenDestination);
        labels.cancel_retries = partial.can_retry;
        labels.cancel_primary = partial.can_retry;
        if (partial.can_retry) labels.cancel = l10n::Get(l10n::StringId::OpRetryFailedItems);
    } else if (!status.active && partial.can_open &&
               (status.type == ops::OpType::Copy || status.type == ops::OpType::Move) &&
               (status.phase == ops::OpPhase::Completed || status.phase == ops::OpPhase::Failed)) {
        labels.show_open = true;
        labels.open = l10n::Get(l10n::StringId::OpOpenDestination);
    }
    return labels;
}

TransferChrome MakeTransferChrome(float scale, float width, float height,
                                  const fluent::Painter& painter, const TransferLabels& labels) {
    TransferChrome chrome;
    const float title = ScaleDip(scale, kCaptionH);
    const float caption = ScaleDip(scale, 40.0f);
    chrome.close = D2D1::RectF(width - caption, 0, width, title);
    chrome.minimize = D2D1::RectF(width - caption * 2.0f, 0, width - caption, title);
    const float footer_h = ScaleDip(scale, kFooterH);
    const float footer_top = height - footer_h;
    const float pad = ScaleDip(scale, kPadX);
    const float gap = ScaleDip(scale, 8.0f);
    const float btn_h = painter.MeasureButtonHeight();
    const float y0 = footer_top + (footer_h - btn_h) * 0.5f;
    const float y1 = y0 + btn_h;

    const float cancel_w = painter.MeasureButtonWidth(labels.cancel);
    chrome.cancel = D2D1::RectF(width - pad - cancel_w, y0, width - pad, y1);
    if (labels.show_pause) {
        const float pause_w = (std::max)(
            painter.MeasureButtonWidth(l10n::Get(l10n::StringId::OpPause), L"\xF8AE"),
            painter.MeasureButtonWidth(l10n::Get(l10n::StringId::OpResume), L"\xEDDA"));
        chrome.pause = D2D1::RectF(chrome.cancel.left - gap - pause_w, y0,
                                   chrome.cancel.left - gap, y1);
    }
    float next_right = chrome.cancel.left - gap;
    if (labels.show_open) {
        const float open_w = painter.MeasureButtonWidth(labels.open);
        chrome.open = D2D1::RectF(next_right - open_w, y0, next_right, y1);
        next_right = chrome.open.left - gap;
    }
    if (labels.show_retry) {
        const float retry_w = (std::max)(painter.MeasureButtonWidth(labels.retry),
                                         painter.MeasureButtonWidth(labels.retry_reserve));
        chrome.retry = D2D1::RectF(next_right - retry_w, y0, next_right, y1);
    }
    const float details_x = pad - ScaleDip(scale, 8.0f);
    // The permission view has no details: 跳过此项 takes that slot, a ghost
    // button like the other dialogs' way out.
    if (labels.show_skip)
        chrome.skip = D2D1::RectF(details_x, y0, details_x + painter.MeasureButtonWidth(labels.skip), y1);
    if (labels.show_details) chrome.details = painter.FitButtonBounds(D2D1::RectF(details_x, y0, details_x, y1),
                                             labels.details, L"\xE70D");
    return chrome;
}

void DrawEllipsizedText(Compositor& compositor, IDWriteTextFormat* format,
                        const D2D1_RECT_F& bounds, const std::wstring& text,
                        const D2D1_COLOR_F& color, bool wrap = false) {
    auto* factory = compositor.DwriteFactory();
    auto* dc = compositor.Dc();
    if (!factory || !dc || !format || text.empty()) return;
    const float width = bounds.right - bounds.left;
    const float height = bounds.bottom - bounds.top;
    if (width <= 0.0f || height <= 0.0f) return;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(factory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                         format, width, height, &layout))) return;
    layout->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    ComPtr<IDWriteInlineObject> ellipsis;
    if (SUCCEEDED(factory->CreateEllipsisTrimmingSign(format, &ellipsis))) {
        const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        layout->SetTrimming(&trimming, ellipsis.get());
    }
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(color, &brush))) return;
    compositor.DrawTextLayout(D2D1::Point2F(bounds.left, bounds.top), layout.get(), brush.get(),
                              D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float TextWidth(IDWriteFactory2* factory, IDWriteTextFormat* format, const std::wstring& text) {
    if (!factory || !format || text.empty()) return 0.0f;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(factory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                         format, 100000.0f, 1000.0f, &layout))) return 0.0f;
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) return 0.0f;
    return metrics.widthIncludingTrailingWhitespace;
}

ComPtr<ID2D1Factory> FactoryOf(ID2D1DeviceContext* dc) {
    ComPtr<ID2D1Factory> factory;
    if (dc) dc->GetFactory(&factory);
    return factory;
}

ComPtr<ID2D1StrokeStyle> RoundStroke(ID2D1Factory* factory) {
    ComPtr<ID2D1StrokeStyle> style;
    if (factory) {
        factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
            D2D1_LINE_JOIN_ROUND), nullptr, 0, &style);
    }
    return style;
}

// Clockwise arc from `start_deg` (0 = 12 o'clock) spanning `span_deg`.
void StrokeArc(ID2D1DeviceContext* dc, D2D1_POINT_2F center, float radius, float start_deg,
               float span_deg, D2D1_COLOR_F color, float stroke) {
    if (!dc || span_deg <= 0.0f || radius <= 0.0f) return;
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(color, &brush))) return;
    if (span_deg >= 359.5f) {
        dc->DrawEllipse(D2D1::Ellipse(center, radius, radius), brush.get(), stroke);
        return;
    }
    auto factory = FactoryOf(dc);
    if (!factory.get()) return;
    const float a0 = (start_deg - 90.0f) * kPi / 180.0f;
    const float a1 = (start_deg + span_deg - 90.0f) * kPi / 180.0f;
    ComPtr<ID2D1PathGeometry> path;
    if (FAILED(factory->CreatePathGeometry(&path))) return;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(path->Open(&sink))) return;
    sink->BeginFigure(D2D1::Point2F(center.x + radius * std::cos(a0), center.y + radius * std::sin(a0)),
                      D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(
        D2D1::Point2F(center.x + radius * std::cos(a1), center.y + radius * std::sin(a1)),
        D2D1::SizeF(radius, radius), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
        span_deg > 180.0f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if (FAILED(sink->Close())) return;
    auto style = RoundStroke(factory.get());
    dc->DrawGeometry(path.get(), brush.get(), stroke, style.get());
}

// Speed area (the HTML's svg): midpoint-x cubic segments, a flat fill under
// the curve and a 1.5-DIP line on top.
void DrawSpeedArea(ID2D1DeviceContext* dc, const D2D1_RECT_F& plot,
                   const std::vector<D2D1_POINT_2F>& points, D2D1_COLOR_F line_color,
                   D2D1_COLOR_F fill_color, float scale) {
    if (!dc || points.size() < 2) return;
    auto factory = FactoryOf(dc);
    if (!factory.get()) return;
    auto add_curve = [&](ID2D1GeometrySink* sink) {
        for (size_t i = 1; i < points.size(); ++i) {
            const float mx = (points[i - 1].x + points[i].x) * 0.5f;
            sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(mx, points[i - 1].y),
                                                D2D1::Point2F(mx, points[i].y), points[i]));
        }
    };
    ComPtr<ID2D1PathGeometry> area;
    if (SUCCEEDED(factory->CreatePathGeometry(&area))) {
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(area->Open(&sink))) {
            sink->BeginFigure(D2D1::Point2F(points.front().x, plot.bottom), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(points.front());
            add_curve(sink.get());
            sink->AddLine(D2D1::Point2F(points.back().x, plot.bottom));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            ComPtr<ID2D1SolidColorBrush> fill;
            if (SUCCEEDED(sink->Close()) && SUCCEEDED(dc->CreateSolidColorBrush(fill_color, &fill)))
                dc->FillGeometry(area.get(), fill.get());
        }
    }
    ComPtr<ID2D1PathGeometry> line;
    if (SUCCEEDED(factory->CreatePathGeometry(&line))) {
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(line->Open(&sink))) {
            sink->BeginFigure(points.front(), D2D1_FIGURE_BEGIN_HOLLOW);
            add_curve(sink.get());
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            ComPtr<ID2D1SolidColorBrush> brush;
            if (SUCCEEDED(sink->Close()) && SUCCEEDED(dc->CreateSolidColorBrush(line_color, &brush))) {
                auto style = RoundStroke(factory.get());
                dc->DrawGeometry(line.get(), brush.get(), 1.5f * scale, style.get());
            }
        }
    }
}

// Clips to a rounded rectangle until PopLayer (the graph track's corners).
bool PushRoundedClip(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, float radius) {
    auto factory = FactoryOf(dc);
    if (!factory.get()) return false;
    ComPtr<ID2D1RoundedRectangleGeometry> geometry;
    if (FAILED(factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(rect, radius, radius), &geometry)))
        return false;
    dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), geometry.get()), nullptr);
    return true;
}

enum class StatusIcon { None, Ok, Error, Warning, Shield };

// The HTML's 16-DIP status icons at (x, y) DIPs: a filled circle (triangle
// for a warning) with a white mark; dark on the light dark-theme colours.
void DrawStatusIcon(ID2D1DeviceContext* dc, StatusIcon kind, float x, float y, float scale,
                    D2D1_COLOR_F color, bool dark_mark) {
    if (!dc || kind == StatusIcon::None) return;
    auto factory = FactoryOf(dc);
    if (!factory.get()) return;
    auto at = [&](float px, float py) { return D2D1::Point2F((x + px) * scale, (y + py) * scale); };
    auto style = RoundStroke(factory.get());
    ComPtr<ID2D1SolidColorBrush> fill;
    ComPtr<ID2D1SolidColorBrush> mark;
    if (FAILED(dc->CreateSolidColorBrush(color, &fill)) ||
        FAILED(dc->CreateSolidColorBrush(dark_mark ? HexColor(0x000000, 0.86f) : HexColor(0xFFFFFF), &mark)))
        return;
    auto path = [&](std::initializer_list<D2D1_POINT_2F> pts, bool closed) {
        ComPtr<ID2D1PathGeometry> geometry;
        if (FAILED(factory->CreatePathGeometry(&geometry))) return geometry;
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(geometry->Open(&sink))) return ComPtr<ID2D1PathGeometry>();
        auto it = pts.begin();
        sink->BeginFigure(*it, closed ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
        for (++it; it != pts.end(); ++it) sink->AddLine(*it);
        sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
        if (FAILED(sink->Close())) return ComPtr<ID2D1PathGeometry>();
        return geometry;
    };
    const float stroke = 1.4f * scale;
    if (kind == StatusIcon::Shield) {
        // Two-tone shield (the UAC mark), built from cubic halves.
        ComPtr<ID2D1SolidColorBrush> blue;
        ComPtr<ID2D1SolidColorBrush> gold;
        if (FAILED(dc->CreateSolidColorBrush(HexColor(0x2F6FD6), &blue)) ||
            FAILED(dc->CreateSolidColorBrush(HexColor(0xF2B90F), &gold)))
            return;
        for (int half = 0; half < 2; ++half) {
            ComPtr<ID2D1PathGeometry> geometry;
            if (FAILED(factory->CreatePathGeometry(&geometry))) return;
            ComPtr<ID2D1GeometrySink> sink;
            if (FAILED(geometry->Open(&sink))) return;
            sink->BeginFigure(at(8.0f, 1.5f), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(at(13.5f, 3.5f));
            sink->AddLine(at(13.5f, 7.7f));
            sink->AddBezier(D2D1::BezierSegment(at(13.5f, 11.0f), at(11.2f, 13.6f), at(8.0f, 14.5f)));
            if (half == 0) {
                sink->AddBezier(D2D1::BezierSegment(at(4.8f, 13.6f), at(2.5f, 11.0f), at(2.5f, 7.7f)));
                sink->AddLine(at(2.5f, 3.5f));
            }
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            if (FAILED(sink->Close())) return;
            dc->FillGeometry(geometry.get(), half == 0 ? blue.get() : gold.get());
        }
        return;
    }
    if (kind == StatusIcon::Warning) {
        auto triangle = path({ at(8.0f, 1.9f), at(14.6f, 14.1f), at(1.4f, 14.1f) }, true);
        if (triangle.get()) {
            dc->FillGeometry(triangle.get(), fill.get());
            dc->DrawGeometry(triangle.get(), fill.get(), 1.2f * scale, style.get());
        }
        dc->DrawLine(at(8.0f, 5.9f), at(8.0f, 9.4f), mark.get(), stroke, style.get());
        dc->FillEllipse(D2D1::Ellipse(at(8.0f, 11.9f), 0.9f * scale, 0.9f * scale), mark.get());
        return;
    }
    dc->FillEllipse(D2D1::Ellipse(at(8.0f, 8.0f), 7.5f * scale, 7.5f * scale), fill.get());
    if (kind == StatusIcon::Ok) {
        auto check = path({ at(4.8f, 8.2f), at(6.9f, 10.3f), at(11.2f, 5.9f) }, false);
        if (check.get()) dc->DrawGeometry(check.get(), mark.get(), stroke, style.get());
    } else {
        dc->DrawLine(at(5.6f, 5.6f), at(10.4f, 10.4f), mark.get(), stroke, style.get());
        dc->DrawLine(at(10.4f, 5.6f), at(5.6f, 10.4f), mark.get(), stroke, style.get());
    }
}

D2D1_RECT_F Rect(float scale, float x, float y, float width, float height) {
    return pulse::ui::DipRect(scale, x, y, width, height);
}

std::wstring LeafName(const std::wstring& path) {
    std::wstring_view view = path;
    while (view.size() > 1 && (view.back() == L'\\' || view.back() == L'/'))
        view.remove_suffix(1);
    const size_t slash = view.find_last_of(L"\\/");
    return slash == std::wstring_view::npos ? std::wstring(view)
                                             : std::wstring(view.substr(slash + 1));
}

std::wstring ParentName(const std::wstring& path) {
    std::wstring copy = path;
    while (copy.size() > 1 && (copy.back() == L'\\' || copy.back() == L'/')) copy.pop_back();
    const size_t slash = copy.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return copy;
    return LeafName(copy.substr(0, slash));
}

// Speed comes in one of two units: bytes for copy/move/empty-recycle, items for
// delete/restore. Formatting in one place keeps the current readout and the peak
// label in step with whichever unit the task actually tracks.
std::wstring FormatSpeedText(ops::OpSpeedBasis basis, double value) {
    if (basis != ops::OpSpeedBasis::Items)
        return pulse::format::ByteSize(static_cast<uint64_t>(value)) + L"/s";
    // Item rates are small: deleting a folder tree of many files reports one item per
    // several seconds, so rounding to whole items would print a flat "0 items/s" while
    // the graph visibly moves. Keep decimals below 10 and trim the trailing zeros.
    if (value >= 10.0)
        return pulse::format::GroupedInt(static_cast<uint64_t>(value + 0.5)) + L" " +
            l10n::Get(l10n::StringId::OpItemsPerSecond);
    wchar_t text[32]{};
    if (value >= 1.0) swprintf_s(text, L"%.1f", value);
    else if (value >= 0.01) swprintf_s(text, L"%.2f", value);
    else wcscpy_s(text, L"0");
    std::wstring number = text;
    if (number.find(L'.') != std::wstring::npos) {
        while (!number.empty() && number.back() == L'0') number.pop_back();
        if (!number.empty() && number.back() == L'.') number.pop_back();
    }
    return number + L" " + l10n::Get(l10n::StringId::OpItemsPerSecond);
}

std::wstring FormatDuration(uint64_t seconds) {
    if (seconds == 0) return l10n::Get(l10n::StringId::OpEstimating).c_str();
    if (seconds < 60) return std::to_wstring(seconds) + l10n::Get(l10n::StringId::OpSeconds).c_str();
    const uint64_t minutes = seconds / 60;
    if (minutes < 60) return std::to_wstring(minutes) + l10n::Get(l10n::StringId::OpMinutes).c_str()
        + std::to_wstring(seconds % 60) + l10n::Get(l10n::StringId::OpSeconds).c_str();
    return std::to_wstring(minutes / 60) + l10n::Get(l10n::StringId::OpHours).c_str()
        + std::to_wstring(minutes % 60) + l10n::Get(l10n::StringId::OpMinutesEnd).c_str();
}

// 已用时间 counts from zero, so 0 s is a real value rather than "estimating".
std::wstring FormatElapsed(uint64_t seconds) {
    if (seconds < 60) return std::to_wstring(seconds) + l10n::Get(l10n::StringId::OpSeconds).c_str();
    return FormatDuration(seconds);
}

// File conflict, Pulse_文件操作对话框_重设计.html §2: what arrives and what
// is there side by side (the shell's icon, size, time, 更大 / 较新 in green),
// then the three choices as list rows. Hover or keyboard focus lights a row
// with the WinUI selection bar; each row says what it will do (保留两者 names
// the copy). R / S / K; 对其余 N 个冲突执行相同操作 sits in the footer.

// The shell's icon for a name of that type (no disk access), at least
// `pixels` tall where the shell has one; null when it has none.
HICON ShellTypeIcon(const std::wstring& name, bool directory, float pixels) {
    SHFILEINFOW info{};
    const DWORD attributes = directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    if (!SHGetFileInfoW(name.c_str(), attributes, &info, sizeof(info),
                        SHGFI_SYSICONINDEX | SHGFI_USEFILEATTRIBUTES))
        return nullptr;
    IImageList* list = nullptr;
    const int size = pixels <= 16.5f ? SHIL_SMALL : pixels <= 32.5f ? SHIL_LARGE : SHIL_EXTRALARGE;
    if (FAILED(SHGetImageList(size, __uuidof(IImageList), reinterpret_cast<void**>(&list))) || !list)
        return nullptr;
    HICON icon = nullptr;
    if (FAILED(list->GetIcon(info.iIcon, ILD_TRANSPARENT, &icon))) icon = nullptr;
    list->Release();
    return icon;
}

// An icon as a bitmap on `dc`; `wic` is created on first use.
ComPtr<ID2D1Bitmap> BitmapFromIcon(ID2D1DeviceContext* dc, ComPtr<IWICImagingFactory>& wic, HICON icon) {
    ComPtr<ID2D1Bitmap> bitmap;
    if (!dc || !icon) return bitmap;
    if (!wic.get() && FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                              IID_PPV_ARGS(&wic))))
        return bitmap;
    ComPtr<IWICBitmap> source;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wic->CreateBitmapFromHICON(icon, &source)) || FAILED(wic->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(source.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                     nullptr, 0.0, WICBitmapPaletteTypeMedianCut)) ||
        FAILED(dc->CreateBitmapFromWicBitmap(converter.get(), nullptr, &bitmap)))
        bitmap.reset();
    return bitmap;
}

// 32-DIP blank page (the HTML's file icon) for when the shell has no icon.
void DrawPageIcon(ID2D1DeviceContext* dc, float x, float y, float s) {
    auto factory = FactoryOf(dc);
    if (!factory.get()) return;
    auto P = [&](float px, float py) { return D2D1::Point2F(x + px * s, y + py * s); };
    ComPtr<ID2D1PathGeometry> page;
    ComPtr<ID2D1PathGeometry> fold;
    if (FAILED(factory->CreatePathGeometry(&page)) || FAILED(factory->CreatePathGeometry(&fold))) return;
    ComPtr<ID2D1GeometrySink> sink;
    if (SUCCEEDED(page->Open(&sink))) {
        sink->BeginFigure(P(7, 3), D2D1_FIGURE_BEGIN_FILLED);
        const D2D1_POINT_2F points[] = { P(19, 3), P(26, 10), P(26, 29), P(6, 29), P(6, 3) };
        sink->AddLines(points, ARRAYSIZE(points));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
    }
    sink.reset();
    if (SUCCEEDED(fold->Open(&sink))) {
        sink->BeginFigure(P(19, 3), D2D1_FIGURE_BEGIN_FILLED);
        const D2D1_POINT_2F points[] = { P(19, 10), P(26, 10) };
        sink->AddLines(points, ARRAYSIZE(points));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
    }
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(HexColor(0xFFFFFF), &brush))) return;
    dc->FillGeometry(page.get(), brush.get());
    brush->SetColor(HexColor(0xE9ECF0));
    dc->FillGeometry(fold.get(), brush.get());
    brush->SetColor(HexColor(0x9AA1AB));
    dc->DrawGeometry(page.get(), brush.get(), s);
    dc->DrawGeometry(fold.get(), brush.get(), s);
}

// The choice glyphs (20 DIP, 1.3 stroke): 0 replace, 1 skip, 2 keep both.
void DrawChoiceGlyph(ID2D1DeviceContext* dc, int index, float x, float y, float s, D2D1_COLOR_F color) {
    auto factory = FactoryOf(dc);
    if (!factory.get()) return;
    auto P = [&](float px, float py) { return D2D1::Point2F(x + px * s, y + py * s); };
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(factory->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) return;
    auto polyline = [&](std::initializer_list<D2D1_POINT_2F> points) {
        auto it = points.begin();
        sink->BeginFigure(*it, D2D1_FIGURE_BEGIN_HOLLOW);
        for (++it; it != points.end(); ++it) sink->AddLine(*it);
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
    };
    auto arc = [&](D2D1_POINT_2F to) {
        sink->AddArc(D2D1::ArcSegment(to, D2D1::SizeF(1.5f * s, 1.5f * s), 0.0f,
                                      D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    };
    if (index == 0) {
        polyline({ P(4, 7.5f), P(15, 7.5f), P(12, 4.5f) });
        polyline({ P(16, 12.5f), P(5, 12.5f), P(8, 15.5f) });
    } else if (index == 1) {
        polyline({ P(4.5f, 5.5f), P(9, 10), P(4.5f, 14.5f) });
        polyline({ P(10.5f, 5.5f), P(15, 10), P(10.5f, 14.5f) });
    } else {
        // The sheet behind, where the front one does not cover it.
        sink->BeginFigure(P(7.5f, 6.5f), D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddLine(P(7.5f, 5));
        arc(P(9, 3.5f));
        sink->AddLine(P(15, 3.5f));
        arc(P(16.5f, 5));
        sink->AddLine(P(16.5f, 12));
        arc(P(15, 13.5f));
        sink->AddLine(P(12.5f, 13.5f));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
    }
    if (FAILED(sink->Close())) return;
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(color, &brush))) return;
    auto style = RoundStroke(factory.get());
    dc->DrawGeometry(path.get(), brush.get(), 1.3f * s, style.get());
    if (index == 2)
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x + 3.5f * s, y + 6.5f * s, x + 12.5f * s, y + 16.5f * s),
                                                   1.5f * s, 1.5f * s),
                                 brush.get(), 1.3f * s, style.get());
}

class ConflictWindow {
public:
    ConflictDialogResult Show(HWND owner, const ops::ConflictItemInfo& conflict,
                              bool dark, D2D1_COLOR_F accent) {
        owner_ = owner;
        conflict_ = conflict;
        conflict_.source = path::FriendlyPathText(conflict_.source);
        conflict_.destination = path::FriendlyPathText(conflict_.destination);
        source_dir_ = ParentPath(conflict_.source);
        destination_dir_ = ParentPath(conflict_.destination);
        dark_ = dark;
        accent_ = accent;
        scale_ = static_cast<float>(pulse::compat::WindowDpi(owner)) / 96.0f;
        icons_[0] = ShellTypeIcon(LeafName(conflict_.source), conflict_.source_is_directory, 32.0f * scale_);
        icons_[1] = ShellTypeIcon(LeafName(conflict_.destination), conflict_.destination_is_directory,
                                  32.0f * scale_);

        WNDCLASSEXW wc{ sizeof(wc) };
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = WndProc;
        wc.lpszClassName = kConflictClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        if (!GetClassInfoExW(wc.hInstance, kConflictClass, &wc)) RegisterClassExW(&wc);

        const int width = static_cast<int>(kConflictW * scale_);
        const int height = static_cast<int>(HeightDip() * scale_);
        hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, kConflictClass, l10n::Get(l10n::StringId::OpConflictTitle).c_str(),
            WS_POPUP | WS_THICKFRAME | WS_SYSMENU,
            CW_USEDEFAULT, CW_USEDEFAULT, width, height, owner, nullptr,
            wc.hInstance, this);
        if (!hwnd_) { ReleaseIcons(); return result_; }
        pulse::ui::CenterOwnedWindow(hwnd_, owner_, width, height);
        EnableWindow(owner_, FALSE);
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
        // Without keyboard focus Windows sends letters as WM_SYSKEYDOWN, so
        // R / S / K, Tab and Enter would only work with Alt held.
        SetFocus(hwnd_);

        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (IsWindow(hwnd_)) { HideComposedDialog(hwnd_, owner_); DestroyWindow(hwnd_); }
        hwnd_ = nullptr;
        EnableWindow(owner_, TRUE);
        ReleaseIcons();
        return result_;
    }

private:
    // Layout in DIPs (the HTML's modal: 24 padding, 18/24 headline at 18,
    // compare block 82-188, rows 204 / 260 / 316, 64 footer).
    static constexpr float kConflictW = 500.0f;
    static constexpr float kPad = 24.0f;
    static constexpr float kCompareTop = 82.0f;   // labels; items +28, paths +90
    static constexpr float kCompareH = 106.0f;
    static constexpr float kColW = 209.5f;        // two columns, 16 gaps around a 1 px rule
    static constexpr float kActionsTop = 204.0f;
    static constexpr float kActionH = 54.0f;
    static constexpr float kActionGap = 2.0f;
    static constexpr float kFooterH = 64.0f;
    static constexpr float kHeightDip = 454.0f;
    static constexpr UINT_PTR kAnimTimer = 2;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<ConflictWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<ConflictWindow*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }
        return self ? self->Handle(message, wparam, lparam)
                    : DefWindowProcW(hwnd, message, wparam, lparam);
    }

    static std::wstring ParentPath(const std::wstring& path) {
        std::wstring copy = path;
        while (copy.size() > 3 && (copy.back() == L'\\' || copy.back() == L'/')) copy.pop_back();
        const size_t slash = copy.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return copy;
        return copy.substr(0, slash == 2 && copy[1] == L':' ? 3 : slash);
    }

    bool ShowsApplyAll() const { return conflict_.remaining > 1; }
    float HeightDip() const { return kHeightDip; }
    size_t Ordinal() const { return (std::max)(conflict_.ordinal, static_cast<size_t>(1)); }
    size_t Total() const {
        return Ordinal() - 1 + (std::max)(conflict_.remaining, static_cast<size_t>(1));
    }
    // 较新 only when the shown times differ: within a minute both read the same.
    static bool NewerThan(const FILETIME& a, const FILETIME& b) {
        const ULONGLONG ta = (static_cast<ULONGLONG>(a.dwHighDateTime) << 32) | a.dwLowDateTime;
        const ULONGLONG tb = (static_cast<ULONGLONG>(b.dwHighDateTime) << 32) | b.dwLowDateTime;
        return ta != 0 && tb != 0 && ta > tb + 60ull * 10000000ull;
    }
    bool SourceNewer() const { return NewerThan(conflict_.source_modified, conflict_.destination_modified); }
    bool DestinationNewer() const { return NewerThan(conflict_.destination_modified, conflict_.source_modified); }
    bool Files() const { return !conflict_.source_is_directory && !conflict_.destination_is_directory; }

    D2D1_RECT_F CloseRect() const { return Rect(scale_, kConflictW - 44, 4, 40, 32); }
    static float ColumnX(int side) { return side == 0 ? kPad : kConflictW - kPad - kColW; }
    D2D1_RECT_F PathRect(int side) const { return Rect(scale_, ColumnX(side), kCompareTop + 90, kColW, 16); }
    D2D1_RECT_F ActionRect(int index) const {
        return Rect(scale_, kPad - 8, kActionsTop + index * (kActionH + kActionGap), kConflictW - (kPad - 8) * 2,
                    kActionH);
    }
    static float FooterTop() { return kHeightDip - kFooterH; }
    D2D1_RECT_F ApplyRect() const { return Rect(scale_, kPad, FooterTop() + 16, 300, 32); }
    D2D1_RECT_F CancelRect() const {
        const float w = (std::max)(painter_.MeasureButtonWidth(l10n::Get(l10n::StringId::Cancel)),
                                   ScaleDip(scale_, 80.0f));
        const float h = painter_.MeasureButtonHeight();
        const float right = (kConflictW - kPad) * scale_;
        const float top = (FooterTop() + kFooterH * 0.5f) * scale_ - h * 0.5f;
        return D2D1::RectF(right - w, top, right, top + h);
    }

    int Hit(float x, float y) const {
        if (pulse::ui::ContainsRect(CloseRect(), x, y)) return 7;
        for (int i = 0; i < 3; ++i) if (pulse::ui::ContainsRect(ActionRect(i), x, y)) return i + 1;
        if (ShowsApplyAll() && pulse::ui::ContainsRect(ApplyRect(), x, y)) return 4;
        if (pulse::ui::ContainsRect(CancelRect(), x, y)) return 6;
        return 0;
    }

    void Complete(ops::ConflictChoice choice) {
        result_.choice = choice;
        result_.apply_to_all = apply_all_;
        done_ = true;
        if (hwnd_) { HideComposedDialog(hwnd_, owner_); DestroyWindow(hwnd_); }
    }

    // Preview of the hovered (or focused) choice: 1 replace, 2 skip, 3 keep.
    int Preview() const {
        if (hover_ >= 1 && hover_ <= 3) return hover_;
        if (keyboard_focus_ && focus_ <= 2) return focus_ + 1;
        return 0;
    }

    void Animate() {
        const ULONGLONG now = GetTickCount64();
        const float dt = anim_tick_ == 0 ? 16.0f : static_cast<float>((std::min)(now - anim_tick_, 100ull));
        anim_tick_ = now;
        const float k = 1.0f - std::exp(-dt / 70.0f);
        const int preview = Preview();
        bool moving = false;
        for (int i = 1; i <= 3; ++i) {
            const float target = preview == i ? 1.0f : 0.0f;
            weight_[i] += (target - weight_[i]) * k;
            if (std::fabs(target - weight_[i]) < 0.01f) weight_[i] = target;
            else moving = true;
        }
        if (!moving) { KillTimer(hwnd_, kAnimTimer); anim_tick_ = 0; }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void Kick() {
        anim_tick_ = 0;
        SetTimer(hwnd_, kAnimTimer, 15, nullptr);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void CreateFormats() {
        auto* factory = compositor_.DwriteFactory();
        auto make = [&](ComPtr<IDWriteTextFormat>& out, typography::FontRole role, float size,
                        DWRITE_FONT_WEIGHT weight) {
            out.reset();
            if (!factory) return;
            typography::TextFormatSpec spec;
            spec.role = role;
            spec.size = size * scale_;
            spec.weight = weight;
            if (FAILED(typography::CreateTextFormat(factory, spec, &out)) || !out.get()) return;
            out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        };
        make(head_format_, typography::FontRole::Display, 18.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        make(sub_format_, typography::FontRole::Text, 13.0f, DWRITE_FONT_WEIGHT_NORMAL);
        make(name_format_, typography::FontRole::Text, 14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        make(body_format_, typography::FontRole::Text, 14.0f, DWRITE_FONT_WEIGHT_NORMAL);
        make(meta_format_, typography::FontRole::Text, 12.0f, DWRITE_FONT_WEIGHT_NORMAL);
    }

    void ReleaseIcons() {
        for (HICON& icon : icons_) {
            if (icon) DestroyIcon(icon);
            icon = nullptr;
        }
    }

    ID2D1Bitmap* IconBitmap(int side) {
        if (!icon_bitmaps_[side].get()) icon_bitmaps_[side] = BitmapFromIcon(compositor_.Dc(), wic_, icons_[side]);
        return icon_bitmaps_[side].get();
    }

    // One side of the comparison: label, icon, name, size and time (更大 /
    // 较新 in green after a dot), and the folder it is in.
    void DrawCompareColumn(const Theme& theme, int side, D2D1_COLOR_F text3, D2D1_COLOR_F ok) {
        auto* dc = compositor_.Dc();
        const bool source = side == 0;
        const float s = scale_;
        const float x = ColumnX(side);
        const float y = kCompareTop;
        const bool moving = conflict_.type == ops::OpType::Move;
        const l10n::StringId label = !source ? l10n::StringId::OpConflictAlreadyThere
            : moving ? l10n::StringId::OpConflictIncomingMove : l10n::StringId::OpConflictIncomingCopy;
        DrawEllipsizedText(compositor_, meta_format_.get(), Rect(s, x, y, kColW, 20), l10n::Get(label),
                           theme.text_secondary);

        const auto icon = Rect(s, x, y + 28, 32, 32);
        if (ID2D1Bitmap* bitmap = IconBitmap(side))
            dc->DrawBitmap(bitmap, icon, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        else
            DrawPageIcon(dc, icon.left, icon.top, s);

        const std::wstring name = LeafName(source ? conflict_.source : conflict_.destination);
        const bool directory = source ? conflict_.source_is_directory : conflict_.destination_is_directory;
        const float tx = x + 44.0f;
        const float tw = kColW - 44.0f;
        DrawEllipsizedText(compositor_, name_format_.get(), Rect(s, tx, y + 28, tw, 20), name, theme.text);
        auto meta_line = [&](float row_y, const std::wstring& value, const std::wstring& badge) {
            if (badge.empty()) {
                DrawEllipsizedText(compositor_, meta_format_.get(), Rect(s, tx, row_y, tw, 18), value,
                                   theme.text_secondary);
                return;
            }
            const std::wstring dot = L" \x00B7 ";
            auto* factory = compositor_.DwriteFactory();
            const float dot_w = TextWidth(factory, meta_format_.get(), dot) / s;
            const float badge_w = TextWidth(factory, meta_format_.get(), badge) / s;
            const float value_w = (std::min)(TextWidth(factory, meta_format_.get(), value) / s,
                                             (std::max)(tw - dot_w - badge_w, 0.0f));
            DrawEllipsizedText(compositor_, meta_format_.get(), Rect(s, tx, row_y, value_w + 1, 18), value,
                               theme.text_secondary);
            painter_.DrawText(dot, Rect(s, tx + value_w, row_y, dot_w + 1, 18), meta_format_.get(),
                              theme.text_secondary);
            painter_.DrawText(badge, Rect(s, tx + value_w + dot_w, row_y, badge_w + 2, 18), meta_format_.get(), ok);
        };
        const uint64_t size = source ? conflict_.source_size : conflict_.destination_size;
        const uint64_t other = source ? conflict_.destination_size : conflict_.source_size;
        const bool larger = Files() && size > other;
        const bool newer = source ? SourceNewer() : DestinationNewer();
        meta_line(y + 48, directory ? l10n::Get(l10n::StringId::OpFolderKind) : pulse::format::ByteSize(size),
                  larger ? l10n::Get(l10n::StringId::OpLarger) : std::wstring());
        meta_line(y + 66, pulse::format::LocalFileTime(source ? conflict_.source_modified : conflict_.destination_modified,
                                                       l10n::Get(l10n::StringId::OpUnknown).c_str()),
                  newer ? l10n::Get(l10n::StringId::OpNewer) : std::wstring());
        // Cut to the column; hovering shows it whole (#55).
        DrawEllipsizedText(compositor_, meta_format_.get(), PathRect(side), source ? source_dir_ : destination_dir_,
                           text3);
    }

    // A choice row: glyph, title over what it will do, the shortcut at the
    // right. Hover / focus fades in the light fill and the 3-DIP accent bar.
    void DrawChoice(const Theme& theme, int index, const std::wstring& title, const std::wstring& detail,
                    const wchar_t* key, D2D1_COLOR_F accent, D2D1_COLOR_F text3, bool high_contrast) {
        const float s = scale_;
        const auto row = ActionRect(index);
        const float lit = weight_[index + 1];
        const bool pressed = pressed_ == index + 1 && hover_ == index + 1;
        const bool focused = keyboard_focus_ && focus_ == index;
        const D2D1_COLOR_F hover_fill = high_contrast ? WithAlpha(theme.text, 0.12f)
            : dark_ ? HexColor(0xFFFFFF, 0.0605f) : HexColor(0x000000, 0.0373f);
        const D2D1_COLOR_F press_fill = high_contrast ? WithAlpha(theme.text, 0.08f)
            : dark_ ? HexColor(0xFFFFFF, 0.0419f) : HexColor(0x000000, 0.0241f);
        const float radius = ScaleDip(s, 4.0f);
        if (pressed) painter_.FillRoundedRect(row, radius, press_fill);
        else if (lit > 0.01f) painter_.FillRoundedRect(row, radius, WithAlpha(hover_fill, hover_fill.a * lit));
        if (lit > 0.01f) {
            const float cy = (row.top + row.bottom) * 0.5f;
            const auto bar = D2D1::RectF(row.left + ScaleDip(s, 2.0f), cy - ScaleDip(s, 8.0f),
                                         row.left + ScaleDip(s, 5.0f), cy + ScaleDip(s, 8.0f));
            painter_.FillRoundedRect(bar, ScaleDip(s, 1.5f), WithAlpha(accent, lit));
        }
        if (focused) painter_.StrokeRoundedRect(row, radius, theme.text, 2.0f * s);

        const float y = kActionsTop + index * (kActionH + kActionGap);
        const float left = kPad - 8.0f;
        const float right = kConflictW - left;
        DrawChoiceGlyph(compositor_.Dc(), index, (left + 14.0f) * s, (y + 17.0f) * s, s, theme.text_secondary);
        DrawEllipsizedText(compositor_, body_format_.get(), Rect(s, left + 48, y + 9, right - 40 - left - 48, 20),
                           title, theme.text);
        DrawEllipsizedText(compositor_, meta_format_.get(), Rect(s, left + 48, y + 29, right - 40 - left - 48, 16),
                           detail, theme.text_secondary);
        painter_.DrawText(key, Rect(s, right - 12 - 16, y + 17, 16, 20), meta_format_.get(), text3,
                          fluent::HorizontalAlignment::Center);
    }

    void Render() {
        if (compositor_.NeedsRecovery()) {
            if (!compositor_.Recover()) return;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
            CreateFormats();
            for (auto& bitmap : icon_bitmaps_) bitmap.reset();
        }
        if (!compositor_.Dc()) return;
        if (!head_format_.get()) CreateFormats();
        const float s = scale_;
        const bool high_contrast = IsHighContrast();
        const Theme theme = high_contrast ? MakeHighContrastTheme() : MakeTheme(dark_, accent_);
        pulse::ui::BeginSurface(compositor_, painter_, theme, dark_, high_contrast, backdrop_enabled_, s);
        const D2D1_COLOR_F accent = high_contrast || !dark_ ? theme.accent
            : D2D1::ColorF(theme.accent.r + (1.0f - theme.accent.r) * 0.28f,
                           theme.accent.g + (1.0f - theme.accent.g) * 0.28f,
                           theme.accent.b + (1.0f - theme.accent.b) * 0.28f, 1.0f);
        const D2D1_COLOR_F text3 = WithAlpha(theme.text, dark_ ? 0.544f : 0.446f);
        const D2D1_COLOR_F ok = high_contrast ? theme.accent : dark_ ? HexColor(0x6CCB5F) : HexColor(0x0F7B0F);
        const bool moving = conflict_.type == ops::OpType::Move;

        fluent::ControlState close_state{};
        close_state.hovered = hover_ == 7;
        close_state.pressed = pressed_ == 7;
        painter_.DrawTitleBarButton(CloseRect(), fluent::TitleBarButtonRole::Close, {}, close_state);

        // Headline, then where it happened (and n / m when there are more).
        DrawEllipsizedText(compositor_, head_format_.get(), Rect(s, kPad, 18, kConflictW - kPad - 64, 24),
                           l10n::Get(l10n::StringId::OpConflictLead) + LeafName(conflict_.destination) +
                               l10n::Get(l10n::StringId::OpQuoteClose), theme.text);
        std::wstring where = l10n::Get(moving ? l10n::StringId::OpConflictMoveFrom
                                              : l10n::StringId::OpConflictCopyFrom) +
            LeafName(source_dir_) +
            l10n::Get(moving ? l10n::StringId::OpConflictMoveTo : l10n::StringId::OpConflictCopyTo) +
            LeafName(destination_dir_);
        if (Total() > 1)
            where += L"  \x00B7  " + l10n::Get(l10n::StringId::OpConflictCountLead) +
                pulse::format::GroupedInt(Ordinal()) + l10n::Get(l10n::StringId::OpConflictCountSep) +
                pulse::format::GroupedInt(Total()) + l10n::Get(l10n::StringId::OpConflictCountEnd);
        DrawEllipsizedText(compositor_, sub_format_.get(), Rect(s, kPad, 46, kConflictW - kPad * 2, 20), where,
                           theme.text_secondary);

        DrawCompareColumn(theme, 0, text3, ok);
        DrawCompareColumn(theme, 1, text3, ok);
        painter_.FillRoundedRect(Rect(s, kConflictW * 0.5f - 0.5f, kCompareTop, 1, kCompareH), 0,
                                 theme.stroke_divider);

        const bool files = Files();
        std::wstring replace_sub = l10n::Get(l10n::StringId::OpReplaceDesc);
        if (files)
            replace_sub = l10n::Get(l10n::StringId::OpReplaceUseLead) + pulse::format::ByteSize(conflict_.source_size) +
                l10n::Get(l10n::StringId::OpReplaceUseMid) + pulse::format::ByteSize(conflict_.destination_size) +
                l10n::Get(l10n::StringId::OpReplaceUseEnd);
        const std::wstring keep_sub = conflict_.keep_both_name.empty()
            ? l10n::Get(l10n::StringId::OpKeepBothDesc)
            : l10n::Get(l10n::StringId::OpKeepNameLead) + conflict_.keep_both_name + l10n::Get(l10n::StringId::OpQuoteClose);
        DrawChoice(theme, 0, l10n::Get(l10n::StringId::OpReplaceShort), replace_sub, L"R", accent, text3,
                   high_contrast);
        DrawChoice(theme, 1, l10n::Get(l10n::StringId::OpSkipShort), l10n::Get(l10n::StringId::OpSkipDesc), L"S",
                   accent, text3, high_contrast);
        DrawChoice(theme, 2, l10n::Get(l10n::StringId::OpKeepBoth), keep_sub, L"K", accent, text3, high_contrast);

        // Footer: the same band as the progress window.
        RECT client{};
        GetClientRect(hwnd_, &client);
        const float width = static_cast<float>(client.right);
        const float height = static_cast<float>(client.bottom);
        const float footer_top = FooterTop() * s;
        painter_.FillRoundedRect(D2D1::RectF(0, footer_top, width, height), 0,
                                 OpFooter(hwnd_, backdrop_enabled_, dark_));
        painter_.FillRoundedRect(D2D1::RectF(0, footer_top, width, footer_top + 1.0f), 0, theme.stroke_divider);
        if (ShowsApplyAll()) {
            fluent::ControlState check{};
            check.checked = apply_all_;
            check.hovered = hover_ == 4;
            check.pressed = pressed_ == 4 && hover_ == 4;
            check.focused = keyboard_focus_ && focus_ == 3;
            check.keyboard_focus = check.focused;
            const std::wstring label = l10n::Get(l10n::StringId::OpApplyRestLead) +
                pulse::format::GroupedInt(conflict_.remaining - 1) + l10n::Get(l10n::StringId::OpApplyRestEnd);
            painter_.DrawCheckBox(ApplyRect(), label, check);
        }
        fluent::ControlState cancel_state{};
        cancel_state.hovered = hover_ == 6;
        cancel_state.pressed = pressed_ == 6;
        cancel_state.focused = keyboard_focus_ && focus_ == (ShowsApplyAll() ? 4 : 3);
        cancel_state.keyboard_focus = cancel_state.focused;
        painter_.DrawButton({ CancelRect(), l10n::Get(l10n::StringId::Cancel), {}, fluent::ButtonKind::Standard,
                              cancel_state });
        SetPathTip(1, PathRect(0));
        SetPathTip(2, PathRect(1));
        pulse::ui::EndSurface(compositor_);
    }

    void Choose(int index) {
        Complete(index == 0 ? ops::ConflictChoice::Replace
                 : index == 1 ? ops::ConflictChoice::Skip
                              : ops::ConflictChoice::KeepBoth);
    }

    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_CREATE:
            scale_ = static_cast<float>(pulse::compat::WindowDpi(hwnd_)) / 96.0f;
            backdrop_enabled_ = pulse::ui::ApplyBackdrop(hwnd_, dark_);
            if (!compositor_.Init(hwnd_)) return -1;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
            CreateFormats();
            CreatePathTips();
            // R / S / K are shortcuts here, not text: keep a Chinese IME from
            // swallowing them as composition (they would only work with Alt).
            ImmAssociateContextEx(hwnd_, nullptr, 0);
            return 0;
        case WM_NCCALCSIZE:
            return 0;
        case WM_NCHITTEST:
            return pulse::ui::BorderlessHitTest(hwnd_, lparam, 40 * scale_, CloseRect());
        case WM_SIZE:
            if (compositor_.Dc()) compositor_.Resize(LOWORD(lparam), HIWORD(lparam));
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_MOVE:
            compositor_.UpdateTextRenderingParams(
                MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST));
            return 0;
        case WM_DPICHANGED: {
            scale_ = HIWORD(wparam) / 96.0f;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetScale(scale_);
            CreateFormats();
            const auto* suggested = reinterpret_cast<RECT*>(lparam);
            SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_TIMER:
            if (wparam == kAnimTimer) { Animate(); return 0; }
            break;
        case WM_MOUSEMOVE: {
            const int next = Hit(static_cast<float>(GET_X_LPARAM(lparam)),
                                 static_cast<float>(GET_Y_LPARAM(lparam)));
            if (next != hover_) { hover_ = next; Kick(); }
            TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, hwnd_, 0 };
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            hover_ = 0;
            Kick();
            return 0;
        case WM_LBUTTONDOWN:
            pressed_ = Hit(static_cast<float>(GET_X_LPARAM(lparam)),
                           static_cast<float>(GET_Y_LPARAM(lparam)));
            keyboard_focus_ = false;
            SetCapture(hwnd_);
            Kick(); // a focus preview fades once the mouse takes over
            return 0;
        case WM_LBUTTONUP: {
            const int hit = Hit(static_cast<float>(GET_X_LPARAM(lparam)),
                                static_cast<float>(GET_Y_LPARAM(lparam)));
            const int pressed = pressed_;
            pressed_ = 0;
            ReleaseCapture();
            if (hit == pressed) {
                if (hit >= 1 && hit <= 3) { focus_ = hit - 1; Choose(hit - 1); return 0; }
                if (hit == 4) apply_all_ = !apply_all_;
                else if (hit == 6 || hit == 7) { Complete(ops::ConflictChoice::Cancel); return 0; }
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_KEYDOWN: {
            if (wparam == VK_PROCESSKEY) {
                const UINT vk = ImmGetVirtualKey(hwnd_);
                if (vk && vk != VK_PROCESSKEY) wparam = vk;
            }
            if (wparam == 'R') { Choose(0); return 0; }
            if (wparam == 'S') { Choose(1); return 0; }
            if (wparam == 'K') { Choose(2); return 0; }
            if (wparam == VK_ESCAPE) { Complete(ops::ConflictChoice::Cancel); return 0; }
            const int count = ShowsApplyAll() ? 5 : 4;
            if (wparam == VK_TAB || wparam == VK_DOWN || wparam == VK_UP) {
                const bool back = wparam == VK_UP || (wparam == VK_TAB && GetKeyState(VK_SHIFT) < 0);
                if (keyboard_focus_) focus_ = (focus_ + (back ? count - 1 : 1)) % count;
                keyboard_focus_ = true;
                Kick();
                return 0;
            }
            if (wparam == VK_RETURN || wparam == VK_SPACE) {
                if (focus_ <= 2) { Choose(focus_); return 0; }
                if (focus_ == 3 && ShowsApplyAll()) apply_all_ = !apply_all_;
                else { Complete(ops::ConflictChoice::Cancel); return 0; }
                keyboard_focus_ = true;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            break;
        }
        case WM_ACTIVATE:
            if (LOWORD(wparam) != WA_INACTIVE) SetFocus(hwnd_);
            return 0;
        case WM_SYSKEYDOWN:
            // No focus: plain keys arrive here with the Alt bit clear.
            if (!(lparam & (1 << 29)) && wparam != VK_F10) return Handle(WM_KEYDOWN, wparam, lparam);
            if (wparam == 'R') { Choose(0); return 0; }
            if (wparam == 'S') { Choose(1); return 0; }
            if (wparam == 'K') { Choose(2); return 0; }
            break;
        case WM_CLOSE:
            Complete(ops::ConflictChoice::Cancel);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(hwnd_, &paint);
            Render();
            EndPaint(hwnd_, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            KillTimer(hwnd_, kAnimTimer);
            compositor_.Shutdown();
            done_ = true;
            return 0;
        }
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }

    void CreatePathTips() {
        INITCOMMONCONTROLSEX controls{ sizeof(controls), ICC_BAR_CLASSES };
        InitCommonControlsEx(&controls);
        tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
            WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!tooltip_) return;
        SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, static_cast<LPARAM>(560 * scale_));
        for (UINT id : { 1u, 2u }) {
            TOOLINFOW info{ sizeof(info) };
            info.uFlags = TTF_SUBCLASS;
            info.hwnd = hwnd_;
            info.uId = id;
            info.lpszText = const_cast<wchar_t*>(
                (id == 1 ? conflict_.source : conflict_.destination).c_str());
            SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
        }
    }

    void SetPathTip(UINT id, const D2D1_RECT_F& box) {
        if (!tooltip_) return;
        TOOLINFOW info{ sizeof(info) };
        info.hwnd = hwnd_;
        info.uId = id;
        info.rect = { static_cast<LONG>(std::floor(box.left)), static_cast<LONG>(std::floor(box.top)),
                      static_cast<LONG>(std::ceil(box.right)), static_cast<LONG>(std::ceil(box.bottom)) };
        SendMessageW(tooltip_, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&info));
    }

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    HWND tooltip_ = nullptr;
    Compositor compositor_;
    fluent::Painter painter_{ &compositor_ };
    ops::ConflictItemInfo conflict_;
    std::wstring source_dir_;
    std::wstring destination_dir_;
    ConflictDialogResult result_;
    D2D1_COLOR_F accent_ = HexColor(0x0078D4);
    float scale_ = 1.0f;
    bool dark_ = false;
    bool backdrop_enabled_ = false;
    bool apply_all_ = false;
    bool done_ = false;
    bool keyboard_focus_ = false;
    int hover_ = 0;
    int pressed_ = 0;
    int focus_ = 0;
    float weight_[4] = {};
    ULONGLONG anim_tick_ = 0;
    ComPtr<IDWriteTextFormat> head_format_;
    ComPtr<IDWriteTextFormat> sub_format_;
    ComPtr<IDWriteTextFormat> name_format_;
    ComPtr<IDWriteTextFormat> body_format_;
    ComPtr<IDWriteTextFormat> meta_format_;
    HICON icons_[2] = {};
    ComPtr<ID2D1Bitmap> icon_bitmaps_[2];
    ComPtr<IWICImagingFactory> wic_;
};

// Permanent delete, Pulse_文件操作对话框_重设计.html §3: the question as the
// headline, why it can't be undone under it (a warning line with the reason
// where there is no Recycle Bin), the first three items in a bordered list
// with "and N more", then 改为移到回收站 · 取消 · 永久删除. Enter deletes; a
// focus ring shows only once the keyboard moves focus.
class PermanentDeleteWindow {
public:
    PermanentDeleteChoice Show(HWND owner, const PermanentDeleteInfo& info, bool dark, D2D1_COLOR_F accent) {
        owner_ = owner;
        info_ = info;
        dark_ = dark;
        accent_ = accent;
        scale_ = static_cast<float>(pulse::compat::WindowDpi(owner)) / 96.0f;
        Measure();
        for (size_t i = 0; i < items_.size(); ++i)
            icons_[i] = ShellTypeIcon(items_[i].name, items_[i].directory, 16.0f * scale_);

        WNDCLASSEXW wc{ sizeof(wc) };
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = WndProc;
        wc.lpszClassName = kDeleteClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!GetClassInfoExW(wc.hInstance, kDeleteClass, &wc)) RegisterClassExW(&wc);

        const int width = static_cast<int>(kW * scale_);
        const int height = static_cast<int>(HeightDip() * scale_);
        hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, kDeleteClass,
            l10n::Get(l10n::StringId::PermanentDelete).c_str(), WS_POPUP | WS_THICKFRAME | WS_SYSMENU,
            CW_USEDEFAULT, CW_USEDEFAULT, width, height, owner, nullptr, wc.hInstance, this);
        if (!hwnd_) { ReleaseIcons(); return choice_; }
        pulse::ui::CenterOwnedWindow(hwnd_, owner_, width, height);
        if (owner_) EnableWindow(owner_, FALSE);
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
        SetFocus(hwnd_);

        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (IsWindow(hwnd_)) { HideComposedDialog(hwnd_, owner_); DestroyWindow(hwnd_); }
        hwnd_ = nullptr;
        if (owner_) EnableWindow(owner_, TRUE);
        ReleaseIcons();
        return choice_;
    }

private:
    // Layout in DIPs: headline 18, reason / warning 46, list from 82 (34-DIP
    // rows inside a 1 px border), 20 below it, then the 64-DIP footer.
    static constexpr float kW = 440.0f;
    static constexpr float kPad = 24.0f;
    static constexpr float kListTop = 82.0f;
    static constexpr float kRowH = 34.0f;
    static constexpr float kFooterH = 64.0f;
    static constexpr size_t kMaxRows = 3;

    struct Item {
        std::wstring name;
        bool directory = false;
        bool sized = false;
        uint64_t bytes = 0;
    };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<PermanentDeleteWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<PermanentDeleteWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }
        return self ? self->Handle(message, wparam, lparam) : DefWindowProcW(hwnd, message, wparam, lparam);
    }

    // Names and sizes for the list; the counts only when the selection is
    // small enough to stat every item (a folder's size would need a walk).
    void Measure() {
        files_ = folders_ = 0;
        total_ = 0;
        const bool stat_all = info_.paths.size() <= 256;
        for (size_t i = 0; i < info_.paths.size(); ++i) {
            if (!stat_all && i >= kMaxRows) break;
            Item item;
            item.name = LeafName(info_.paths[i]);
            if (item.name.empty()) item.name = info_.paths[i];
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (GetFileAttributesExW(info_.paths[i].c_str(), GetFileExInfoStandard, &data)) {
                item.directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                if (!item.directory) {
                    item.bytes = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
                    item.sized = true;
                }
            }
            item.directory ? ++folders_ : ++files_;
            total_ += item.bytes;
            if (items_.size() < kMaxRows) items_.push_back(std::move(item));
        }
        totals_known_ = stat_all;
    }

    std::wstring Headline() const {
        if (info_.paths.size() == 1 && !items_.empty())
            return l10n::Get(l10n::StringId::PermDeleteOneLead) + items_.front().name +
                   l10n::Get(l10n::StringId::PermDeleteOneEnd);
        return l10n::Get(l10n::StringId::PermDeleteManyLead) + pulse::format::GroupedInt(info_.paths.size()) +
               l10n::Get(l10n::StringId::PermDeleteManyEnd);
    }

    // Why there is no Recycle Bin here; empty when there is one.
    std::wstring Reason() const {
        if (!info_.no_recycle_bin) return {};
        using Reason = PermanentDeleteInfo::NoBinReason;
        std::wstring text = l10n::Get(info_.no_bin_reason == Reason::Disabled   ? l10n::StringId::RecycleBinOffForDrive
                                      : info_.no_bin_reason == Reason::TooLarge ? l10n::StringId::RecycleBinTooSmallForItem
                                                                                : l10n::StringId::NetworkLocationsAndRemovableDrivesHaveNoRecycleBin);
        while (!text.empty() && text.back() == L' ') text.pop_back();
        if (info_.no_bin_reason != Reason::Disabled) text += l10n::Get(l10n::StringId::PermDeleteCantUndo);
        return text;
    }

    // What the whole selection holds: "共 3.1 MB", or folders · files.
    std::wstring Counts() const {
        if (!totals_known_) return {};
        if (folders_ == 0) return l10n::Get(l10n::StringId::PermDeleteTotalLead) + pulse::format::ByteSize(total_);
        std::wstring counts = pulse::format::GroupedInt(folders_) + l10n::Get(l10n::StringId::Folders);
        if (files_) counts += L" \x00B7 " + pulse::format::GroupedInt(files_) + l10n::Get(l10n::StringId::PermDeleteFiles);
        return counts;
    }

    size_t MoreCount() const { return info_.paths.size() > items_.size() ? info_.paths.size() - items_.size() : 0; }
    size_t RowCount() const { return items_.size() + (MoreCount() ? 1 : 0); }
    float FooterTop() const { return kListTop + 2.0f + static_cast<float>(RowCount()) * kRowH + 20.0f; }
    float HeightDip() const { return FooterTop() + kFooterH; }

    D2D1_RECT_F CloseRect() const { return Rect(scale_, kW - 44, 4, 40, 32); }
    // 0 改为移到回收站 (left, flush with the text after its padding), 1 cancel, 2 delete.
    D2D1_RECT_F ButtonRect(int which) const {
        const float h = painter_.MeasureButtonHeight();
        const float top = (FooterTop() + kFooterH * 0.5f) * scale_ - h * 0.5f;
        auto width = [&](l10n::StringId id) {
            return (std::max)(painter_.MeasureButtonWidth(l10n::Get(id)), ScaleDip(scale_, 80.0f));
        };
        if (which == 0) {
            const float left = (kPad - 12.0f) * scale_;
            return D2D1::RectF(left, top, left + width(l10n::StringId::PermDeleteSwitchToBin), top + h);
        }
        const float right = (kW - kPad) * scale_;
        const float delete_w = width(l10n::StringId::PermanentDelete);
        if (which == 2) return D2D1::RectF(right - delete_w, top, right, top + h);
        const float cancel_right = right - delete_w - ScaleDip(scale_, 8.0f);
        return D2D1::RectF(cancel_right - width(l10n::StringId::Cancel), top, cancel_right, top + h);
    }

    // Focus order: [recycle,] cancel, delete. Delete starts focused (Enter).
    int FocusCount() const { return info_.offer_recycle ? 3 : 2; }
    int FocusTarget(int focus) const { return info_.offer_recycle ? focus : focus + 1; } // 0 bin, 1 cancel, 2 delete

    int Hit(float x, float y) const {
        if (pulse::ui::ContainsRect(CloseRect(), x, y)) return 4;
        if (pulse::ui::ContainsRect(ButtonRect(2), x, y)) return 3;
        if (pulse::ui::ContainsRect(ButtonRect(1), x, y)) return 2;
        if (info_.offer_recycle && pulse::ui::ContainsRect(ButtonRect(0), x, y)) return 1;
        return 0;
    }

    void Complete(PermanentDeleteChoice choice) {
        choice_ = choice;
        done_ = true;
        if (hwnd_) { HideComposedDialog(hwnd_, owner_); DestroyWindow(hwnd_); }
    }

    void Activate(int target) {
        if (target == 0) Complete(PermanentDeleteChoice::Recycle);
        else if (target == 2) Complete(PermanentDeleteChoice::Delete);
        else Complete(PermanentDeleteChoice::Cancel);
    }

    void CreateFormats() {
        auto* factory = compositor_.DwriteFactory();
        auto make = [&](ComPtr<IDWriteTextFormat>& out, typography::FontRole role, float size, DWRITE_FONT_WEIGHT weight) {
            out.reset();
            if (!factory) return;
            typography::TextFormatSpec spec;
            spec.role = role;
            spec.size = size * scale_;
            spec.weight = weight;
            if (FAILED(typography::CreateTextFormat(factory, spec, &out)) || !out.get()) return;
            out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        };
        make(head_format_, typography::FontRole::Display, 18.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        make(sub_format_, typography::FontRole::Text, 13.0f, DWRITE_FONT_WEIGHT_NORMAL);
        make(meta_format_, typography::FontRole::Text, 12.0f, DWRITE_FONT_WEIGHT_NORMAL);
    }

    void ReleaseIcons() {
        for (HICON& icon : icons_) {
            if (icon) DestroyIcon(icon);
            icon = nullptr;
        }
    }

    ID2D1Bitmap* IconBitmap(size_t index) {
        if (!icon_bitmaps_[index].get()) icon_bitmaps_[index] = BitmapFromIcon(compositor_.Dc(), wic_, icons_[index]);
        return icon_bitmaps_[index].get();
    }

    void Render() {
        if (compositor_.NeedsRecovery()) {
            if (!compositor_.Recover()) return;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
            CreateFormats();
            for (auto& bitmap : icon_bitmaps_) bitmap.reset();
        }
        auto* dc = compositor_.Dc();
        if (!dc) return;
        if (!head_format_.get()) CreateFormats();
        const float s = scale_;
        const bool high_contrast = IsHighContrast();
        const Theme theme = high_contrast ? MakeHighContrastTheme() : MakeTheme(dark_, accent_);
        pulse::ui::BeginSurface(compositor_, painter_, theme, dark_, high_contrast, backdrop_enabled_, s);
        const D2D1_COLOR_F text3 = WithAlpha(theme.text, dark_ ? 0.544f : 0.446f);
        const D2D1_COLOR_F warn = high_contrast ? theme.accent : dark_ ? HexColor(0xFCE100) : HexColor(0x9D5D00);
        const D2D1_COLOR_F stroke = high_contrast ? theme.text
            : dark_ ? HexColor(0xFFFFFF, 0.0698f) : HexColor(0x000000, 0.0578f);

        fluent::ControlState close_state{};
        close_state.hovered = hover_ == 4;
        close_state.pressed = pressed_ == 4;
        painter_.DrawTitleBarButton(CloseRect(), fluent::TitleBarButtonRole::Close, {}, close_state);

        DrawEllipsizedText(compositor_, head_format_.get(), Rect(s, kPad, 18, kW - kPad - 64, 24), Headline(),
                           theme.text);
        const std::wstring reason = Reason();
        if (reason.empty()) {
            DrawEllipsizedText(compositor_, sub_format_.get(), Rect(s, kPad, 46, kW - kPad * 2, 20),
                               l10n::Get(l10n::StringId::PermDeleteWarn), theme.text_secondary);
        } else {
            DrawStatusIcon(dc, StatusIcon::Warning, kPad, 48, s, warn, dark_ && !high_contrast);
            DrawEllipsizedText(compositor_, sub_format_.get(), Rect(s, kPad + 24, 46, kW - kPad * 2 - 24, 20), reason,
                               theme.text_secondary);
        }

        // The list: a 1 px border, 34-DIP rows split by dividers.
        const size_t rows = RowCount();
        const float list_h = 2.0f + static_cast<float>(rows) * kRowH;
        const auto box = Rect(s, kPad, kListTop, kW - kPad * 2, list_h);
        const float half = 0.5f;
        painter_.StrokeRoundedRect(D2D1::RectF(box.left + half, box.top + half, box.right - half, box.bottom - half),
                                   ScaleDip(s, 6.0f), stroke, 1.0f);
        const float x0 = kPad + 1.0f + 12.0f;
        const float x1 = kW - kPad - 1.0f - 12.0f;
        for (size_t i = 0; i < rows; ++i) {
            const float y = kListTop + 1.0f + static_cast<float>(i) * kRowH;
            if (i > 0) {
                const float line = std::round(y * s);
                painter_.FillRoundedRect(D2D1::RectF(box.left + 1.0f, line, box.right - 1.0f, line + 1.0f), 0,
                                         theme.stroke_divider);
            }
            if (i >= items_.size()) {
                const std::wstring more = l10n::Get(l10n::StringId::PermDeleteMoreLead) +
                    pulse::format::GroupedInt(MoreCount()) + l10n::Get(l10n::StringId::PermDeleteMoreEnd);
                const std::wstring counts = Counts();
                const float more_w = TextWidth(compositor_.DwriteFactory(), meta_format_.get(), more) / s;
                painter_.DrawText(more, Rect(s, x0, y, more_w + 2, kRowH), meta_format_.get(), theme.text_secondary);
                if (!counts.empty())
                    DrawEllipsizedText(compositor_, meta_format_.get(),
                                       Rect(s, x0 + more_w, y, (std::max)(x1 - x0 - more_w, 0.0f), kRowH),
                                       L" \x00B7 " + counts, text3);
                continue;
            }
            const Item& item = items_[i];
            const auto icon = Rect(s, x0, y + 9, 16, 16);
            if (ID2D1Bitmap* bitmap = IconBitmap(i))
                dc->DrawBitmap(bitmap, icon, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            else
                DrawPageIcon(dc, icon.left, icon.top, s * 0.5f);
            const std::wstring size = item.directory ? l10n::Get(l10n::StringId::Folder)
                                    : item.sized ? pulse::format::ByteSize(item.bytes) : std::wstring();
            const float size_w = size.empty() ? 0.0f : TextWidth(compositor_.DwriteFactory(), meta_format_.get(), size) / s;
            DrawEllipsizedText(compositor_, sub_format_.get(),
                               Rect(s, x0 + 26, y, x1 - x0 - 26 - (size.empty() ? 0.0f : size_w + 12), kRowH),
                               item.name, theme.text);
            if (!size.empty())
                painter_.DrawText(size, Rect(s, x1 - size_w - 1, y, size_w + 2, kRowH), meta_format_.get(),
                                  theme.text_secondary);
        }

        // Footer: the band all the file-operation dialogs share.
        RECT client{};
        GetClientRect(hwnd_, &client);
        const float footer = FooterTop() * s;
        painter_.FillRoundedRect(D2D1::RectF(0, footer, static_cast<float>(client.right), static_cast<float>(client.bottom)), 0,
                                 OpFooter(hwnd_, backdrop_enabled_, dark_));
        painter_.FillRoundedRect(D2D1::RectF(0, footer, static_cast<float>(client.right), footer + 1.0f), 0, theme.stroke_divider);
        const int focus_target = keyboard_focus_ ? FocusTarget(focus_) : -1;
        auto state = [&](int hit, int target) {
            fluent::ControlState st{};
            st.hovered = hover_ == hit;
            st.pressed = pressed_ == hit;
            st.focused = focus_target == target;
            st.keyboard_focus = st.focused;
            return st;
        };
        if (info_.offer_recycle)
            painter_.DrawButton({ ButtonRect(0), l10n::Get(l10n::StringId::PermDeleteSwitchToBin), {},
                                  fluent::ButtonKind::Transparent, state(1, 0) });
        painter_.DrawButton({ ButtonRect(1), l10n::Get(l10n::StringId::Cancel), {}, fluent::ButtonKind::Standard,
                              state(2, 1) });
        painter_.DrawButton({ ButtonRect(2), l10n::Get(l10n::StringId::PermanentDelete), {},
                              fluent::ButtonKind::Danger, state(3, 2) });
        pulse::ui::EndSurface(compositor_);
    }

    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_CREATE:
            scale_ = static_cast<float>(pulse::compat::WindowDpi(hwnd_)) / 96.0f;
            backdrop_enabled_ = pulse::ui::ApplyBackdrop(hwnd_, dark_);
            if (!compositor_.Init(hwnd_)) return -1;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
            CreateFormats();
            ImmAssociateContextEx(hwnd_, nullptr, 0);
            focus_ = FocusCount() - 1; // the delete button, so Enter deletes
            return 0;
        case WM_NCCALCSIZE:
            return 0;
        case WM_NCHITTEST:
            return pulse::ui::BorderlessHitTest(hwnd_, lparam, 40 * scale_, CloseRect());
        case WM_SIZE:
            if (compositor_.Dc()) compositor_.Resize(LOWORD(lparam), HIWORD(lparam));
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_MOVE:
            compositor_.UpdateTextRenderingParams(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST));
            return 0;
        case WM_DPICHANGED: {
            scale_ = HIWORD(wparam) / 96.0f;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetScale(scale_);
            CreateFormats();
            const auto* suggested = reinterpret_cast<RECT*>(lparam);
            SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOOWNERZORDER);
            return 0;
        }
        case WM_MOUSEMOVE: {
            const int next = Hit(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
            if (next != hover_) { hover_ = next; InvalidateRect(hwnd_, nullptr, FALSE); }
            TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, hwnd_, 0 };
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            hover_ = 0;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
            pressed_ = Hit(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP: {
            const int hit = Hit(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
            const int pressed = pressed_;
            pressed_ = 0;
            ReleaseCapture();
            if (hit == pressed && hit != 0) {
                if (hit == 1) Complete(PermanentDeleteChoice::Recycle);
                else if (hit == 3) Complete(PermanentDeleteChoice::Delete);
                else Complete(PermanentDeleteChoice::Cancel);
                return 0;
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_ACTIVATE:
            if (LOWORD(wparam) != WA_INACTIVE) SetFocus(hwnd_);
            return 0;
        case WM_SYSKEYDOWN:
            if (!(lparam & (1 << 29)) && wparam != VK_F10) return Handle(WM_KEYDOWN, wparam, lparam);
            break;
        case WM_KEYDOWN:
            if (wparam == VK_PROCESSKEY) {
                const UINT vk = ImmGetVirtualKey(hwnd_);
                if (vk && vk != VK_PROCESSKEY) wparam = vk;
            }
            if (wparam == VK_ESCAPE) { Complete(PermanentDeleteChoice::Cancel); return 0; }
            if (wparam == VK_RETURN || wparam == VK_SPACE) { Activate(FocusTarget(focus_)); return 0; }
            if (wparam == VK_TAB || wparam == VK_LEFT || wparam == VK_RIGHT) {
                const bool back = wparam == VK_LEFT || (wparam == VK_TAB && GetKeyState(VK_SHIFT) < 0);
                focus_ = (focus_ + (back ? FocusCount() - 1 : 1)) % FocusCount();
                keyboard_focus_ = true;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            break;
        case WM_CLOSE:
            Complete(PermanentDeleteChoice::Cancel);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(hwnd_, &paint);
            Render();
            EndPaint(hwnd_, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            compositor_.Shutdown();
            done_ = true;
            return 0;
        }
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    Compositor compositor_;
    fluent::Painter painter_{ &compositor_ };
    PermanentDeleteInfo info_;
    std::vector<Item> items_;
    size_t files_ = 0;
    size_t folders_ = 0;
    uint64_t total_ = 0;
    bool totals_known_ = false;
    PermanentDeleteChoice choice_ = PermanentDeleteChoice::Cancel;
    D2D1_COLOR_F accent_ = HexColor(0x0078D4);
    float scale_ = 1.0f;
    bool dark_ = false;
    bool backdrop_enabled_ = false;
    bool done_ = false;
    bool keyboard_focus_ = false;
    int hover_ = 0;
    int pressed_ = 0;
    int focus_ = 0;
    ComPtr<IDWriteTextFormat> head_format_;
    ComPtr<IDWriteTextFormat> sub_format_;
    ComPtr<IDWriteTextFormat> meta_format_;
    HICON icons_[kMaxRows] = {};
    ComPtr<ID2D1Bitmap> icon_bitmaps_[kMaxRows];
    ComPtr<IWICImagingFactory> wic_;
};

// File in use, Pulse_文件操作对话框_重设计.html §4: 460 DIP. The programs
// holding the item, each with its own icon (切换到程序 when it has a window).
// The dialog watches the programs and the file and continues on its own once
// the file is free, so retrying is only a 立即重试 link on the waiting line;
// the buttons are 跳过此项 · 结束进程并重试 (when every owner may be ended) ·
// 取消.
class LockedItemWindow {
public:
    LockedItemChoice Show(HWND owner, const LockedItemInfo& info, bool dark, D2D1_COLOR_F accent) {
        owner_ = owner;
        info_ = info;
        dark_ = dark;
        accent_ = accent;
        scale_ = static_cast<float>(pulse::compat::WindowDpi(owner)) / 96.0f;
        gone_.assign(info_.owners.size(), false);
        windows_.assign(info_.owners.size(), nullptr);
        icons_.assign(info_.owners.size(), nullptr);
        bitmaps_.resize(info_.owners.size());
        LoadIcons();
        FindOwnerWindows();
        focus_ = DefaultFocus();
        opened_ = GetTickCount64();

        WNDCLASSEXW wc{ sizeof(wc) };
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = WndProc;
        wc.lpszClassName = kLockedClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!GetClassInfoExW(wc.hInstance, kLockedClass, &wc)) RegisterClassExW(&wc);

        const int width = static_cast<int>(kW * scale_);
        const int height = static_cast<int>(HeightDip() * scale_);
        hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, kLockedClass,
            l10n::Get(l10n::StringId::LockedItemTitle).c_str(), WS_POPUP | WS_THICKFRAME | WS_SYSMENU,
            CW_USEDEFAULT, CW_USEDEFAULT, width, height, owner, nullptr, wc.hInstance, this);
        if (hwnd_) {
            pulse::ui::CenterOwnedWindow(hwnd_, owner_, width, height);
            if (owner_) EnableWindow(owner_, FALSE);
            ShowWindow(hwnd_, SW_SHOW);
            SetForegroundWindow(hwnd_);
            SetFocus(hwnd_);
            StartWatcher();
            SetTimer(hwnd_, kPingTimer, 50, nullptr);
            MSG message{};
            while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (IsWindow(hwnd_)) { HideComposedDialog(hwnd_, owner_); DestroyWindow(hwnd_); }
            hwnd_ = nullptr;
            if (owner_) EnableWindow(owner_, TRUE);
        }
        StopWatcher();
        for (HICON icon : icons_) if (icon) DestroyIcon(icon);
        return choice_;
    }

private:
    static constexpr float kW = 460.0f;
    static constexpr float kPad = 24.0f;
    static constexpr float kListTop = 82.0f;
    static constexpr float kRowH = 56.0f;
    static constexpr float kMoreH = 34.0f;
    static constexpr size_t kMaxRows = 4;
    static constexpr UINT_PTR kPingTimer = 4;
    static constexpr UINT_PTR kContinueTimer = 5;
    static constexpr UINT kWatchMessage = WM_APP + 41;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<LockedItemWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<LockedItemWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }
        return self ? self->Handle(message, wparam, lparam) : DefWindowProcW(hwnd, message, wparam, lparam);
    }

    // ---- layout (DIP) ----------------------------------------------------
    // Headline 18, sub 46, the programs from 82 (56-DIP rows in a 1 px
    // border), the waiting line 12 below, 20 more, then the 64-DIP footer.
    size_t Rows() const { return (std::min)(info_.owners.size(), kMaxRows); }
    bool HasMore() const { return info_.owners.size() > kMaxRows; }
    float ListH() const {
        return 2.0f + static_cast<float>((std::max)(Rows(), static_cast<size_t>(1))) * kRowH + (HasMore() ? kMoreH : 0.0f);
    }
    float WaitTop() const { return kListTop + ListH() + 12.0f; }
    float FooterTop() const { return WaitTop() + 20.0f + 20.0f; }
    float HeightDip() const { return FooterTop() + 64.0f; }
    D2D1_RECT_F CloseRect() const { return Rect(scale_, kW - 44, 4, 40, 32); }
    float ButtonTop() const { return (FooterTop() + 32.0f) * scale_ - painter_.MeasureButtonHeight() * 0.5f; }
    float ButtonW(l10n::StringId id) const {
        return (std::max)(painter_.MeasureButtonWidth(l10n::Get(id)), ScaleDip(scale_, 80.0f));
    }
    D2D1_RECT_F CancelRect() const {
        const float right = (kW - kPad) * scale_, top = ButtonTop();
        return D2D1::RectF(right - ButtonW(l10n::StringId::Cancel), top, right, top + painter_.MeasureButtonHeight());
    }
    D2D1_RECT_F EndRect() const {
        const float right = CancelRect().left - ScaleDip(scale_, 8.0f), top = ButtonTop();
        return D2D1::RectF(right - ButtonW(l10n::StringId::LockedItemEndRetry), top, right,
                           top + painter_.MeasureButtonHeight());
    }
    D2D1_RECT_F SkipRect() const {
        const float left = (kPad - 12.0f) * scale_, top = ButtonTop();
        return D2D1::RectF(left, top, left + ButtonW(l10n::StringId::LockSkip), top + painter_.MeasureButtonHeight());
    }
    float LinkW(l10n::StringId id) const {
        return TextWidth(compositor_.DwriteFactory(), link_format_.get(), l10n::Get(id)) / scale_ + 2.0f;
    }
    D2D1_RECT_F RetryRect() const {
        const float w = LinkW(l10n::StringId::LockReleaseNow);
        return Rect(scale_, kW - kPad - w, WaitTop(), w, 20);
    }
    D2D1_RECT_F SwitchRect(size_t row) const {
        const float w = LinkW(l10n::StringId::LockSwitchTo);
        return Rect(scale_, kW - kPad - 13 - w, kListTop + 1 + static_cast<float>(row) * kRowH + 18, w, 20);
    }

    // Focus order follows the reading order: 立即释放, [跳过此项,]
    // [结束进程并重试,] 取消. Both of the first two end processes, which loses
    // unsaved work, so Enter cancels unless the user moved the focus.
    std::vector<int> FocusOrder() const { // 0 skip, 1 release, 2 end, 3 cancel
        std::vector<int> order;
        if (!released_) order.push_back(1);
        if (info_.can_skip) order.push_back(0);
        if (info_.can_end) order.push_back(2);
        order.push_back(3);
        return order;
    }
    int DefaultFocus() const { return 3; }
    void MoveFocus(bool back) {
        const std::vector<int> order = FocusOrder();
        size_t at = 0;
        while (at < order.size() && order[at] != focus_) ++at;
        if (at == order.size()) at = order.size() - 1;
        at = (at + (back ? order.size() - 1 : 1)) % order.size();
        focus_ = order[at];
    }

    int Hit(float x, float y) const {
        if (pulse::ui::ContainsRect(CloseRect(), x, y)) return 4;
        if (pulse::ui::ContainsRect(CancelRect(), x, y)) return 2;
        if (info_.can_end && pulse::ui::ContainsRect(EndRect(), x, y)) return 3;
        if (info_.can_skip && pulse::ui::ContainsRect(SkipRect(), x, y)) return 1;
        if (!released_ && pulse::ui::ContainsRect(RetryRect(), x, y)) return 5;
        for (size_t i = 0; i < Rows(); ++i)
            if (windows_[i] && !gone_[i] && pulse::ui::ContainsRect(SwitchRect(i), x, y)) return 10 + static_cast<int>(i);
        return 0;
    }

    void Complete(LockedItemChoice choice) {
        choice_ = choice;
        done_ = true;
        if (hwnd_) { HideComposedDialog(hwnd_, owner_); DestroyWindow(hwnd_); }
    }

    void Activate(int target) {
        if (target == 0) Complete(LockedItemChoice::Skip);
        else if (target == 1) Complete(LockedItemChoice::Retry);
        else if (target == 2 && info_.can_end) Complete(LockedItemChoice::EndAndRetry);
        else Complete(LockedItemChoice::Cancel);
    }

    // ---- owners -----------------------------------------------------------
    struct FindWindowContext { DWORD pid; HWND found; };
    static BOOL CALLBACK FindOwnerWindow(HWND hwnd, LPARAM param) {
        auto* context = reinterpret_cast<FindWindowContext*>(param);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != context->pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
        if (GetWindowTextLengthW(hwnd) == 0) return TRUE;
        context->found = hwnd;
        return FALSE;
    }

    void FindOwnerWindows() {
        for (size_t i = 0; i < info_.owners.size(); ++i) {
            FindWindowContext context{ info_.owners[i].pid, nullptr };
            if (!gone_[i] && context.pid) EnumWindows(FindOwnerWindow, reinterpret_cast<LPARAM>(&context));
            windows_[i] = context.found;
        }
    }

    void SwitchTo(size_t index) {
        HWND target = index < windows_.size() ? windows_[index] : nullptr;
        if (!target || !IsWindow(target)) return;
        if (IsIconic(target)) ShowWindow(target, SW_RESTORE);
        AllowSetForegroundWindow(info_.owners[index].pid);
        SetForegroundWindow(target);
    }

    void LoadIcons() {
        const int size = static_cast<int>(std::lround(24.0f * scale_));
        for (size_t i = 0; i < info_.owners.size() && i < kMaxRows; ++i) {
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, info_.owners[i].pid);
            if (!process) continue;
            wchar_t image[MAX_PATH * 2]{};
            DWORD length = ARRAYSIZE(image);
            if (QueryFullProcessImageNameW(process, 0, image, &length)) {
                HICON icon = nullptr;
                UINT id = 0;
                if (PrivateExtractIconsW(image, 0, size, size, &icon, &id, 1, 0) == 1 && icon) icons_[i] = icon;
            }
            CloseHandle(process);
        }
        // Programs without an icon of their own (console tools, ended
        // processes) get the shell's generic application icon.
        for (size_t i = 0; i < info_.owners.size() && i < kMaxRows; ++i)
            if (!icons_[i]) icons_[i] = ShellTypeIcon(L"program.exe", false, 24.0f * scale_);
    }

    ID2D1Bitmap* IconBitmap(size_t index) {
        if (index >= icons_.size() || !icons_[index]) return nullptr;
        if (!bitmaps_[index].get()) bitmaps_[index] = BitmapFromIcon(compositor_.Dc(), wic_, icons_[index]);
        return bitmaps_[index].get();
    }

    // ---- watching ---------------------------------------------------------
    // A worker polls every 500 ms: has each owner exited (same instance), and
    // can the item be opened with no sharing at all (nobody holds it)? Two
    // free reads in a row count as released; folders rely on the owners.
    static bool OwnerGone(const ops::LockOwner& owner) {
        HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, owner.pid);
        if (!process) return GetLastError() == ERROR_INVALID_PARAMETER;
        bool gone = WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
        if (!gone && (owner.start_time.dwLowDateTime | owner.start_time.dwHighDateTime)) {
            FILETIME created{}, exited{}, kernel{}, user{};
            if (GetProcessTimes(process, &created, &exited, &kernel, &user))
                gone = CompareFileTime(&created, &owner.start_time) != 0; // the PID was reused
        }
        CloseHandle(process);
        return gone;
    }

    static bool ItemFree(const std::wstring& path) {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const DWORD error = GetLastError();
            return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        }
        if (attributes & FILE_ATTRIBUTE_DIRECTORY) return false;
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        CloseHandle(file);
        return true;
    }

    void StartWatcher() {
        stop_ = false;
        const HWND target = hwnd_;
        const std::vector<ops::LockOwner> owners = info_.owners;
        const std::wstring path = info_.path;
        watcher_ = std::thread([this, target, owners, path] {
            int free_reads = 0;
            while (!stop_.load()) {
                uint64_t mask = 0;
                bool all_gone = !owners.empty();
                for (size_t i = 0; i < owners.size(); ++i) {
                    const bool gone = OwnerGone(owners[i]);
                    if (gone && i < 64) mask |= 1ull << i;
                    all_gone = all_gone && gone;
                }
                free_reads = !path.empty() && ItemFree(path) ? free_reads + 1 : 0;
                const bool released = all_gone || free_reads >= 2;
                watch_mask_.store(mask);
                PostMessageW(target, kWatchMessage, released ? 1 : 0, 0);
                for (int slice = 0; slice < 10 && !stop_.load(); ++slice) Sleep(50);
            }
        });
    }

    void StopWatcher() {
        stop_ = true;
        if (watcher_.joinable()) watcher_.join();
    }

    void OnWatch(bool released) {
        const uint64_t mask = watch_mask_.load();
        bool changed = false;
        for (size_t i = 0; i < gone_.size() && i < 64; ++i) {
            const bool gone = (mask >> i) & 1u;
            if (gone && !gone_[i]) { gone_[i] = true; changed = true; }
        }
        if (changed) FindOwnerWindows();
        if (released && !released_) {
            released_ = true;
            released_at_ = GetTickCount64();
            // Every row reads as closed once the file is free.
            for (size_t i = 0; i < gone_.size(); ++i) gone_[i] = true;
            if (focus_ == 1) focus_ = FocusOrder().front();
            SetTimer(hwnd_, kContinueTimer, 900, nullptr);
            changed = true;
        }
        if (changed) InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // ---- drawing ----------------------------------------------------------
    void CreateFormats() {
        auto* factory = compositor_.DwriteFactory();
        auto make = [&](ComPtr<IDWriteTextFormat>& out, typography::FontRole role, float size, DWRITE_FONT_WEIGHT weight) {
            out.reset();
            if (!factory) return;
            typography::TextFormatSpec spec;
            spec.role = role;
            spec.size = size * scale_;
            spec.weight = weight;
            if (FAILED(typography::CreateTextFormat(factory, spec, &out)) || !out.get()) return;
            out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        };
        make(head_format_, typography::FontRole::Display, 18.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        make(sub_format_, typography::FontRole::Text, 13.0f, DWRITE_FONT_WEIGHT_NORMAL);
        make(name_format_, typography::FontRole::Text, 14.0f, DWRITE_FONT_WEIGHT_NORMAL);
        make(meta_format_, typography::FontRole::Text, 12.0f, DWRITE_FONT_WEIGHT_NORMAL);
        make(link_format_, typography::FontRole::Text, 12.0f, DWRITE_FONT_WEIGHT_NORMAL);
    }

    std::wstring Verb() const {
        switch (info_.type) {
        case ops::OpType::RealDelete: return l10n::Get(l10n::StringId::LockVerbDelete);
        case ops::OpType::Move: return l10n::Get(l10n::StringId::LockVerbMove);
        case ops::OpType::Copy: return l10n::Get(l10n::StringId::LockVerbCopy);
        case ops::OpType::Rename:
        case ops::OpType::BatchRename: return l10n::Get(l10n::StringId::LockVerbRename);
        default: return l10n::Get(l10n::StringId::LockVerbRecycle);
        }
    }

    // A text link: accent text, underlined while hovered, a focus ring after
    // keyboard use.
    void DrawLink(const std::wstring& text, const D2D1_RECT_F& r, bool hovered, bool focused, D2D1_COLOR_F color,
                  const Theme& theme) {
        painter_.DrawText(text, r, link_format_.get(), color);
        const float s = scale_;
        if (hovered) {
            const float y = std::round((r.top + r.bottom) * 0.5f + ScaleDip(s, 7.0f));
            painter_.FillRoundedRect(D2D1::RectF(r.left, y, r.right - ScaleDip(s, 2.0f), y + (std::max)(1.0f, std::round(s))), 0,
                                     color);
        }
        if (focused) {
            const float g = ScaleDip(s, 3.0f);
            painter_.StrokeRoundedRect(D2D1::RectF(r.left - g, r.top - ScaleDip(s, 1.0f), r.right + g, r.bottom + ScaleDip(s, 1.0f)),
                                       ScaleDip(s, 4.0f), theme.text, ScaleDip(s, 2.0f));
        }
    }

    void Render() {
        if (compositor_.NeedsRecovery()) {
            if (!compositor_.Recover()) return;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
            CreateFormats();
            for (auto& bitmap : bitmaps_) bitmap.reset();
        }
        if (!compositor_.Dc()) return;
        if (!head_format_.get()) CreateFormats();
        auto* dc = compositor_.Dc();
        const float s = scale_;
        const bool high_contrast = IsHighContrast();
        const Theme theme = high_contrast ? MakeHighContrastTheme() : MakeTheme(dark_, accent_);
        pulse::ui::BeginSurface(compositor_, painter_, theme, dark_, high_contrast, backdrop_enabled_, s);
        auto mix = [](D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
            return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a);
        };
        const D2D1_COLOR_F accent = high_contrast ? theme.accent
            : dark_ ? mix(theme.accent, HexColor(0xFFFFFF), 0.28f) : theme.accent;
        const D2D1_COLOR_F link = high_contrast ? theme.accent
            : dark_ ? mix(theme.accent, HexColor(0xFFFFFF), 0.45f) : mix(theme.accent, HexColor(0x000000), 0.2f);
        const D2D1_COLOR_F ok = high_contrast ? theme.accent : dark_ ? HexColor(0x6CCB5F) : HexColor(0x0F7B0F);
        const D2D1_COLOR_F track = high_contrast ? WithAlpha(theme.text, 0.12f)
            : dark_ ? HexColor(0xFFFFFF, 0.06f) : HexColor(0x000000, 0.045f);
        const D2D1_COLOR_F stroke = high_contrast ? theme.text
            : dark_ ? HexColor(0xFFFFFF, 0.0698f) : HexColor(0x000000, 0.0578f);
        const bool keyboard = keyboard_focus_;

        fluent::ControlState close_state{};
        close_state.hovered = hover_ == 4;
        close_state.pressed = pressed_ == 4;
        painter_.DrawTitleBarButton(CloseRect(), fluent::TitleBarButtonRole::Close, {}, close_state);

        const std::wstring leaf = LeafName(info_.path);
        DrawEllipsizedText(compositor_, head_format_.get(), Rect(s, kPad, 18, kW - kPad - 64, 24),
                           l10n::Get(l10n::StringId::LockSubLead) + leaf + l10n::Get(l10n::StringId::LockInUseEnd),
                           theme.text);
        DrawEllipsizedText(compositor_, sub_format_.get(), Rect(s, kPad, 46, kW - kPad * 2, 20),
                           l10n::Get(l10n::StringId::LockCloseLead) + Verb() + l10n::Get(l10n::StringId::LockCloseEnd),
                           theme.text_secondary);

        // The programs: a 1 px border, 56-DIP rows split by dividers.
        const auto box = Rect(s, kPad, kListTop, kW - kPad * 2, ListH());
        painter_.StrokeRoundedRect(D2D1::RectF(box.left + 0.5f, box.top + 0.5f, box.right - 0.5f, box.bottom - 0.5f),
                                   ScaleDip(s, 6.0f), stroke, 1.0f);
        const float x0 = kPad + 13.0f;
        const float x1 = kW - kPad - 13.0f;
        auto divider = [&](float y) {
            const float line = std::round(y * s);
            painter_.FillRoundedRect(D2D1::RectF(box.left + 1.0f, line, box.right - 1.0f, line + 1.0f), 0, theme.stroke_divider);
        };
        for (size_t i = 0; i < Rows(); ++i) {
            const ops::LockOwner& owner = info_.owners[i];
            const float y = kListTop + 1.0f + static_cast<float>(i) * kRowH;
            if (i > 0) divider(y);
            const auto icon = Rect(s, x0, y + 16, 24, 24);
            if (ID2D1Bitmap* bitmap = IconBitmap(i))
                dc->DrawBitmap(bitmap, icon, gone_[i] ? 0.55f : 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            else
                DrawPageIcon(dc, icon.left, icon.top, s * 0.75f);

            std::wstring right_text;
            float right_w = 0.0f;
            if (gone_[i]) {
                right_text = l10n::Get(l10n::StringId::LockClosed);
                right_w = TextWidth(compositor_.DwriteFactory(), meta_format_.get(), right_text) / s + 2.0f;
                painter_.DrawText(right_text, Rect(s, x1 - right_w, y, right_w, kRowH), meta_format_.get(), ok);
            } else if (windows_[i]) {
                const auto r = SwitchRect(i);
                right_w = (r.right - r.left) / s;
                DrawLink(l10n::Get(l10n::StringId::LockSwitchTo), r, hover_ == 10 + static_cast<int>(i), false, link, theme);
            }
            const float text_w = x1 - (x0 + 36) - (right_w > 0 ? right_w + 12 : 0.0f);
            const std::wstring name = !owner.app_name.empty() ? owner.app_name
                                    : !owner.image_name.empty() ? owner.image_name : L"PID " + std::to_wstring(owner.pid);
            DrawEllipsizedText(compositor_, name_format_.get(), Rect(s, x0 + 36, y + 9, text_w, 20), name,
                               gone_[i] ? theme.text_secondary : theme.text);
            std::wstring meta = owner.image_name.empty() ? std::wstring() : owner.image_name + L" \x00B7 ";
            meta += L"PID " + std::to_wstring(owner.pid);
            DrawEllipsizedText(compositor_, meta_format_.get(), Rect(s, x0 + 36, y + 29, text_w, 18), meta,
                               theme.text_secondary);
        }
        if (HasMore()) {
            const float y = kListTop + 1.0f + static_cast<float>(Rows()) * kRowH;
            divider(y);
            painter_.DrawText(l10n::Get(l10n::StringId::LockMoreLead) +
                                  pulse::format::GroupedInt(info_.owners.size() - Rows()) +
                                  l10n::Get(l10n::StringId::LockMoreEnd),
                              Rect(s, x0, y, x1 - x0, kMoreH), meta_format_.get(), theme.text_secondary);
        }

        // Waiting: a spinning ring and 立即重试; a check once the file is free.
        const float wy = WaitTop();
        if (released_) {
            DrawStatusIcon(dc, StatusIcon::Ok, kPad, wy + 2, s, ok, dark_ && !high_contrast);
            painter_.DrawText(l10n::Get(l10n::StringId::LockReleased), Rect(s, kPad + 22, wy, kW - kPad * 2 - 22, 20),
                              meta_format_.get(), ok);
        } else {
            const D2D1_POINT_2F center = D2D1::Point2F(ScaleDip(s, kPad + 7.0f), ScaleDip(s, wy + 10.0f));
            const float radius = ScaleDip(s, 6.0f), width = ScaleDip(s, 2.0f);
            StrokeArc(dc, center, radius, 0.0f, 360.0f, track, width);
            const float turn = static_cast<float>((GetTickCount64() - opened_) % 900) / 900.0f * 360.0f;
            StrokeArc(dc, center, radius, turn - 45.0f, 90.0f, accent, width);
            const auto retry = RetryRect();
            painter_.DrawText(l10n::Get(l10n::StringId::LockWaiting),
                              Rect(s, kPad + 22, wy, (retry.left / s) - kPad - 22 - 8, 20), meta_format_.get(),
                              theme.text_secondary);
            DrawLink(l10n::Get(l10n::StringId::LockReleaseNow), retry, hover_ == 5, keyboard && focus_ == 1, link, theme);
        }

        // Footer: the band all the file-operation dialogs share.
        RECT client{};
        GetClientRect(hwnd_, &client);
        const float footer = FooterTop() * s;
        painter_.FillRoundedRect(D2D1::RectF(0, footer, static_cast<float>(client.right), static_cast<float>(client.bottom)), 0,
                                 OpFooter(hwnd_, backdrop_enabled_, dark_));
        painter_.FillRoundedRect(D2D1::RectF(0, footer, static_cast<float>(client.right), footer + 1.0f), 0, theme.stroke_divider);
        auto state = [&](int hit, int target) {
            fluent::ControlState st{};
            st.hovered = hover_ == hit;
            st.pressed = pressed_ == hit;
            st.focused = keyboard && focus_ == target;
            st.keyboard_focus = st.focused;
            return st;
        };
        if (info_.can_skip)
            painter_.DrawButton({ SkipRect(), l10n::Get(l10n::StringId::LockSkip), {}, fluent::ButtonKind::Transparent,
                                  state(1, 0) });
        if (info_.can_end)
            painter_.DrawButton({ EndRect(), l10n::Get(l10n::StringId::LockedItemEndRetry), {},
                                  fluent::ButtonKind::Standard, state(3, 2) });
        painter_.DrawButton({ CancelRect(), l10n::Get(l10n::StringId::Cancel), {}, fluent::ButtonKind::Standard,
                              state(2, 3) });
        pulse::ui::EndSurface(compositor_);
    }

    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_CREATE:
            scale_ = static_cast<float>(pulse::compat::WindowDpi(hwnd_)) / 96.0f;
            backdrop_enabled_ = pulse::ui::ApplyBackdrop(hwnd_, dark_);
            if (!compositor_.Init(hwnd_)) return -1;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
            CreateFormats();
            ImmAssociateContextEx(hwnd_, nullptr, 0);
            return 0;
        case WM_NCCALCSIZE:
            return 0;
        case WM_NCHITTEST:
            return pulse::ui::BorderlessHitTest(hwnd_, lparam, 40 * scale_, CloseRect());
        case WM_SIZE:
            if (compositor_.Dc()) compositor_.Resize(LOWORD(lparam), HIWORD(lparam));
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_MOVE:
            compositor_.UpdateTextRenderingParams(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST));
            return 0;
        case WM_DPICHANGED: {
            scale_ = HIWORD(wparam) / 96.0f;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetScale(scale_);
            CreateFormats();
            const auto* suggested = reinterpret_cast<RECT*>(lparam);
            SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOOWNERZORDER);
            return 0;
        }
        case kWatchMessage:
            OnWatch(wparam != 0);
            return 0;
        case WM_TIMER:
            if (wparam == kPingTimer) {
                // Only the ring animates.
                if (released_) return 0;
                RECT strip{ static_cast<LONG>((kPad - 2) * scale_), static_cast<LONG>((WaitTop() + 1) * scale_),
                            static_cast<LONG>((kPad + 17) * scale_), static_cast<LONG>((WaitTop() + 19) * scale_) };
                InvalidateRect(hwnd_, &strip, FALSE);
                return 0;
            }
            if (wparam == kContinueTimer) {
                KillTimer(hwnd_, kContinueTimer);
                Complete(LockedItemChoice::Retry);
                return 0;
            }
            break;
        case WM_MOUSEMOVE: {
            const int next = Hit(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
            if (next != hover_) { hover_ = next; InvalidateRect(hwnd_, nullptr, FALSE); }
            TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, hwnd_, 0 };
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            hover_ = 0;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
            pressed_ = Hit(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP: {
            const int hit = Hit(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
            const int pressed = pressed_;
            pressed_ = 0;
            ReleaseCapture();
            if (hit == pressed && hit != 0) {
                if (hit >= 10) { SwitchTo(static_cast<size_t>(hit - 10)); InvalidateRect(hwnd_, nullptr, FALSE); return 0; }
                if (hit == 1) Activate(0);
                // 立即释放 ends the owners right away instead of waiting for
                // them to close; retrying alone is the 重试 button's job.
                else if (hit == 5) Complete(LockedItemChoice::EndAndRetry);
                else if (hit == 3) Activate(2);
                else Complete(LockedItemChoice::Cancel);
                return 0;
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_ACTIVATE:
            if (LOWORD(wparam) != WA_INACTIVE) SetFocus(hwnd_);
            return 0;
        case WM_SYSKEYDOWN:
            if (!(lparam & (1 << 29)) && wparam != VK_F10) return Handle(WM_KEYDOWN, wparam, lparam);
            break;
        case WM_KEYDOWN:
            if (wparam == VK_PROCESSKEY) {
                const UINT vk = ImmGetVirtualKey(hwnd_);
                if (vk && vk != VK_PROCESSKEY) wparam = vk;
            }
            if (wparam == VK_ESCAPE) { Complete(LockedItemChoice::Cancel); return 0; }
            if (wparam == VK_RETURN || wparam == VK_SPACE) { Activate(focus_); return 0; }
            if (wparam == VK_TAB || wparam == VK_LEFT || wparam == VK_RIGHT) {
                MoveFocus(wparam == VK_LEFT || (wparam == VK_TAB && GetKeyState(VK_SHIFT) < 0));
                keyboard_focus_ = true;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            break;
        case WM_CLOSE:
            Complete(LockedItemChoice::Cancel);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(hwnd_, &paint);
            Render();
            EndPaint(hwnd_, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            KillTimer(hwnd_, kPingTimer);
            KillTimer(hwnd_, kContinueTimer);
            compositor_.Shutdown();
            done_ = true;
            return 0;
        }
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    Compositor compositor_;
    fluent::Painter painter_{ &compositor_ };
    LockedItemInfo info_;
    std::vector<bool> gone_;
    std::vector<HWND> windows_;
    std::vector<HICON> icons_;
    std::vector<ComPtr<ID2D1Bitmap>> bitmaps_;
    ComPtr<IWICImagingFactory> wic_;
    std::thread watcher_;
    std::atomic<bool> stop_{ false };
    std::atomic<uint64_t> watch_mask_{ 0 };
    LockedItemChoice choice_ = LockedItemChoice::Cancel;
    D2D1_COLOR_F accent_ = HexColor(0x0078D4);
    float scale_ = 1.0f;
    bool dark_ = false;
    bool backdrop_enabled_ = false;
    bool done_ = false;
    bool released_ = false;
    ULONGLONG released_at_ = 0;
    ULONGLONG opened_ = 0;
    bool keyboard_focus_ = false;
    int hover_ = 0;
    int pressed_ = 0;
    int focus_ = 0;
    ComPtr<IDWriteTextFormat> head_format_;
    ComPtr<IDWriteTextFormat> sub_format_;
    ComPtr<IDWriteTextFormat> name_format_;
    ComPtr<IDWriteTextFormat> meta_format_;
    ComPtr<IDWriteTextFormat> link_format_;
};

} // namespace

FileOperationWindow::FileOperationWindow() : painter_(&compositor_) {}
FileOperationWindow::~FileOperationWindow() { Destroy(); }

bool FileOperationWindow::Create(HWND owner, FileOperationCallbacks callbacks) {
    if (hwnd_) return true;
    owner_ = owner;
    callbacks_ = std::move(callbacks);
    dark_ = ShouldUseDarkMode(ThemeMode::Auto);
    accent_ = GetAccentColor();

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = kTransferClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    if (!GetClassInfoExW(wc.hInstance, kTransferClass, &wc)) RegisterClassExW(&wc);
    const UINT owner_dpi = pulse::compat::WindowDpi(owner_);
    const int width = MulDiv(static_cast<int>(kDlgW), owner_dpi, 96);
    const int height = MulDiv(static_cast<int>(kCollapsedH), owner_dpi, 96);
    hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, kTransferClass, l10n::Get(l10n::StringId::OpWindow).c_str(),
        WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, width, height, owner_, nullptr,
        wc.hInstance, this);
    return hwnd_ != nullptr;
}

void FileOperationWindow::Destroy() {
    if (hwnd_) { HideComposedDialog(hwnd_); DestroyWindow(hwnd_); }
    hwnd_ = nullptr;
}

void FileOperationWindow::SetTheme(bool dark, D2D1_COLOR_F accent) {
    dark_ = dark;
    accent_ = accent;
    if (hwnd_) {
        ApplyWindowTheme();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void FileOperationWindow::SetTaskStart(uint64_t task_id,
                                       std::chrono::steady_clock::time_point started) {
    if (task_id == 0 || task_id != status_.task_id) return;
    if (started.time_since_epoch().count() == 0) return;
    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    const ULONGLONG now = GetTickCount64();
    if (age <= 0 || static_cast<ULONGLONG>(age) >= now) return;
    task_start_tick_ = now - static_cast<ULONGLONG>(age);
}

void FileOperationWindow::CreateCardFormats() {
    auto* factory = compositor_.DwriteFactory();
    auto make = [&](ComPtr<IDWriteTextFormat>& out, typography::FontRole role, float size,
                    DWRITE_FONT_WEIGHT weight) {
        out.reset();
        if (!factory) return;
        typography::TextFormatSpec spec;
        spec.role = role;
        spec.size = size * scale_;
        spec.weight = weight;
        if (FAILED(typography::CreateTextFormat(factory, spec, &out)) || !out.get()) return;
        out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    };
    make(pct_format_, typography::FontRole::Display, 28.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    make(unit_format_, typography::FontRole::Display, 16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    make(title_format_, typography::FontRole::Text, 14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    make(speed_format_, typography::FontRole::Text, 14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    make(label_format_, typography::FontRole::Text, 12.0f, DWRITE_FONT_WEIGHT_NORMAL);
    make(value_format_, typography::FontRole::Text, 13.0f, DWRITE_FONT_WEIGHT_NORMAL);
    make(chip_format_, typography::FontRole::Text, 13.0f, DWRITE_FONT_WEIGHT_NORMAL);
    make(glyph_format_, typography::FontRole::Icon, 12.0f, DWRITE_FONT_WEIGHT_NORMAL);
    make(tile_glyph_format_, typography::FontRole::Icon, 18.0f, DWRITE_FONT_WEIGHT_NORMAL);
    if (glyph_format_.get()) glyph_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    if (tile_glyph_format_.get()) tile_glyph_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
}

namespace {
bool IsPartialFailure(const ops::OpStatus& status) {
    return !status.active && status.phase == ops::OpPhase::Failed &&
           (status.type == ops::OpType::Copy || status.type == ops::OpType::Move ||
            status.type == ops::OpType::RecycleDelete || status.type == ops::OpType::RealDelete) &&
           status.failed_items > 0 && !status.failures.empty();
}

// "%s … %s" catalog text with grouped numbers.
std::wstring FormatCounts(l10n::StringId id, uint64_t first, uint64_t second = 0) {
    const std::wstring format = l10n::Get(id);
    const std::wstring a = pulse::format::GroupedInt(first);
    const std::wstring b = pulse::format::GroupedInt(second);
    wchar_t text[256];
    if (swprintf_s(text, format.c_str(), a.c_str(), b.c_str()) < 0) return a;
    return text;
}
}  // namespace

TransferFailureView FileOperationWindow::FailureView() const {
    TransferFailureView view;
    view.can_open = !status_.destination_dir.empty() && callbacks_.open_folder != nullptr;
    if (!board_) return view;
    view.on = true;
    if (callbacks_.retry_failed)
        view.can_retry = std::any_of(board_->rows.begin(), board_->rows.end(), [](const FailureRow& row) {
            return row.state == FailureRow::State::Failed;
        });
    return view;
}

bool FileOperationWindow::TracksRetry(uint64_t task_id) const {
    return board_ && task_id != 0 &&
           std::find(board_->retry_tasks.begin(), board_->retry_tasks.end(), task_id) != board_->retry_tasks.end();
}

size_t FileOperationWindow::VisibleFailureRows() const {
    return board_ ? (std::min)(board_->rows.size(), kFailMaxRows) : 0;
}

float FileOperationWindow::FailureBlockDip() const {
    if (!board_) return 0.0f;
    const float list_h = static_cast<float>(VisibleFailureRows()) * kFailRowH +
                         (board_->unlisted > 0 ? kFailMoreH : 0.0f) + 2.0f;
    return kFailListTop + list_h + 14.0f - kContentH;
}

// The task that failed, with what its retries have fixed since: once nothing
// is left it reads as a success (and closes itself like one).
ops::OpStatus FileOperationWindow::BoardStatus() const {
    ops::OpStatus status = board_->status;
    uint64_t failed = board_->unlisted;
    uint64_t retried = 0;
    for (const auto& row : board_->rows)
        if (row.state != FailureRow::State::Done) ++failed;
        else ++retried;
    status.failed_items = failed;
    // A delete's title counts what is gone, the retried items included.
    if (status.type == ops::OpType::RecycleDelete || status.type == ops::OpType::RealDelete)
        status.completed_items += retried;
    if (failed == 0) {
        status.phase = ops::OpPhase::Completed;
        status.last_error.clear();
    }
    return status;
}

void FileOperationWindow::RetryFailureRows(const std::vector<size_t>& rows) {
    if (!board_ || !callbacks_.retry_failed) return;
    std::map<uint64_t, std::vector<size_t>> by_task;   // origin task -> its indices
    for (size_t r : rows) {
        if (r >= board_->rows.size()) continue;
        const FailureRow& row = board_->rows[r];
        if (row.state == FailureRow::State::Failed) by_task[row.origin_task].push_back(row.origin_index);
    }
    for (const auto& [task, indices] : by_task) {
        const auto spawns = callbacks_.retry_failed(task, indices);
        for (const auto& spawn : spawns) {
            board_->retry_tasks.push_back(spawn.task_id);
            for (size_t index : spawn.items) {
                for (auto& row : board_->rows) {
                    if (row.state != FailureRow::State::Failed || row.origin_task != task ||
                        row.origin_index != index) continue;
                    row.retry_task = spawn.task_id;
                    row.state = FailureRow::State::Retrying;
                }
            }
        }
    }
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void FileOperationWindow::FinishRetry(const ops::OpStatus& done) {
    for (auto& row : board_->rows) {
        // Only rows still waiting on it: the same finish arrives repeatedly.
        if (row.state != FailureRow::State::Retrying || row.retry_task != done.task_id) continue;
        row.retry_task = 0;
        const auto found = std::find_if(done.failures.begin(), done.failures.end(), [&](const ops::OpFailure& failure) {
            return _wcsicmp(failure.source.c_str(), row.failure.source.c_str()) == 0;
        });
        if (done.phase == ops::OpPhase::Completed) {
            row.state = FailureRow::State::Done;
        } else if (found != done.failures.end()) {
            // Failed again: its next retry goes through the retry's list.
            row.failure = *found;
            row.origin_task = done.task_id;
            row.origin_index = static_cast<size_t>(found - done.failures.begin());
            row.state = FailureRow::State::Failed;
        } else if (done.failed_items == 0) {
            // The retry stopped as a whole (or was cancelled).
            row.state = FailureRow::State::Failed;
            if (!done.last_error.empty()) {
                const size_t bar = done.last_error.rfind(L" | ");
                row.failure.reason = l10n::ServiceErrorText(
                    bar == std::wstring::npos ? done.last_error : done.last_error.substr(0, bar));
            }
        } else {
            row.state = FailureRow::State::Done;
        }
    }
}

void FileOperationWindow::Update(const ops::OpStatus& incoming) {
    ops::OpStatus status = incoming;
    const bool had_board = board_.has_value();
    if (board_) {
        const bool spawned = incoming.task_id != 0 &&
            std::find(board_->retry_tasks.begin(), board_->retry_tasks.end(), incoming.task_id) !=
                board_->retry_tasks.end();
        if (spawned || incoming.task_id == board_->task_id) {
            if (spawned && !incoming.active && (incoming.phase == ops::OpPhase::Completed ||
                                                incoming.phase == ops::OpPhase::Failed))
                FinishRetry(incoming);
            // Retries that finished while another one was being reported.
            if (callbacks_.task_outcome) {
                std::vector<uint64_t> waiting;
                for (const auto& row : board_->rows)
                    if (row.state == FailureRow::State::Retrying && row.retry_task != incoming.task_id &&
                        std::find(waiting.begin(), waiting.end(), row.retry_task) == waiting.end())
                        waiting.push_back(row.retry_task);
                for (uint64_t task : waiting)
                    if (const auto outcome = callbacks_.task_outcome(task))
                        if (!outcome->active) FinishRetry(*outcome);
            }
            status = BoardStatus();
        } else {
            board_.reset();
            board_scroll_ = 0;
            failure_buttons_.clear();
        }
    }
    if (!board_ && IsPartialFailure(status)) {
        FailureBoard board;
        board.task_id = status.task_id;
        board.status = status;
        for (size_t i = 0; i < status.failures.size(); ++i) {
            FailureRow row;
            row.failure = status.failures[i];
            row.origin_task = status.task_id;
            row.origin_index = i;
            board.rows.push_back(std::move(row));
        }
        board.unlisted = status.failed_items > status.failures.size()
            ? status.failed_items - status.failures.size() : 0;
        board_ = std::move(board);
        board_scroll_ = 0;
    }
    if (had_board != board_.has_value()) ResizeForDetails(false);
    const bool new_task = status.task_id != status_.task_id;
    const ULONGLONG now = GetTickCount64();
    if (!new_task && status.active && status_.active) {
        const bool leaving_authorization = status.authorization == ops::AuthorizationState::None &&
            status_.authorization != ops::AuthorizationState::None;
        const bool retry_ended = status_.authorization_retrying && !status.authorization_retrying;
        if (hold_until_ == 0) {
            if (leaving_authorization) hold_until_ = now + kAuthorizationExitDelayMs;
            else if (retry_ended) hold_until_ = retry_shown_tick_ + kRetryMinVisibleMs;
        }
        if ((leaving_authorization || retry_ended) && now < hold_until_) {
            held_status_ = status;  // released by the render timer if it sticks
            has_held_status_ = true;
            return;
        }
    }
    hold_until_ = 0;
    has_held_status_ = false;
    if (status.authorization_retrying && !status_.authorization_retrying) retry_shown_tick_ = now;
    const bool was_active = status_.active;
    const bool was_succeeded = !new_task && !status_.active &&
        status_.phase == ops::OpPhase::Completed && status_.last_error.empty();
    if (new_task || status.authorization != status_.authorization) pressed_ = 0;
    const bool authorization_changed = (status.authorization != ops::AuthorizationState::None) !=
                                       (status_.authorization != ops::AuthorizationState::None);
    status_ = ops::PresentOperationStatus(status);
    if (new_task) {
        graph_.Reset();
        SyncGraph();   // a card built late starts with the run so far
        task_start_tick_ = now;
        task_end_tick_ = 0;
        countdown_base_ = 0;
        kept_ = false;
        finish_unseen_ = false;
    }
    const bool finished = !status_.active &&
        (status_.phase == ops::OpPhase::Completed || status_.phase == ops::OpPhase::Failed);
    if (!finished) task_end_tick_ = 0;
    else if (task_end_tick_ == 0) task_end_tick_ = now;
    const bool succeeded = finished && status_.phase == ops::OpPhase::Completed &&
        status_.last_error.empty();
    // The countdown starts only when the finish happens in front of the user
    // in a window the app opened (SetAutoClose); a summary opened from the
    // pill stays until closed.
    if (!succeeded || !auto_close_ || kept_) {
        countdown_base_ = 0;
        finish_unseen_ = false;
    } else if (countdown_base_ == 0 && !was_succeeded && !closing_ && hwnd_ &&
               IsWindowVisible(hwnd_)) {
        if (IsIconic(hwnd_)) finish_unseen_ = true;
        else countdown_base_ = now;
    }
    if (!hwnd_) return;
    if (authorization_changed) ResizeForDetails(true);
    InvalidateRect(hwnd_, nullptr, FALSE);
    // The render timer stops once an operation ends; restart it for the next
    // one and for the auto-close countdown.
    if (((status_.active && !was_active) || countdown_base_ != 0) && IsWindowVisible(hwnd_))
        SetTimer(hwnd_, kRenderTimer, 33, nullptr);
}

void FileOperationWindow::Show(bool activate) {
    if (!hwnd_) return;
    const bool was_visible = IsWindowVisible(hwnd_) != FALSE;
    if (closing_) {
        // Reopened during the exit zoom: put the card back at full size.
        closing_ = false;
        compositor_.ClearZoom();
        ApplyWindowTheme();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    if (!positioned_) {
        RECT rect{};
        GetWindowRect(hwnd_, &rect);
        pulse::ui::CenterOwnedWindow(hwnd_, owner_, rect.right - rect.left, rect.bottom - rect.top);
        positioned_ = true;
    }
    if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
    SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER |
                 (activate ? 0 : SWP_NOACTIVATE) | SWP_SHOWWINDOW);
    // SWP_SHOWWINDOW sends no WM_SHOWWINDOW; start the animation timer here.
    if (!was_visible) {
        SyncGraph();   // fill in what ran while the card was hidden
        InvalidateRect(hwnd_, nullptr, FALSE);
        SetTimer(hwnd_, kRenderTimer, 33, nullptr);
    }
    if (activate) SetForegroundWindow(hwnd_);
}

void FileOperationWindow::SetAutoClose(bool on) {
    auto_close_ = on;
    if (on || countdown_base_ == 0) return;
    countdown_base_ = 0;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void FileOperationWindow::Hide() {
    countdown_base_ = 0;
    if (!hwnd_) return;
    ShowWindow(hwnd_, SW_HIDE);
    if (closing_) {
        closing_ = false;
        compositor_.ClearZoom();
        ApplyWindowTheme();
    }
}

bool FileOperationWindow::IsVisible() const {
    return hwnd_ && IsWindowVisible(hwnd_);
}

void FileOperationWindow::ApplyWindowTheme() {
    backdrop_enabled_ = pulse::ui::ApplyBackdrop(hwnd_, dark_);
}

void FileOperationWindow::ResizeForDetails(bool preserve_center) {
    if (!hwnd_) return;
    RECT rect{};
    GetWindowRect(hwnd_, &rect);
    const int width = rect.right - rect.left;
    const int old_height = rect.bottom - rect.top;
    const float height_dip = status_.authorization != ops::AuthorizationState::None
        ? kAuthorizationH : (detailed_ ? kDetailedH : kCollapsedH) + FailureBlockDip();
    const int height = static_cast<int>(height_dip * scale_);
    const int y = preserve_center ? rect.top - (height - old_height) / 2 : rect.top;
    SetWindowPos(hwnd_, nullptr, rect.left, y, width, height,
                 SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
}

int FileOperationWindow::HitTestControl(float x, float y) const {
    if (closing_) return 0;
    const auto labels = MakeTransferLabels(status_, detailed_, FailureView());
    const auto chrome = MakeTransferChrome(scale_,
        static_cast<float>(compositor_.Width()), static_cast<float>(compositor_.Height()),
        painter_, labels);
    if (pulse::ui::ContainsRect(chrome.close, x, y)) return 1;
    if (pulse::ui::ContainsRect(chrome.minimize, x, y)) return 2;
    if (board_ && !status_.active) {
        for (const auto& [row, bounds] : failure_buttons_)
            if (pulse::ui::ContainsRect(bounds, x, y)) return kHitFailRow + static_cast<int>(row);
    }
    if (labels.show_open && pulse::ui::ContainsRect(chrome.open, x, y)) return kHitOpen;
    if (labels.show_retry && labels.actions_enabled && pulse::ui::ContainsRect(chrome.retry, x, y)) return 6;
    if (labels.show_skip && labels.actions_enabled && pulse::ui::ContainsRect(chrome.skip, x, y)) return 7;
    if (labels.show_pause && pulse::ui::ContainsRect(chrome.pause, x, y)) return 3;
    if (pulse::ui::ContainsRect(chrome.cancel, x, y)) return 4;
    if (labels.show_details && pulse::ui::ContainsRect(chrome.details, x, y)) return 5;
    return 0;
}

void FileOperationWindow::RequestClose() {
    if (closing_) return;
    const bool emptying = status_.type == ops::OpType::EmptyRecycle;
    if (status_.active && status_.phase != ops::OpPhase::Completed &&
        status_.phase != ops::OpPhase::Failed && !emptying) {
        if (callbacks_.cancel) callbacks_.cancel();
        return;
    }
    if (callbacks_.dismiss) callbacks_.dismiss();
    Hide();
}

bool FileOperationWindow::AutoClosePending() const {
    return countdown_base_ != 0 && !closing_ && !status_.active &&
           status_.phase == ops::OpPhase::Completed && status_.last_error.empty();
}

void FileOperationWindow::TickAutoClose(ULONGLONG now) {
    if (closing_) {
        if (now - closing_tick_ >= kCloseAnimMs) FinishAutoClose();
        return;
    }
    // Hovering does not hold the card (an untouched card closes itself); a
    // button held down or a minimized card restarts the countdown.
    countdown_hover_ = pressed_ != 0 || IsIconic(hwnd_);
    if (countdown_hover_) countdown_base_ = now;
    if (now - countdown_base_ >= kAutoCloseMs) {
        BeginAutoClose();
        return;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void FileOperationWindow::BeginAutoClose() {
    closing_ = true;
    closing_tick_ = GetTickCount64();
    countdown_base_ = 0;
    // The DWM backdrop does not follow the DComp zoom: drop it and paint the
    // card opaque first so it leaves as one piece.
    const DWORD none = DWMSBT_NONE;
    DwmSetWindowAttribute(hwnd_, DWMWA_SYSTEMBACKDROP_TYPE, &none, sizeof(none));
    // Without the backdrop, the frame extended over the client area is an
    // opaque DWM sheet that stayed behind the shrinking card as a blank
    // window. Without it the window is clear where the card is not.
    const MARGINS flat{ 0, 0, 0, 0 };
    DwmExtendFrameIntoClientArea(hwnd_, &flat);
    backdrop_enabled_ = false;
    pulse::ui::SuppressWindowShadow(hwnd_);  // Windows 10: it would stay full size
    Render();
    SetTimer(hwnd_, kCloseTimer, static_cast<UINT>(kCloseAnimMs), nullptr);
    // Toward the bottom-right, where the status pill sits.
    if (!compositor_.PlayZoom(static_cast<float>(compositor_.Width()),
                              static_cast<float>(compositor_.Height()), false,
                              static_cast<double>(kCloseAnimMs) / 1000.0))
        FinishAutoClose();
}

void FileOperationWindow::FinishAutoClose() {
    if (hwnd_) KillTimer(hwnd_, kCloseTimer);
    if (!closing_) return;
    closing_ = false;
    countdown_base_ = 0;
    if (callbacks_.dismiss) callbacks_.dismiss();
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
    compositor_.ClearZoom();
    ApplyWindowTheme();
}

// A click, key or drag on the finished card: the user is looking at it, so
// it stays until they close it.
void FileOperationWindow::KeepOpen() {
    if (closing_ || kept_ || status_.active) return;
    if (status_.phase != ops::OpPhase::Completed && status_.phase != ops::OpPhase::Failed) return;
    kept_ = true;
    finish_unseen_ = false;
    if (countdown_base_ == 0) return;
    countdown_base_ = 0;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void FileOperationWindow::SampleGraph(ULONGLONG now) {
    if (!graph_.Due(now)) return;
    if (status_.phase != ops::OpPhase::Running || ShowsPaused(status_)) {
        graph_.last_tick = now;
        return;
    }
    if (status_.speed_basis == ops::OpSpeedBasis::None) return;
    // Sample whichever unit this task tracks: byte transfers and
    // empty-recycle feed the byte rate, delete and restore the item rate.
    graph_.Sample(now, status_.percent, status_.speed_basis == ops::OpSpeedBasis::Items
        ? status_.items_per_second : status_.bytes_per_second);
}

// The engine samples every run in the background; while the card was not
// on screen it holds the only record, so take it whenever it knows more.
void FileOperationWindow::SyncGraph() {
    if (!callbacks_.speed_history || status_.task_id == 0) return;
    auto history = callbacks_.speed_history(status_.task_id);
    if (history.points.empty()) return;
    if (history.by_progress == graph_.by_progress && !graph_.points.empty() &&
        graph_.by_progress && history.points.back().x < graph_.points.back().x) return;
    if (history.by_progress == graph_.by_progress && !graph_.by_progress &&
        history.points.size() <= graph_.points.size()) return;
    const ULONGLONG tick = graph_.last_tick;
    graph_ = std::move(history);
    if (tick > graph_.last_tick) graph_.last_tick = tick;
}

void FileOperationWindow::Render() {
    if (compositor_.NeedsRecovery()) {
        if (!compositor_.Recover()) return;
        compositor_.RecreateTextFormats(scale_);
        painter_.SetCompositor(&compositor_);
        painter_.SetScale(scale_);
        CreateCardFormats();
    }
    auto* dc = compositor_.Dc();
    if (!dc) return;
    if (!pct_format_.get()) CreateCardFormats();
    const bool high_contrast = IsHighContrast();
    const Theme theme = high_contrast ? MakeHighContrastTheme() : MakeTheme(dark_, accent_);
    pulse::ui::BeginSurface(compositor_, painter_, theme, dark_, high_contrast, backdrop_enabled_,
                 scale_);
    const float s = scale_;
    const float width = static_cast<float>(compositor_.Width());
    const float height = static_cast<float>(compositor_.Height());
    const float dip_w = width / std::max(s, 0.001f);
    const float content_w = dip_w - kPadX * 2.0f;
    auto* factory = compositor_.DwriteFactory();
    const auto labels = MakeTransferLabels(status_, detailed_, FailureView());
    const auto chrome = MakeTransferChrome(s, width, height, painter_, labels);
    const ULONGLONG now = GetTickCount64();

    const bool authorization = status_.authorization != ops::AuthorizationState::None;
    const bool requesting = status_.authorization == ops::AuthorizationState::Requesting;
    const bool failed = status_.phase == ops::OpPhase::Failed;
    const bool paused = ShowsPaused(status_);
    const bool pausing = PauseInFlight(status_);
    const bool waiting = status_.phase == ops::OpPhase::WaitingForConflict;
    const bool scanning = status_.phase == ops::OpPhase::Scanning;
    const bool completed = status_.phase == ops::OpPhase::Completed && !failed;
    const bool cancelling = status_.phase == ops::OpPhase::Cancelling;
    // Partial failure: the rest went through, the failed items are listed.
    const bool partial = failed && board_.has_value();
    const bool moving = status_.type == ops::OpType::Move;
    const bool deleting = status_.type == ops::OpType::RecycleDelete
                       || status_.type == ops::OpType::RealDelete;
    const uint64_t partial_failed = partial ? status_.failed_items : 0;
    // A delete counts what it removed: items it skipped are neither.
    const uint64_t partial_done = deleting ? status_.completed_items
        : status_.total_items > partial_failed ? status_.total_items - partial_failed : 0;
    const bool restoring = status_.type == ops::OpType::RestoreRecycle;
    const bool emptying = status_.type == ops::OpType::EmptyRecycle;
    // 新建 and 重命名 go through the same window (opened from the status pill)
    // and must not read as a copy.
    const bool creating = status_.type == ops::OpType::CreateFolder
                       || status_.type == ops::OpType::CreateTextFile;
    const bool renaming = status_.type == ops::OpType::Rename
                       || status_.type == ops::OpType::BatchRename;
    const bool byte_basis = status_.speed_basis == ops::OpSpeedBasis::Bytes;
    const ops::OpSpeedBasis shown_basis = status_.speed_basis == ops::OpSpeedBasis::None
        ? ops::OpSpeedBasis::Bytes : status_.speed_basis;

    // Colours (Pulse_文件操作对话框_重设计.html): the accent carries the
    // progress; paused, waiting and failed turn the graph grey. Status colours
    // show only in the title icon and the graph's end dot.
    auto mix = [](D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
        return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a);
    };
    const D2D1_COLOR_F accent = high_contrast ? theme.accent
        : dark_ ? mix(theme.accent, HexColor(0xFFFFFF), 0.28f) : theme.accent;
    const D2D1_COLOR_F link = high_contrast ? theme.accent
        : dark_ ? mix(theme.accent, HexColor(0xFFFFFF), 0.45f) : mix(theme.accent, HexColor(0x000000), 0.2f);
    const D2D1_COLOR_F text3 = WithAlpha(theme.text, dark_ ? 0.544f : 0.446f);
    const D2D1_COLOR_F ok_color = high_contrast ? theme.accent : dark_ ? HexColor(0x6CCB5F) : HexColor(0x0F7B0F);
    const D2D1_COLOR_F warn_color = high_contrast ? theme.accent : dark_ ? HexColor(0xFCE100) : HexColor(0x9D5D00);
    const D2D1_COLOR_F bad_color = high_contrast ? theme.danger : dark_ ? HexColor(0xFF99A4) : HexColor(0xC42B1C);
    const D2D1_COLOR_F track = high_contrast ? WithAlpha(theme.text, 0.12f)
        : dark_ ? HexColor(0xFFFFFF, 0.06f) : HexColor(0x000000, 0.045f);
    const D2D1_COLOR_F stroke = high_contrast ? theme.text
        : dark_ ? HexColor(0xFFFFFF, 0.0698f) : HexColor(0x000000, 0.0578f);
    const float fraction = status_.percent < 0.0f ? 0.0f
        : std::clamp(status_.percent / 100.0f, 0.0f, 1.0f);
    const bool list_mode = board_.has_value() && !authorization;

    const uint64_t item_count = status_.total_items > 0 ? status_.total_items
        : (std::max)(status_.completed_items, static_cast<uint64_t>(1));
    const std::wstring items_suffix = l10n::Get(l10n::StringId::OpItemsSuffix);
    std::wstring title;
    if (emptying) {
        if (failed) title = l10n::Get(l10n::StringId::OpCannotEmpty);
        else if (completed) title = l10n::Get(l10n::StringId::OpEmptied);
        else title = l10n::Get(l10n::StringId::OpEmptying);
    } else if (failed && (creating || renaming)) {
        title = l10n::Get(renaming ? l10n::StringId::CannotRename
            : status_.type == ops::OpType::CreateFolder ? l10n::StringId::CannotCreateFolder
            : l10n::StringId::CannotCreateFile);
    } else if (completed && (creating || renaming)) {
        title = l10n::Get(creating ? l10n::StringId::OpCreatedPrefix : l10n::StringId::OpRenamedPrefix) +
            pulse::format::GroupedInt((std::max)(status_.completed_items, static_cast<uint64_t>(1))) +
            items_suffix;
    } else if (!paused && !scanning && (creating || renaming)) {
        title = l10n::Get(creating ? l10n::StringId::OpCreatingPrefix : l10n::StringId::OpRenamingPrefix) +
            pulse::format::GroupedInt(item_count) + items_suffix;
    } else if (partial) {
        title = FormatCounts(deleting ? l10n::StringId::OpDeletePartialTitle
                             : moving ? l10n::StringId::OpMovePartialTitle : l10n::StringId::OpCopyPartialTitle,
                             partial_done, partial_failed);
    } else if (failed) {
        title = l10n::Get(deleting ? l10n::StringId::OpDeleteFailed : restoring ? l10n::StringId::OpRestoreFailed
            : moving ? l10n::StringId::OpMoveFailed : l10n::StringId::OpCopyFailed);
    } else if (completed) {
        title = l10n::Get(deleting ? l10n::StringId::OpDeletedPrefix : restoring ? l10n::StringId::OpRestoredPrefix
            : moving ? l10n::StringId::OpMovedPrefix : l10n::StringId::OpCopiedPrefix)
            + pulse::format::GroupedInt(status_.completed_items) + items_suffix;
    } else if (authorization) {
        // Pulse_文件操作对话框_重设计.html 1j: the title says what needs it.
        title = l10n::Get(status_.type == ops::OpType::Copy ? l10n::StringId::OpAuthTitleCopy
            : moving ? l10n::StringId::OpAuthTitleMove
            : deleting ? l10n::StringId::OpAuthTitleDelete
                       : l10n::StringId::OpAuthorizationRequired);
    } else if (paused && !pausing) {
        title = l10n::Get(deleting ? l10n::StringId::OpDeletePaused : restoring ? l10n::StringId::OpRestorePaused
            : moving ? l10n::StringId::OpMovePaused : l10n::StringId::OpCopyPaused);
    } else if (scanning) {
        title = l10n::Get(l10n::StringId::OpPreparingList);
    } else {
        title = l10n::Get(deleting ? l10n::StringId::OpDeletingPrefix : restoring ? l10n::StringId::OpRestoringPrefix
            : moving ? l10n::StringId::OpMovingPrefix : l10n::StringId::OpCopyingPrefix)
            + pulse::format::GroupedInt(item_count) + items_suffix;
    }
    // Title, with a status icon once the task has ended.
    StatusIcon title_icon = StatusIcon::None;
    D2D1_COLOR_F icon_color = ok_color;
    if (authorization) title_icon = StatusIcon::None;
    else if (partial) { title_icon = StatusIcon::Warning; icon_color = warn_color; }
    else if (failed) { title_icon = StatusIcon::Error; icon_color = bad_color; }
    else if (completed) title_icon = StatusIcon::Ok;
    const float title_x = title_icon == StatusIcon::None ? kPadX : kPadX + 24.0f;
    const float title_right = chrome.minimize.left / s - 8.0f;
    if (title_icon != StatusIcon::None)
        DrawStatusIcon(dc, title_icon, kPadX, 12.0f, s, icon_color, dark_ && !high_contrast);
    DrawEllipsizedText(compositor_, title_format_.get(), Rect(s, title_x, 8, title_right - title_x, 24),
                       title, theme.text);

    // Route: one line of text, source → destination.
    const std::wstring src = status_.source_label.empty()
        ? l10n::Get(l10n::StringId::OpSourceShort) : status_.source_label;
    const bool two_ends = !status_.destination_label.empty() && !deleting && !emptying && !restoring
                       && !creating && !renaming;
    {
        const float y = 34.0f, h = 16.0f;
        if (emptying) {
            DrawEllipsizedText(compositor_, label_format_.get(), Rect(s, kPadX, y, content_w, h),
                               l10n::Get(l10n::StringId::OpEmptyingSub), theme.text_secondary);
        } else if (two_ends) {
            const std::wstring arrow = L"\x2192";
            const float arrow_w = TextWidth(factory, label_format_.get(), arrow) / s + 8.0f;
            float a = TextWidth(factory, label_format_.get(), src) / s + 1.0f;
            float b = TextWidth(factory, label_format_.get(), status_.destination_label) / s + 1.0f;
            const float avail = content_w - arrow_w;
            if (a + b > avail) {
                const float half = avail * 0.5f;
                if (a < half) b = avail - a;
                else if (b < half) a = avail - b;
                else { a = half; b = half; }
            }
            DrawEllipsizedText(compositor_, label_format_.get(), Rect(s, kPadX, y, a, h), src, theme.text);
            painter_.DrawText(arrow, Rect(s, kPadX + a, y, arrow_w, h), label_format_.get(), text3,
                              fluent::HorizontalAlignment::Center);
            DrawEllipsizedText(compositor_, label_format_.get(), Rect(s, kPadX + a + arrow_w, y, b, h),
                               status_.destination_label, theme.text);
        } else {
            DrawEllipsizedText(compositor_, label_format_.get(), Rect(s, kPadX, y, content_w, h), src, theme.text);
        }
    }

    // Title-bar buttons. The auto-close countdown drains under 关闭 instead.
    fluent::ControlState minimize{};
    minimize.hovered = hover_ == 2;
    minimize.pressed = pressed_ == 2;
    painter_.DrawTitleBarButton(chrome.minimize, fluent::TitleBarButtonRole::Minimize, {}, minimize);
    fluent::ControlState close{};
    close.hovered = hover_ == 1;
    close.pressed = pressed_ == 1;
    painter_.DrawTitleBarButton(chrome.close, fluent::TitleBarButtonRole::Close, {}, close);
    const bool counting = AutoClosePending() || closing_;

    const double live_speed = status_.speed_basis == ops::OpSpeedBasis::Items
        ? status_.items_per_second : status_.bytes_per_second;
    const double peak_speed = status_.speed_basis == ops::OpSpeedBasis::Items
        ? status_.peak_items_per_second : status_.peak_bytes_per_second;
    const ULONGLONG end_tick = task_end_tick_ != 0 ? task_end_tick_ : now;
    const uint64_t elapsed_ms = task_start_tick_ != 0 && end_tick > task_start_tick_
        ? end_tick - task_start_tick_ : 0;
    const double elapsed_s = static_cast<double>(elapsed_ms) / 1000.0;
    const double done_amount = byte_basis ? static_cast<double>(status_.transferred_bytes)
                                          : static_cast<double>(status_.completed_items);
    const double average = elapsed_s >= 0.25 ? done_amount / elapsed_s : 0.0;

    // The engine's first estimates come from a cold start (the file cache, a
    // slow first file) and can read hours for a copy of seconds. Hold 正在估算
    // for the first 3 s, and while the estimate is far beyond what the
    // current rate would take.
    const uint64_t left_bytes = status_.total_bytes > status_.transferred_bytes
        ? status_.total_bytes - status_.transferred_bytes : 0;
    const double rate_eta = byte_basis && status_.bytes_per_second > 0.0
        ? static_cast<double>(left_bytes) / status_.bytes_per_second : 0.0;
    const bool eta_unsettled = elapsed_ms < 3000 ||
        (rate_eta > 0.0 && static_cast<double>(status_.eta_seconds) > rate_eta * 4.0 + 30.0);
    const std::wstring time_text = partial ? std::wstring(L"\x2014")
        : failed ? l10n::Get(l10n::StringId::OpIncomplete)
        : completed ? l10n::Get(l10n::StringId::OpCompleted)
        : paused ? l10n::Get(l10n::StringId::OpPaused)
        : waiting ? l10n::Get(l10n::StringId::OpWaiting)
        : emptying ? l10n::Get(l10n::StringId::OpEmptying)
        : status_.speed_basis == ops::OpSpeedBasis::None ? l10n::Get(l10n::StringId::OpNoEstimate)
        : status_.eta_seconds == 0 || eta_unsettled ? l10n::Get(l10n::StringId::OpEstimating)
        : l10n::Get(l10n::StringId::OpApprox) + FormatDuration(status_.eta_seconds);
    const uint64_t remain_items = status_.total_items > status_.completed_items
        ? status_.total_items - status_.completed_items : 0;
    std::wstring remain_text;
    if (status_.total_items > 0) {
        if (byte_basis && status_.total_bytes > 0)
            remain_text = pulse::format::GroupedInt(remain_items) + l10n::Get(l10n::StringId::OpCountBytes)
                + pulse::format::ByteSize(left_bytes) + L")";
        else remain_text = pulse::format::GroupedInt(remain_items) + l10n::Get(l10n::StringId::OpCountSuffix);
    }
    const std::wstring remain_line = remain_text.empty() ? std::wstring()
        : l10n::Get(l10n::StringId::OpRemainingShort) + L" " + remain_text;
    const std::wstring dot = L"  \x00B7  ";
    const std::wstring took = l10n::Get(l10n::StringId::OpTookLead) + FormatElapsed(elapsed_ms / 1000);

    const float hero_y = 62.0f;
    const float graph_top = 100.0f;
    const float graph_h = 56.0f;
    const float rows_top = 166.0f;
    const float row_h = 18.0f;
    const bool indeterminate = scanning || status_.percent < 0.0f;

    if (authorization) {
        // Permission view (HTML 1j): the shield beside a two-line message in
        // place of the numbers and the graph. The first line says what is
        // needed or what went wrong; the second, while Windows asks, that it
        // asks once for the whole operation, otherwise which item is waiting.
        DrawStatusIcon(dc, StatusIcon::Shield, kPadX, hero_y + 2.0f, s, warn_color, false);
        const std::wstring explanation = requesting
            ? l10n::Get(l10n::StringId::OpAuthorizationPrompt)
            : status_.last_error.empty() ? l10n::Get(l10n::StringId::OpAuthNotGrantedShort)
                                        : l10n::ServiceErrorText(status_.last_error);
        DrawEllipsizedText(compositor_, chip_format_.get(),
            Rect(s, kPadX + 24, hero_y, content_w - 24, 20), explanation, theme.text);
        const std::wstring detail = requesting
            ? l10n::Get(l10n::StringId::OpAuthorizationOnce)
            : l10n::Get(l10n::StringId::OpItemPrefix) + (status_.current_item.empty() ? src : status_.current_item);
        DrawEllipsizedText(compositor_, label_format_.get(),
            Rect(s, kPadX + 24, hero_y + 22, content_w - 24, row_h), detail, theme.text_secondary);
        if (requesting && !status_.current_item.empty())
            DrawEllipsizedText(compositor_, label_format_.get(),
                Rect(s, kPadX + 24, hero_y + 22 + row_h, content_w - 24, row_h),
                l10n::Get(l10n::StringId::OpItemPrefix) + status_.current_item, text3);
    } else if (!list_mode) {
        // --- Hero: percent on the left, speed or state on the right --------
        const D2D1_RECT_F right_box = Rect(s, dip_w - kPadX - 220.0f, hero_y + 10.0f, 220.0f, 20.0f);
        if (indeterminate) {
            DrawEllipsizedText(compositor_, value_format_.get(), Rect(s, kPadX, hero_y + 10.0f, content_w * 0.55f, 20.0f),
                               l10n::Get(scanning ? l10n::StringId::OpScanning : l10n::StringId::OpPreparing),
                               theme.text_secondary);
            if (status_.total_items > 0) {
                std::wstring found = pulse::format::GroupedInt(status_.total_items);
                found += byte_basis && status_.total_bytes > 0
                    ? l10n::Get(l10n::StringId::OpCountBytes) + pulse::format::ByteSize(status_.total_bytes) + L")"
                    : l10n::Get(l10n::StringId::OpCountSuffix);
                painter_.DrawText(found, right_box, value_format_.get(), theme.text_secondary,
                                  fluent::HorizontalAlignment::Right);
            }
        } else {
            const bool muted = failed || paused || cancelling;
            const int whole = static_cast<int>(std::floor(fraction * 100.0f + (completed ? 0.5f : 0.0f)));
            const std::wstring pct = std::to_wstring(completed ? 100 : whole);
            const float pct_w = TextWidth(factory, pct_format_.get(), pct) / s;
            painter_.DrawText(pct, Rect(s, kPadX - 1, hero_y, pct_w + 4, 32), pct_format_.get(),
                              muted ? theme.text_secondary : theme.text);
            painter_.DrawText(L"%", Rect(s, kPadX + pct_w - 1, hero_y + 7, 20, 24), unit_format_.get(),
                              theme.text_secondary);
        }
        if (!indeterminate) {
            std::wstring state;
            if (completed) {
                if (average > 0.0) {
                    const std::wstring speed = FormatSpeedText(shown_basis, average);
                    const float speed_w = TextWidth(factory, speed_format_.get(), speed) / s;
                    painter_.DrawText(speed, right_box, speed_format_.get(), theme.text,
                                      fluent::HorizontalAlignment::Right);
                    painter_.DrawText(l10n::Get(l10n::StringId::OpAverageLead),
                                      Rect(s, dip_w - kPadX - 220.0f, hero_y + 10.0f, 220.0f - speed_w - 6.0f, 20.0f),
                                      label_format_.get(), theme.text_secondary, fluent::HorizontalAlignment::Right);
                }
            } else if (failed) state = l10n::Get(l10n::StringId::OpIncomplete);
            else if (cancelling) state = l10n::Get(l10n::StringId::OpCancelling);
            else if (pausing) state = l10n::Get(l10n::StringId::OpPausing);
            else if (paused) state = l10n::Get(l10n::StringId::OpPaused);
            else if (waiting) state = l10n::Get(l10n::StringId::OpWaiting);
            else if (status_.speed_basis == ops::OpSpeedBasis::None) state.clear();
            else if (live_speed <= 0.0) state = l10n::Get(l10n::StringId::OpEstimating);
            else painter_.DrawText(FormatSpeedText(shown_basis, live_speed), right_box, speed_format_.get(),
                                   theme.text, fluent::HorizontalAlignment::Right);
            if (!state.empty())
                painter_.DrawText(state, right_box, value_format_.get(), theme.text_secondary,
                                  fluent::HorizontalAlignment::Right);
        }

        // --- Speed graph: a flat track, the area up to the progress ---------
        const auto plot = Rect(s, kPadX, graph_top, content_w, graph_h);
        const float radius = ScaleDip(s, 4.0f);
        painter_.FillRoundedRect(plot, radius, track);
        const bool clipped = PushRoundedClip(dc, plot, radius);
        if (indeterminate) {
            // A short bar slides along the bottom edge.
            const float t = static_cast<float>(now % 1600) / 1600.0f;
            const float eased = t * t * (3.0f - 2.0f * t);
            const float bar_w = content_w * 0.3f;
            const float x = -bar_w + (content_w + bar_w) * eased;
            painter_.FillRoundedRect(Rect(s, kPadX + x, graph_top + graph_h - 3.0f, bar_w, 3.0f),
                                     ScaleDip(s, 1.5f), accent);
        } else {
            double max_value = 0.0;
            for (const auto& point : graph_.points) max_value = (std::max)(max_value, point.v);
            if (graph_.points.size() >= 2 && max_value > 0.0) {
                const float plot_w = plot.right - plot.left;
                const float top = plot.top + ScaleDip(s, 8.0f);
                const float bottom = plot.bottom - ScaleDip(s, 6.0f);
                const double ceiling = max_value * 1.08;
                std::vector<D2D1_POINT_2F> points;
                points.reserve(graph_.points.size());
                const size_t span = (std::max)(graph_.points.size(), kTimeGraphPoints) - 1;
                for (size_t i = 0; i < graph_.points.size(); ++i) {
                    const float x = graph_.by_progress ? plot.left + plot_w * graph_.points[i].x
                        : plot.left + plot_w * static_cast<float>(i) / static_cast<float>(span);
                    const float y = bottom - (bottom - top) * static_cast<float>(graph_.points[i].v / ceiling);
                    points.push_back(D2D1::Point2F(x, y));
                }
                const bool grey = paused || waiting || failed || cancelling;
                const D2D1_COLOR_F line = grey ? text3 : accent;
                const D2D1_COLOR_F fill = WithAlpha(grey ? theme.text : accent,
                                                    grey ? 0.10f : (dark_ ? 0.20f : 0.13f));
                DrawSpeedArea(dc, plot, points, line, fill, s);
                ComPtr<ID2D1SolidColorBrush> dot_brush;
                if (!completed && SUCCEEDED(dc->CreateSolidColorBrush(failed ? bad_color : line, &dot_brush)))
                    dc->FillEllipse(D2D1::Ellipse(points.back(), ScaleDip(s, 3.0f), ScaleDip(s, 3.0f)),
                                    dot_brush.get());
            }
        }
        if (clipped) dc->PopLayer();

        // --- Rows under the graph -------------------------------------------
        if (failed) {
            // pulse_shell errors are Simplified; known messages are localized and
            // file names are kept as they are on disk.
            const std::wstring error = status_.last_error.empty() ? l10n::Get(l10n::StringId::OpFailed)
                                                                  : l10n::ServiceErrorText(status_.last_error);
            DrawEllipsizedText(compositor_, chip_format_.get(), Rect(s, kPadX, rows_top + 2, content_w, row_h),
                               error, theme.text);
            if (!remain_line.empty())
                DrawEllipsizedText(compositor_, label_format_.get(),
                                   Rect(s, kPadX, rows_top + 2 + row_h, content_w, row_h), remain_line,
                                   theme.text_secondary);
        } else if (completed) {
            std::wstring line = took;
            if (byte_basis && status_.total_bytes > 0) line += dot + pulse::format::ByteSize(status_.total_bytes);
            DrawEllipsizedText(compositor_, label_format_.get(), Rect(s, kPadX, rows_top, content_w, row_h),
                               line, theme.text_secondary);
        } else {
            std::wstring item_line;
            if (emptying && status_.current_item.empty()) item_line = l10n::Get(l10n::StringId::OpEmptyingSub);
            else item_line = status_.current_item.empty() ? src : status_.current_item;
            std::wstring counter;
            if (status_.total_items > 0 && !scanning)
                counter = pulse::format::GroupedInt(status_.completed_items) + L" / " +
                          pulse::format::GroupedInt(status_.total_items);
            const float counter_w = counter.empty() ? 0.0f
                : TextWidth(factory, label_format_.get(), counter) / s + 8.0f;
            DrawEllipsizedText(compositor_, label_format_.get(),
                               Rect(s, kPadX, rows_top, content_w - counter_w, row_h), item_line,
                               scanning ? theme.text_secondary : theme.text);
            if (!counter.empty())
                painter_.DrawText(counter, Rect(s, dip_w - kPadX - counter_w, rows_top, counter_w, row_h),
                                  label_format_.get(), text3, fluent::HorizontalAlignment::Right);
            std::wstring second;
            if (!scanning && !remain_line.empty()) {
                second = remain_line;
                if (!paused && !waiting && status_.speed_basis != ops::OpSpeedBasis::None) second += dot + time_text;
            }
            if (!second.empty())
                DrawEllipsizedText(compositor_, label_format_.get(),
                                   Rect(s, kPadX, rows_top + row_h, content_w, row_h), second, theme.text_secondary);
        }
    }

    // --- Failed items (partial failure) --------------------------------------
    const float fail_block = FailureBlockDip();
    failure_buttons_.clear();
    if (list_mode) {
        std::wstring summary = took;
        if (average > 0.0) summary += dot + l10n::Get(l10n::StringId::OpAverageLead) + FormatSpeedText(shown_basis, average);
        DrawEllipsizedText(compositor_, label_format_.get(), Rect(s, kPadX, hero_y, content_w, row_h),
                           summary, theme.text_secondary);

        const float list_top = kFailListTop;
        const size_t visible = VisibleFailureRows();
        board_scroll_ = (std::min)(board_scroll_, board_->rows.size() - visible);
        const float list_h = static_cast<float>(visible) * kFailRowH +
                             (board_->unlisted > 0 ? kFailMoreH : 0.0f);
        const auto box = Rect(s, kPadX, list_top, content_w, list_h + 2.0f);
        painter_.StrokeRoundedRect(box, ScaleDip(s, 6.0f), stroke);
        const std::wstring retry_label = l10n::Get(l10n::StringId::RecoveryRetry);
        const std::wstring retrying_label = l10n::Get(l10n::StringId::Retrying);
        const std::wstring done_label = l10n::Get(l10n::StringId::OpCompleted);
        const float retry_w = TextWidth(factory, label_format_.get(), retry_label) / s + 16.0f;
        const float rows_y = list_top + 1.0f;
        for (size_t v = 0; v < visible; ++v) {
            const size_t index = board_scroll_ + v;
            if (index >= board_->rows.size()) break;
            const FailureRow& row = board_->rows[index];
            const float y = rows_y + kFailRowH * static_cast<float>(v);
            if (v > 0) {
                const float line = ScaleDip(s, y);
                painter_.FillRoundedRect(D2D1::RectF(box.left + 1.0f, line, box.right - 1.0f, line + 1.0f), 0,
                                         theme.stroke_divider);
            }
            const bool done = row.state == FailureRow::State::Done;
            painter_.DrawGlyphWithFormat(L"\xE8A5", Rect(s, kPadX + 8, y, 18, kFailRowH), theme.text_secondary,
                                         glyph_format_.get());
            const std::wstring name = LeafName(row.failure.source);
            const float name_w = (std::min)(170.0f, TextWidth(factory, chip_format_.get(), name) / s + 2.0f);
            DrawEllipsizedText(compositor_, chip_format_.get(), Rect(s, kPadX + 34, y, name_w, kFailRowH),
                               name, theme.text);
            float action_left = kPadX + content_w - 6.0f;
            if (row.state == FailureRow::State::Failed && callbacks_.retry_failed) {
                const auto button = Rect(s, action_left - retry_w, y + (kFailRowH - 24.0f) * 0.5f, retry_w, 24.0f);
                failure_buttons_.push_back({ v, button });
                const bool hot = hover_ == kHitFailRow + static_cast<int>(v);
                const bool down = pressed_ == kHitFailRow + static_cast<int>(v);
                if (hot || down)
                    painter_.FillRoundedRect(button, ScaleDip(s, 4.0f),
                                             WithAlpha(theme.text, down ? 0.06f : (dark_ ? 0.06f : 0.037f)));
                painter_.DrawText(retry_label, button, label_format_.get(), link,
                                  fluent::HorizontalAlignment::Center);
                action_left -= retry_w + 4.0f;
            } else if (row.state == FailureRow::State::Retrying || done) {
                const std::wstring& label = done ? done_label : retrying_label;
                const float w = TextWidth(factory, label_format_.get(), label) / s + 2.0f;
                const D2D1_COLOR_F color = done ? ok_color : theme.text_secondary;
                const float right = action_left - 2.0f;
                painter_.DrawText(label, Rect(s, right - w, y, w, kFailRowH), label_format_.get(), color,
                                  fluent::HorizontalAlignment::Right);
                if (done)
                    painter_.DrawGlyphWithFormat(L"\xE73E", Rect(s, right - w - 18.0f, y, 14, kFailRowH), color,
                                                 glyph_format_.get());
                action_left = right - w - (done ? 22.0f : 4.0f);
            }
            const float reason_x = kPadX + 34.0f + name_w + 10.0f;
            if (action_left > reason_x + 8.0f)
                DrawEllipsizedText(compositor_, label_format_.get(),
                                   Rect(s, reason_x, y, action_left - reason_x - 6.0f, kFailRowH),
                                   row.failure.reason, theme.text_secondary);
        }
        if (board_->unlisted > 0) {
            const float y = rows_y + kFailRowH * static_cast<float>(visible);
            const float line = ScaleDip(s, y);
            painter_.FillRoundedRect(D2D1::RectF(box.left + 1.0f, line, box.right - 1.0f, line + 1.0f), 0,
                                     theme.stroke_divider);
            DrawEllipsizedText(compositor_, label_format_.get(), Rect(s, kPadX + 10, y, content_w - 20, kFailMoreH),
                               FormatCounts(l10n::StringId::OpMoreFailures, board_->unlisted), text3);
        }
        if (board_->rows.size() > visible && visible > 0) {
            // Wheel-scrolled: a thin thumb shows where the view is.
            const float track_h = static_cast<float>(visible) * kFailRowH - 8.0f;
            const float thumb = (std::max)(16.0f, track_h * static_cast<float>(visible) /
                                                      static_cast<float>(board_->rows.size()));
            const float at = rows_y + 4.0f + (track_h - thumb) * static_cast<float>(board_scroll_) /
                             static_cast<float>(board_->rows.size() - visible);
            painter_.FillRoundedRect(Rect(s, kPadX + content_w - 6.0f, at, 2.0f, thumb), ScaleDip(s, 1.0f),
                                     WithAlpha(theme.text, 0.25f));
        }
    }

    // --- Details: a two-column label / value list ------------------------------
    if (detailed_ && !authorization) {
        const float top = kContentH + fail_block;
        painter_.FillRoundedRect(D2D1::RectF(ScaleDip(s, kPadX), ScaleDip(s, top),
                                             ScaleDip(s, kPadX + content_w), ScaleDip(s, top) + 1.0f),
                                 0, theme.stroke_divider);
        const std::wstring detail_labels[6] = {
            l10n::Get(l10n::StringId::OpProcessedItems), l10n::Get(l10n::StringId::OpTransferredBytes),
            l10n::Get(l10n::StringId::OpAverageSpeed), l10n::Get(l10n::StringId::OpPeakSpeed),
            l10n::Get(l10n::StringId::OpElapsed), l10n::Get(l10n::StringId::OpRemainingTime) };
        const std::wstring detail_values[6] = {
            status_.total_items > 0
                ? pulse::format::GroupedInt(status_.completed_items) + L" / " + pulse::format::GroupedInt(status_.total_items)
                : pulse::format::GroupedInt(status_.completed_items),
            byte_basis && status_.total_bytes > 0
                ? pulse::format::ByteSize(status_.transferred_bytes) + L" / " + pulse::format::ByteSize(status_.total_bytes)
                : std::wstring(L"\x2014"),
            average > 0.0 ? FormatSpeedText(shown_basis, average) : std::wstring(L"\x2014"),
            peak_speed > 0.0 ? FormatSpeedText(shown_basis, peak_speed) : std::wstring(L"\x2014"),
            FormatElapsed(elapsed_ms / 1000), time_text };
        const float col_w = (content_w - 16.0f) * 0.5f;
        for (int i = 0; i < 6; ++i) {
            const float x = kPadX + (col_w + 16.0f) * static_cast<float>(i % 2);
            const float y = top + 13.0f + 44.0f * static_cast<float>(i / 2);
            DrawEllipsizedText(compositor_, label_format_.get(), Rect(s, x, y, col_w, 16), detail_labels[i],
                               theme.text_secondary);
            DrawEllipsizedText(compositor_, value_format_.get(), Rect(s, x, y + 16, col_w, 18), detail_values[i],
                               theme.text);
        }
    }

    // --- Footer -----------------------------------------------------------
    const float footer_top = height - ScaleDip(s, kFooterH);
    painter_.FillRoundedRect(D2D1::RectF(0, footer_top, width, height), 0,
        OpFooter(hwnd_, backdrop_enabled_, dark_));
    painter_.FillRoundedRect(D2D1::RectF(0, footer_top, width, footer_top + 1.0f), 0, theme.stroke_divider);

    fluent::ControlState detail_state{};
    detail_state.hovered = hover_ == 5;
    detail_state.pressed = pressed_ == 5;
    if (labels.show_details) painter_.DrawButton({ chrome.details, labels.details,
        detailed_ ? L"\xE70E" : L"\xE70D", fluent::ButtonKind::Transparent, detail_state });
    if (labels.show_pause) {
        fluent::ControlState pause_state{};
        pause_state.enabled = status_.active && !failed && !cancelling && !waiting;
        pause_state.hovered = hover_ == 3;
        pause_state.pressed = pressed_ == 3;
        painter_.DrawButton({ chrome.pause, labels.pause, labels.pause_glyph,
                              paused ? fluent::ButtonKind::Primary : fluent::ButtonKind::Standard,
                              pause_state });
    }
    if (labels.show_retry) {
        fluent::ControlState retry_state{};
        retry_state.hovered = hover_ == 6;
        retry_state.pressed = pressed_ == 6;
        retry_state.enabled = labels.actions_enabled;
        painter_.DrawButton({ chrome.retry, labels.retry, {},
                              fluent::ButtonKind::Primary, retry_state });
    }
    if (labels.show_skip) {
        fluent::ControlState skip_state{};
        skip_state.hovered = hover_ == 7;
        skip_state.pressed = pressed_ == 7;
        skip_state.enabled = labels.actions_enabled;
        painter_.DrawButton({ chrome.skip, labels.skip, {},
                              fluent::ButtonKind::Transparent, skip_state });
    }
    if (labels.show_open) {
        fluent::ControlState open_state{};
        open_state.hovered = hover_ == kHitOpen;
        open_state.pressed = pressed_ == kHitOpen;
        painter_.DrawButton({ chrome.open, labels.open, {}, fluent::ButtonKind::Standard, open_state });
    }
    fluent::ControlState cancel_state{};
    cancel_state.enabled = !cancelling;
    cancel_state.hovered = hover_ == 4;
    cancel_state.pressed = pressed_ == 4;
    painter_.DrawButton({ chrome.cancel, labels.cancel, {},
                          labels.cancel_primary ? fluent::ButtonKind::Primary : fluent::ButtonKind::Standard,
                          cancel_state });
    if (counting) {
        // Auto-close: a ring around × drains over kAutoCloseMs; a held button
        // or a minimized card keeps it full.
        const float left = closing_ || countdown_hover_ ? 1.0f
            : 1.0f - std::clamp(static_cast<float>(now - countdown_base_) / kAutoCloseMs, 0.0f, 1.0f);
        const D2D1_POINT_2F c = D2D1::Point2F((chrome.close.left + chrome.close.right) * 0.5f,
                                              (chrome.close.top + chrome.close.bottom) * 0.5f);
        const float r = ScaleDip(s, 11.0f);
        StrokeArc(dc, c, r, 0.0f, 360.0f, WithAlpha(ok_color, 0.18f), ScaleDip(s, 2.0f));
        StrokeArc(dc, c, r, 360.0f * (1.0f - left), 360.0f * left, ok_color, ScaleDip(s, 2.0f));
    }
    pulse::ui::EndSurface(compositor_);
}

LRESULT CALLBACK FileOperationWindow::WndProc(HWND hwnd, UINT message,
                                               WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<FileOperationWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<FileOperationWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    return self ? self->HandleMessage(message, wparam, lparam)
                : DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT FileOperationWindow::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE:
        scale_ = static_cast<float>(pulse::compat::WindowDpi(hwnd_)) / 96.0f;
        ApplyWindowTheme();
        if (!compositor_.Init(hwnd_)) return -1;
        compositor_.RecreateTextFormats(scale_);
        painter_.SetCompositor(&compositor_);
        painter_.SetScale(scale_);
        CreateCardFormats();
        // The window is created hidden and kept for reuse; its 33 ms
        // animation timer only runs while it is shown.
        return 0;
    case WM_SHOWWINDOW:
        if (wparam) SetTimer(hwnd_, kRenderTimer, 33, nullptr);
        else KillTimer(hwnd_, kRenderTimer);
        break;
    case WM_NCCALCSIZE:
        return 0;
    case WM_NCHITTEST: {
        const auto labels = MakeTransferLabels(status_, detailed_, FailureView());
        const auto chrome = MakeTransferChrome(scale_,
            static_cast<float>(compositor_.Width()), static_cast<float>(compositor_.Height()),
            painter_, labels);
        return pulse::ui::BorderlessHitTest(hwnd_, lparam, ScaleDip(scale_, kTitleH),
            D2D1::RectF(chrome.minimize.left, 0, chrome.close.right, chrome.close.bottom));
    }
    case WM_GETMINMAXINFO: {
        if (status_.authorization == ops::AuthorizationState::None) break;
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        info->ptMinTrackSize = { static_cast<LONG>(kDlgW * scale_),
                                 static_cast<LONG>(kAuthorizationH * scale_) };
        return 0;
    }
    case WM_SIZE:
        if (compositor_.Dc() && wparam != SIZE_MINIMIZED)
            compositor_.Resize(LOWORD(lparam), HIWORD(lparam));
        if (wparam == SIZE_RESTORED && finish_unseen_) {
            // Finished while minimized: count down now that it is seen.
            finish_unseen_ = false;
            if (auto_close_ && !kept_ && !closing_ && !status_.active &&
                status_.phase == ops::OpPhase::Completed && status_.last_error.empty()) {
                countdown_base_ = GetTickCount64();
                SetTimer(hwnd_, kRenderTimer, 33, nullptr);
            }
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_ENTERSIZEMOVE:
        KeepOpen();
        break;
    case WM_MOVE:
        compositor_.UpdateTextRenderingParams(
            MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST));
        return 0;
    case WM_DPICHANGED: {
        scale_ = HIWORD(wparam) / 96.0f;
        compositor_.RecreateTextFormats(scale_);
        painter_.SetScale(scale_);
        CreateCardFormats();
        const auto* suggested = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_TIMER:
        if (wparam == kRenderTimer && !IsWindowVisible(hwnd_)) {
            KillTimer(hwnd_, kRenderTimer);
            return 0;
        }
        if (wparam == kCloseTimer) {
            FinishAutoClose();
            return 0;
        }
        if (wparam == kRenderTimer && has_held_status_ && GetTickCount64() >= hold_until_) {
            const ops::OpStatus held = held_status_;
            Update(held);
        }
        if (wparam == kRenderTimer) {
            const ULONGLONG now = GetTickCount64();
            if (closing_ || AutoClosePending()) {
                TickAutoClose(now);
                return 0;
            }
            // Only a running operation animates (speed graph, live dot); a
            // finished one is redrawn by Update and input alone.
            if (!status_.active) {
                KillTimer(hwnd_, kRenderTimer);
                return 0;
            }
            SampleGraph(now);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_MOUSEMOVE: {
        const int next = HitTestControl(static_cast<float>(GET_X_LPARAM(lparam)),
                                        static_cast<float>(GET_Y_LPARAM(lparam)));
        if (next != hover_) { hover_ = next; InvalidateRect(hwnd_, nullptr, FALSE); }
        TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, hwnd_, 0 };
        TrackMouseEvent(&track);
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (board_ && board_->rows.size() > kFailMaxRows) {
            const short delta = GET_WHEEL_DELTA_WPARAM(wparam);
            const size_t last = board_->rows.size() - kFailMaxRows;
            if (delta > 0 && board_scroll_ > 0) --board_scroll_;
            else if (delta < 0 && board_scroll_ < last) ++board_scroll_;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_MOUSELEAVE:
        hover_ = 0;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_RBUTTONDOWN:
        KeepOpen();
        return 0;
    case WM_LBUTTONDOWN:
        pressed_ = HitTestControl(static_cast<float>(GET_X_LPARAM(lparam)),
                                  static_cast<float>(GET_Y_LPARAM(lparam)));
        // Closing (1) and minimizing (2) are not a reason to keep the card.
        if (pressed_ != 1 && pressed_ != 2) KeepOpen();
        SetCapture(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        const int hit = HitTestControl(static_cast<float>(GET_X_LPARAM(lparam)),
                                       static_cast<float>(GET_Y_LPARAM(lparam)));
        const int pressed = pressed_;
        pressed_ = 0;
        ReleaseCapture();
        if (hit == pressed) {
            const auto labels = MakeTransferLabels(status_, detailed_, FailureView());
            if (hit == 4 && labels.cancel_retries && board_) {
                std::vector<size_t> rows(board_->rows.size());
                for (size_t i = 0; i < rows.size(); ++i) rows[i] = i;
                RetryFailureRows(rows);
            } else if (hit == 1 || hit == 4) {
                RequestClose();
            } else if (hit == kHitOpen && callbacks_.open_folder && !status_.destination_dir.empty()) {
                callbacks_.open_folder(status_.destination_dir);
            } else if (hit >= kHitFailRow && board_) {
                RetryFailureRows({ board_scroll_ + static_cast<size_t>(hit - kHitFailRow) });
            } else if (hit == 2) ShowWindow(hwnd_, SW_MINIMIZE);
            else if (hit == 3 && status_.active &&
                     status_.phase != ops::OpPhase::WaitingForConflict &&
                     status_.phase != ops::OpPhase::Cancelling) {
                if (ShowsPaused(status_)) {
                    if (callbacks_.resume) callbacks_.resume();
                } else if (callbacks_.pause) callbacks_.pause();
            } else if ((hit == 6 || hit == 7) && status_.active &&
                       status_.authorization == ops::AuthorizationState::ActionRequired &&
                       status_.phase != ops::OpPhase::Cancelling) {
                if (hit == 6 && callbacks_.retry_authorization)
                    callbacks_.retry_authorization(status_.task_id);
                else if (hit == 7 && status_.can_skip_authorization && callbacks_.skip_authorization)
                    callbacks_.skip_authorization(status_.task_id);
            } else if (hit == 5) {
                detailed_ = !detailed_;
                ResizeForDetails(true);
            }
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) {
            RequestClose();
            return 0;
        }
        KeepOpen();
        break;
    case WM_CLOSE:
        RequestClose();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(hwnd_, &paint);
        Render();
        EndPaint(hwnd_, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        KillTimer(hwnd_, kRenderTimer);
        compositor_.Shutdown();
        hwnd_ = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd_, message, wparam, lparam);
}

ConflictDialogResult ShowFileConflictDialog(HWND owner,
                                            const ops::ConflictItemInfo& conflict,
                                            bool dark,
                                            D2D1_COLOR_F accent) {
    ConflictWindow window;
    return window.Show(owner, conflict, dark, accent);
}

PermanentDeleteChoice ShowPermanentDeleteDialog(HWND owner, const PermanentDeleteInfo& info,
                                                bool dark, D2D1_COLOR_F accent) {
    PermanentDeleteWindow window;
    return window.Show(owner, info, dark, accent);
}

LockedItemChoice ShowLockedItemDialog(HWND owner, const LockedItemInfo& info, bool dark,
                                      D2D1_COLOR_F accent) {
    LockedItemWindow window;
    return window.Show(owner, info, dark, accent);
}


} // namespace pulse::ui

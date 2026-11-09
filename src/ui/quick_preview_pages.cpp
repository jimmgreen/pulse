// Quick Look reader for paged documents (multi-page PDF / PDF-compatible AI).
//
// The preview host renders one page per request: the page number travels as
// the request's frame_index and the page count comes back as frame_count with
// a zero frame delay (see RunPdf in preview_decoders.cpp). Pages are laid out
// as one continuous vertical scroll at "fit width" times pages_zoom_, with an
// optional thumbnail strip on the left and a page indicator pill. Only pages
// near the viewport are requested; the thumbnail cache keeps them resident.
#include "quick_preview_window.h"
#include "../ipc/preview_protocol.h"
#include "../common/localization.h"
#include "../common/text_format.h"
#include <d2d1helper.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>

namespace pulse::ui {
namespace {
constexpr float kStripWidth = 104.0f;       // DIP
constexpr float kStripMinWindow = 560.0f;   // narrower windows hide the strip
constexpr float kStripPad = 14.0f;
constexpr float kStripLabel = 16.0f;
constexpr float kStripGap = 8.0f;
constexpr float kPageMargin = 16.0f;
constexpr float kPageGap = 12.0f;
constexpr float kMinZoom = 0.25f;
constexpr float kMaxZoom = 5.0f;
constexpr float kWheelStep = 100.0f;
constexpr uint32_t kThumbPixels = 160;      // strip thumbnails and pending-page stand-ins
constexpr uint32_t kMaxPagePixels = 3072;
constexpr float kDefaultAspect = 1.41421356f;  // A4 portrait until page 1 reports its size

bool Inside(const D2D1_RECT_F& rect, POINT point) {
    return point.x >= rect.left && point.x < rect.right &&
        point.y >= rect.top && point.y < rect.bottom;
}
}  // namespace

void QuickPreviewWindow::ResetPages() {
    pages_count_ = 0;
    page_aspect_.clear();
    pages_scroll_ = 0.0f;
    pages_pan_x_ = 0.0f;
    pages_zoom_ = 1.0f;
    pages_strip_scroll_ = 0.0f;
    pages_follow_ = UINT32_MAX;
}

QuickPreviewWindow::PagesLayout QuickPreviewWindow::ComputePagesLayout() const {
    PagesLayout layout;
    const D2D1_RECT_F content = ContentRect();
    layout.view = content;
    const float width = content.right - content.left;
    if (pages_strip_ && width >= kStripMinWindow * scale_) {
        layout.strip = D2D1::RectF(content.left, content.top,
                                   content.left + kStripWidth * scale_, content.bottom);
        layout.view.left = layout.strip.right;
    }
    layout.margin = (embedded_ ? 6.0f : kPageMargin) * scale_;
    layout.gap = kPageGap * scale_;
    const float fit = (std::max)(40.0f * scale_,
                                 layout.view.right - layout.view.left - layout.margin * 2.0f);
    layout.page_w = fit * pages_zoom_;
    return layout;
}

float QuickPreviewWindow::PageAspect(uint32_t page) const noexcept {
    if (page < page_aspect_.size() && page_aspect_[page] > 0.0f) return page_aspect_[page];
    if (!page_aspect_.empty() && page_aspect_[0] > 0.0f) return page_aspect_[0];
    return kDefaultAspect;
}

float QuickPreviewWindow::PageTop(const PagesLayout& layout, uint32_t page) const {
    float top = layout.margin;
    for (uint32_t i = 0; i < page && i < pages_count_; ++i)
        top += layout.page_w * PageAspect(i) + layout.gap;
    return top;
}

float QuickPreviewWindow::PagesDocHeight(const PagesLayout& layout) const {
    return PageTop(layout, pages_count_) - layout.gap + layout.margin;
}

uint32_t QuickPreviewWindow::CurrentPage(const PagesLayout& layout) const {
    const float probe = pages_scroll_ + (layout.view.bottom - layout.view.top) * 0.35f;
    float top = layout.margin;
    for (uint32_t i = 0; i < pages_count_; ++i) {
        const float bottom = top + layout.page_w * PageAspect(i);
        if (probe < bottom + layout.gap * 0.5f) return i;
        top = bottom + layout.gap;
    }
    return pages_count_ ? pages_count_ - 1 : 0;
}

void QuickPreviewWindow::ClampPages(const PagesLayout& layout) {
    const float view_w = layout.view.right - layout.view.left;
    const float view_h = layout.view.bottom - layout.view.top;
    const float max_scroll = (std::max)(0.0f, PagesDocHeight(layout) - view_h);
    const float max_pan = (std::max)(0.0f, layout.page_w + layout.margin * 2.0f - view_w);
    pages_scroll_ = std::clamp(pages_scroll_, 0.0f, max_scroll);
    pages_pan_x_ = std::clamp(pages_pan_x_, 0.0f, max_pan);
}

void QuickPreviewWindow::ScrollToPage(uint32_t page) {
    if (!pages_count_) return;
    page = (std::min)(page, pages_count_ - 1);
    const PagesLayout layout = ComputePagesLayout();
    pages_scroll_ = PageTop(layout, page) - layout.margin;
    ClampPages(layout);
}

void QuickPreviewWindow::ZoomPages(float cursor_x, float cursor_y, float factor) {
    const PagesLayout before = ComputePagesLayout();
    const float old_zoom = pages_zoom_;
    float next = std::clamp(old_zoom * factor, kMinZoom, kMaxZoom);
    if (std::fabs(next - 1.0f) < 0.03f) next = 1.0f;
    if (next == old_zoom) return;
    const float ratio = next / old_zoom;
    // Keep the document point under the cursor in place. Page heights scale
    // with the zoom; margins and gaps do not, which is close enough here.
    const float local_y = cursor_y - before.view.top;
    const float local_x = cursor_x - before.view.left;
    const float doc_y = pages_scroll_ + local_y;
    const float doc_x = pages_pan_x_ + local_x;
    pages_zoom_ = next;
    pages_scroll_ = (doc_y - before.margin) * ratio + before.margin - local_y;
    pages_pan_x_ = (doc_x - before.margin) * ratio + before.margin - local_x;
    ClampPages(ComputePagesLayout());
}

void QuickPreviewWindow::TogglePagesFit() {
    const PagesLayout layout = ComputePagesLayout();
    const uint32_t page = CurrentPage(layout);
    if (pages_zoom_ != 1.0f) {
        pages_zoom_ = 1.0f;
    } else {
        // Fit page: the whole current page fits the view height.
        const float view_h = layout.view.bottom - layout.view.top - layout.margin * 2.0f;
        const float fit_w = layout.page_w;  // zoom is 1 here
        const float page_h = fit_w * PageAspect(page);
        if (page_h <= view_h + 0.5f) return;  // already fully visible
        pages_zoom_ = std::clamp(view_h / page_h, kMinZoom, 1.0f);
    }
    pages_pan_x_ = 0.0f;
    ScrollToPage(page);
}

bool QuickPreviewWindow::PagesWheel(POINT client, float steps, bool ctrl, bool shift) {
    if (native_kind_ != NativeKind::Pages) return false;
    const PagesLayout layout = ComputePagesLayout();
    if (layout.strip.right > layout.strip.left && Inside(layout.strip, client) && !ctrl) {
        pages_strip_scroll_ -= steps * kWheelStep * scale_;  // clamped while drawing
        return true;
    }
    if (ctrl) {
        ZoomPages(static_cast<float>(client.x), static_cast<float>(client.y),
                  std::pow(1.15f, steps));
        return true;
    }
    if (shift) pages_pan_x_ -= steps * kWheelStep * scale_;
    else pages_scroll_ -= steps * kWheelStep * scale_;
    ClampPages(layout);
    return true;
}

bool QuickPreviewWindow::PagesMouseDown(POINT client) {
    if (native_kind_ != NativeKind::Pages) return false;
    const PagesLayout layout = ComputePagesLayout();
    if (layout.strip.right > layout.strip.left && Inside(layout.strip, client)) {
        const float pad = kStripPad * scale_;
        const float thumb_w = (layout.strip.right - layout.strip.left) - pad * 2.0f;
        float y = layout.strip.top + pad - pages_strip_scroll_;
        for (uint32_t i = 0; i < pages_count_; ++i) {
            const float h = thumb_w * PageAspect(i) + kStripLabel * scale_;
            if (client.y >= y && client.y < y + h) {
                ScrollToPage(i);
                break;
            }
            y += h + kStripGap * scale_;
            if (y > layout.strip.bottom) break;
        }
        return true;
    }
    if (Inside(layout.view, client)) {
        SetCapture(hwnd_);
        panning_ = true;
        pan_anchor_ = client;
        pan_start_x_ = pages_pan_x_;
        pan_start_y_ = pages_scroll_;
    }
    return true;
}

bool QuickPreviewWindow::PagesKey(WPARAM key) {
    if (native_kind_ != NativeKind::Pages || !pages_count_) return false;
    const PagesLayout layout = ComputePagesLayout();
    const uint32_t page = CurrentPage(layout);
    switch (key) {
    case VK_PRIOR: {
        // First to the top of a partly scrolled page, then to the previous one.
        const float top = PageTop(layout, page) - layout.margin;
        ScrollToPage(pages_scroll_ > top + 2.0f ? page : (page ? page - 1 : 0));
        return true;
    }
    case VK_NEXT:
        ScrollToPage(page + 1);
        return true;
    case VK_HOME:
        ScrollToPage(0);
        return true;
    case VK_END:
        ScrollToPage(pages_count_ - 1);
        return true;
    case '1': case VK_NUMPAD1:
        pages_zoom_ = 1.0f;
        pages_pan_x_ = 0.0f;
        ScrollToPage(page);
        return true;
    case '2': case VK_NUMPAD2:
        if (pages_zoom_ != 1.0f) {
            pages_zoom_ = 1.0f;
            ScrollToPage(page);
        }
        TogglePagesFit();
        return true;
    default:
        return false;
    }
}

void QuickPreviewWindow::DrawPages(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* secondary_brush) {
    if (!dc || !pages_count_) return;
    if (page_aspect_.size() != pages_count_) page_aspect_.resize(pages_count_, 0.0f);
    const PagesLayout layout = ComputePagesLayout();
    ClampPages(layout);
    const float view_w = layout.view.right - layout.view.left;
    const float view_h = layout.view.bottom - layout.view.top;
    const uint32_t current = CurrentPage(layout);

    ComPtr<ID2D1SolidColorBrush> paper, shadow, accent, fill;
    dc->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF), &paper);
    dc->CreateSolidColorBrush(D2D1::ColorF(0x000000, dark_ ? 0.45f : 0.16f), &shadow);
    dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(0x60CDFF) : D2D1::ColorF(0x005FB8), &accent);
    dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(0x0F0F0F) : D2D1::ColorF(0xE6E6E6), &fill);
    if (!paper.get() || !shadow.get() || !accent.get() || !fill.get()) return;

    auto draw_page = [&](uint32_t page, const D2D1_RECT_F& rect, uint32_t pixels) {
        uint32_t count = 0, delay = 0, source_w = 0, source_h = 0;
        PreviewDrawResult result = thumbnails_.Draw(dc, rect, item_.path, item_.attrs, pixels,
            generation_, item_.modified, item_.size, 1.0f, nullptr, nullptr, nullptr, true,
            nullptr, nullptr, nullptr, nullptr, nullptr, page, &count, &delay, nullptr,
            nullptr, nullptr, &source_w, &source_h);
        if (result == PreviewDrawResult::Bitmap && source_w && source_h)
            page_aspect_[page] = static_cast<float>(source_h) / static_cast<float>(source_w);
        if (result == PreviewDrawResult::Pending && pixels != kThumbPixels) {
            // A blurry stand-in beats a blank page while the sharp one renders.
            result = thumbnails_.Draw(dc, rect, item_.path, item_.attrs, kThumbPixels,
                generation_, item_.modified, item_.size, 1.0f, nullptr, nullptr, nullptr, true,
                nullptr, nullptr, nullptr, nullptr, nullptr, page);
        }
        return result;
    };

    // --- Thumbnail strip (drawn first: host requests are served newest first,
    // so the main pages queued afterwards render before the thumbnails). ---
    if (layout.strip.right > layout.strip.left) {
        dc->FillRectangle(layout.strip, fill.get());
        const float pad = kStripPad * scale_;
        const float thumb_w = (layout.strip.right - layout.strip.left) - pad * 2.0f;
        const float label_h = kStripLabel * scale_;
        const float step_gap = kStripGap * scale_;
        const float strip_h = layout.strip.bottom - layout.strip.top;
        float total = pad;
        float current_top = 0.0f, current_bottom = 0.0f;
        for (uint32_t i = 0; i < pages_count_; ++i) {
            const float h = thumb_w * PageAspect(i) + label_h;
            if (i == current) { current_top = total; current_bottom = total + h; }
            total += h + step_gap;
        }
        total += pad - step_gap;
        if (current != pages_follow_) {
            pages_follow_ = current;
            if (current_top - pad < pages_strip_scroll_)
                pages_strip_scroll_ = current_top - pad;
            else if (current_bottom + pad > pages_strip_scroll_ + strip_h)
                pages_strip_scroll_ = current_bottom + pad - strip_h;
        }
        pages_strip_scroll_ = std::clamp(pages_strip_scroll_, 0.0f, (std::max)(0.0f, total - strip_h));
        dc->PushAxisAlignedClip(layout.strip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        IDWriteTextFormat* small_format = compositor_.SmallFormat();
        if (small_format) {
            small_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            small_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        float y = layout.strip.top + pad - pages_strip_scroll_;
        for (uint32_t i = 0; i < pages_count_ && y < layout.strip.bottom; ++i) {
            const float h = thumb_w * PageAspect(i);
            if (y + h + label_h >= layout.strip.top) {
                const D2D1_RECT_F rect = D2D1::RectF(layout.strip.left + pad, y,
                                                     layout.strip.left + pad + thumb_w, y + h);
                dc->FillRectangle(rect, paper.get());
                draw_page(i, rect, kThumbPixels);
                if (i == current) {
                    const float o = 2.5f * scale_;
                    dc->DrawRectangle(D2D1::RectF(rect.left - o, rect.top - o,
                                                  rect.right + o, rect.bottom + o),
                                      accent.get(), 2.0f * scale_);
                }
                if (small_format) {
                    wchar_t label[16];
                    swprintf_s(label, L"%u", i + 1);
                    dc->DrawTextW(label, static_cast<UINT32>(wcslen(label)), small_format,
                        D2D1::RectF(rect.left, rect.bottom, rect.right, rect.bottom + label_h),
                        i == current ? accent.get() : secondary_brush);
                }
            }
            y += h + label_h + step_gap;
        }
        if (small_format) {
            small_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            small_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }
        dc->PopAxisAlignedClip();
    }

    // --- Pages: visible ones plus a prefetch band above and below. ---
    struct Slot { uint32_t page; D2D1_RECT_F rect; float distance; };
    std::vector<Slot> slots;
    const float x0 = layout.page_w + layout.margin * 2.0f <= view_w
        ? layout.view.left + (view_w - layout.page_w) * 0.5f
        : layout.view.left + layout.margin - pages_pan_x_;
    const float band_top = pages_scroll_ - view_h * 0.6f;
    const float band_bottom = pages_scroll_ + view_h * 1.6f;
    const float center = pages_scroll_ + view_h * 0.5f;
    float top = layout.margin;
    for (uint32_t i = 0; i < pages_count_ && top < band_bottom; ++i) {
        const float h = layout.page_w * PageAspect(i);
        if (top + h >= band_top) {
            const float y = layout.view.top + top - pages_scroll_;
            slots.push_back({i, D2D1::RectF(x0, y, x0 + layout.page_w, y + h),
                             std::fabs(top + h * 0.5f - center)});
        }
        top += h + layout.gap;
    }
    // Farthest first: the current page is queued last and so rendered first.
    std::sort(slots.begin(), slots.end(),
              [](const Slot& a, const Slot& b) { return a.distance > b.distance; });
    dc->PushAxisAlignedClip(layout.view, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    IDWriteTextFormat* text_format = compositor_.TextFormat();
    for (const Slot& slot : slots) {
        const D2D1_RECT_F& rect = slot.rect;
        const float s = 2.0f * scale_;
        dc->FillRectangle(D2D1::RectF(rect.left - s * 0.5f, rect.top, rect.right + s * 0.5f,
                                      rect.bottom + s), shadow.get());
        dc->FillRectangle(rect, paper.get());
        const float edge = (std::max)(rect.right - rect.left, rect.bottom - rect.top);
        const uint32_t pixels = (std::min)(kMaxPagePixels,
            ipc::BucketPreviewPixelSize(static_cast<uint32_t>((std::max)(1.0f, edge))));
        if (draw_page(slot.page, rect, pixels) != PreviewDrawResult::Bitmap && text_format) {
            wchar_t label[16];
            swprintf_s(label, L"%u", slot.page + 1);
            text_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            text_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            ComPtr<ID2D1SolidColorBrush> grey;
            dc->CreateSolidColorBrush(D2D1::ColorF(0x9A9A9A), &grey);
            if (grey.get())
                dc->DrawTextW(label, static_cast<UINT32>(wcslen(label)), text_format, rect,
                              grey.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            text_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            text_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }

    // --- Scroll indicator. ---
    const float doc_h = PagesDocHeight(layout);
    if (doc_h > view_h + 1.0f) {
        const float track_top = layout.view.top + 4.0f * scale_;
        const float track_h = view_h - 8.0f * scale_;
        const float thumb_h = (std::max)(24.0f * scale_, track_h * view_h / doc_h);
        const float t = pages_scroll_ / (doc_h - view_h);
        const float y = track_top + (track_h - thumb_h) * t;
        const float w = 4.0f * scale_;
        ComPtr<ID2D1SolidColorBrush> bar;
        dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(0xFFFFFF, 0.35f)
                                        : D2D1::ColorF(0x000000, 0.30f), &bar);
        if (bar.get())
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(
                layout.view.right - w - 3.0f * scale_, y, layout.view.right - 3.0f * scale_,
                y + thumb_h), w * 0.5f, w * 0.5f), bar.get());
    }

    // --- Page and zoom live in the footer. ---
    {
        std::vector<std::wstring> parts;
        std::wstring ext;
        if (const size_t dot = item_.name.find_last_of(L'.'); dot != std::wstring::npos && dot + 1 < item_.name.size()) {
            ext = item_.name.substr(dot + 1);
            for (wchar_t& c : ext) c = static_cast<wchar_t>(std::towupper(c));
        }
        if (!ext.empty()) parts.push_back(std::move(ext));
        wchar_t label[96];
        swprintf_s(label, pulse::l10n::Get(pulse::l10n::StringId::PreviewPageOf).c_str(), current + 1, pages_count_);
        parts.push_back(label);
        if (item_.size) parts.push_back(pulse::format::ByteSize(item_.size, true));
        SetFooterParts(std::move(parts));
        std::wstring zoom;
        if (pages_zoom_ == 1.0f) {
            zoom = pulse::l10n::Get(pulse::l10n::StringId::PreviewFit).c_str();
        } else {
            wchar_t percent[16];
            swprintf_s(percent, L"%d%%", static_cast<int>(std::lround(pages_zoom_ * 100.0f)));
            zoom = percent;
        }
        AddFooterControl(kFooterZoomOut, L"\x2212");
        AddFooterControl(kFooterZoom, std::move(zoom));
        AddFooterControl(kFooterZoomIn, L"+");
    }
    dc->PopAxisAlignedClip();
}

}  // namespace pulse::ui

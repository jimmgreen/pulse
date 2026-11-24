#include "quick_preview_window.h"
#include "quick_preview_occlusion.h"

#include <array>
#include <cmath>

namespace pulse::ui {

void QuickPreviewWindow::SetEmbeddedOcclusion(const D2D1_RECT_F& parent_bounds) {
    ClipEmbeddedPreview(*this, parent_bounds, {});
}

void ClipEmbeddedPreview(QuickPreviewWindow& preview, const D2D1_RECT_F& parent_bounds, const D2D1_RECT_F& other_bounds) {
    const HWND hwnd = preview.hwnd_;
    if (!hwnd || !preview.embedded_) return;
    RECT window{};
    if (!GetWindowRect(hwnd, &window)) return;
    const RECT bounds{0, 0, window.right - window.left, window.bottom - window.top};
    std::array<RECT, 2> holes{};
    size_t count = 0;
    for (const auto& rect : {parent_bounds, other_bounds}) {
        if (rect.right <= rect.left || rect.bottom <= rect.top) continue;
        RECT cover{static_cast<LONG>(std::floor(rect.left)), static_cast<LONG>(std::floor(rect.top)),
                   static_cast<LONG>(std::ceil(rect.right)), static_cast<LONG>(std::ceil(rect.bottom))};
        MapWindowPoints(preview.owner_, nullptr, reinterpret_cast<POINT*>(&cover), 2);
        OffsetRect(&cover, -window.left, -window.top);
        if (IntersectRect(&holes[count], &bounds, &cover)) ++count;
    }
    const bool covered = count != 0;
    const RECT single_hole = count == 1 ? holes.front() : RECT{};
    if (covered == preview.embedded_occluded_ && (!covered || (count == 1 &&
        EqualRect(&single_hole, &preview.embedded_occlusion_) && EqualRect(&bounds, &preview.embedded_region_bounds_)))) return;

    if (covered) {
        HRGN region = CreateRectRgnIndirect(&bounds);
        HRGN cutout = CreateRectRgn(0, 0, 0, 0);
        bool valid = region && cutout;
        for (size_t i = 0; valid && i < count; ++i) {
            const auto& r = holes[i];
            valid = SetRectRgn(cutout, r.left, r.top, r.right, r.bottom) &&
                    CombineRgn(region, region, cutout, RGN_DIFF) != ERROR;
        }
        if (valid && count == 2 && preview.embedded_occluded_ &&
            GetWindowRgn(hwnd, cutout) != ERROR && EqualRgn(region, cutout)) {
            DeleteObject(cutout);
            DeleteObject(region);
            return;
        }
        if (cutout) DeleteObject(cutout);
        if (!valid || !SetWindowRgn(hwnd, region, TRUE)) {
            if (region) DeleteObject(region);
            return;
        }
        // SetWindowRgn owns region after success. A region also clips child
        // controls/video, and lets mouse input reach the parent's flyout.
    } else if (!SetWindowRgn(hwnd, nullptr, TRUE)) {
        return;
    }
    // Shell providers use a separate topmost overlay, outside child clipping.
    // Cover it without stopping its worker/session, only on cover transitions.
    if (covered != preview.embedded_occluded_) preview.handler_.SetCovered(covered || preview.handler_covered_);
    preview.embedded_occluded_ = covered;
    preview.embedded_occlusion_ = single_hole;
    preview.embedded_region_bounds_ = bounds;
    InvalidateRect(preview.owner_, nullptr, FALSE);
    InvalidateRect(hwnd, nullptr, FALSE);
}

} // namespace pulse::ui

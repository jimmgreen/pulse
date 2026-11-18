#include "quick_preview_window.h"

#include <cmath>

namespace pulse::ui {

void QuickPreviewWindow::SetEmbeddedOcclusion(const D2D1_RECT_F& parent_bounds) {
    if (!hwnd_ || !embedded_) return;
    RECT window{};
    if (!GetWindowRect(hwnd_, &window)) return;
    const RECT bounds{0, 0, window.right - window.left, window.bottom - window.top};
    RECT hole{};
    bool covered = false;
    if (parent_bounds.right > parent_bounds.left && parent_bounds.bottom > parent_bounds.top) {
        RECT cover{static_cast<LONG>(std::floor(parent_bounds.left)),
                   static_cast<LONG>(std::floor(parent_bounds.top)),
                   static_cast<LONG>(std::ceil(parent_bounds.right)),
                   static_cast<LONG>(std::ceil(parent_bounds.bottom))};
        MapWindowPoints(owner_, nullptr, reinterpret_cast<POINT*>(&cover), 2);
        OffsetRect(&cover, -window.left, -window.top);
        covered = IntersectRect(&hole, &bounds, &cover) != FALSE;
    }
    if (covered == embedded_occluded_ && (!covered ||
        (EqualRect(&hole, &embedded_occlusion_) && EqualRect(&bounds, &embedded_region_bounds_)))) return;

    if (covered) {
        HRGN region = CreateRectRgnIndirect(&bounds);
        HRGN cutout = CreateRectRgnIndirect(&hole);
        if (!region || !cutout) {
            if (region) DeleteObject(region);
            if (cutout) DeleteObject(cutout);
            return;
        }
        const int combined = CombineRgn(region, region, cutout, RGN_DIFF);
        DeleteObject(cutout);
        if (combined == ERROR || !SetWindowRgn(hwnd_, region, TRUE)) {
            DeleteObject(region);
            return;
        }
        // SetWindowRgn owns region after success. A region also clips child
        // controls/video, and lets mouse input reach the parent's flyout.
    } else if (!SetWindowRgn(hwnd_, nullptr, TRUE)) {
        return;
    }
    // Shell providers use a separate topmost overlay, outside child clipping.
    // Cover it without stopping its worker/session, only on cover transitions.
    if (covered != embedded_occluded_) handler_.SetCovered(covered || handler_covered_);
    embedded_occluded_ = covered;
    embedded_occlusion_ = hole;
    embedded_region_bounds_ = bounds;
    InvalidateRect(owner_, nullptr, FALSE);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

} // namespace pulse::ui

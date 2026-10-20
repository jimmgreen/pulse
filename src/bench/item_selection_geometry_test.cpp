#include "../ui/item_selection_geometry.h"
#include <cstdio>
#include <cassert>

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

void TestItemSelectionGeometryAllModes() {
    using namespace pulse::ui;
    const ViewMode modes[] = {
        ViewMode::ExtraLargeIcons,
        ViewMode::LargeIcons,
        ViewMode::MediumIcons,
        ViewMode::SmallIcons,
        ViewMode::List,
        ViewMode::Details,
        ViewMode::Tiles,
        ViewMode::Content
    };

    const float scales[] = { 1.0f, 1.5f, 2.0f };

    for (float scale : scales) {
        for (ViewMode mode : modes) {
            const D2D1_RECT_F item_rect = D2D1::RectF(50.0f, 100.0f, 250.0f, 160.0f);

            // Compact density
            const auto compact_geom = ComputeItemSelectionGeometry(
                mode, item_rect, scale, TouchDensity::Compact, false, true, false, CheckboxVisibility::Auto);

            Check(compact_geom.visible, "Checkbox is visible when hovered");
            const float vis_w = compact_geom.visual_rect.right - compact_geom.visual_rect.left;
            const float vis_h = compact_geom.visual_rect.bottom - compact_geom.visual_rect.top;
            Check(std::abs(vis_w - 18.0f * scale) < 0.001f, "Visual box width is 18 DIP");
            Check(std::abs(vis_h - 18.0f * scale) < 0.001f, "Visual box height is 18 DIP");

            // Touch density: hit rect must be at least 44x44 DIP (or clamped to item)
            const auto touch_geom = ComputeItemSelectionGeometry(
                mode, item_rect, scale, TouchDensity::Touch, true, false, true, CheckboxVisibility::Auto);

            Check(touch_geom.visible, "Checkbox is visible when selected");
            const float hit_w = touch_geom.hit_rect.right - touch_geom.hit_rect.left;
            const float hit_h = touch_geom.hit_rect.bottom - touch_geom.hit_rect.top;
            Check(hit_w >= 44.0f * scale - 0.01f || touch_geom.hit_rect.right == item_rect.right,
                  "Touch density hit width meets 44 DIP minimum or item boundary");
            Check(hit_h >= 44.0f * scale - 0.01f || touch_geom.hit_rect.bottom == item_rect.bottom,
                  "Touch density hit height meets 44 DIP minimum or item boundary");

            // Grid vs Linear behavior
            const bool is_grid = (mode == ViewMode::ExtraLargeIcons ||
                                  mode == ViewMode::LargeIcons ||
                                  mode == ViewMode::MediumIcons);
            if (is_grid) {
                Check(touch_geom.leading_slot_width >= 44.0f * scale,
                       "Grid views reserve an independent checkbox gutter before artwork");
                Check(touch_geom.visual_rect.left == item_rect.left + 6.0f * scale,
                      "Grid view checkbox is placed at top-left corner");
            } else {
                Check(touch_geom.leading_slot_width > 0.0f,
                      "Linear views reserve leading slot for checkbox");
            }
        }
    }
}

void TestHeaderSelectionGeometry() {
    using namespace pulse::ui;
    const D2D1_RECT_F col_rect = D2D1::RectF(10.0f, 0.0f, 200.0f, 32.0f);
    const float scale = 1.25f;

    const auto compact_hdr = ComputeHeaderSelectionGeometry(col_rect, scale, TouchDensity::Compact);
    Check(compact_hdr.hit_rect.left == col_rect.left, "Header hit rect begins at column left");
    Check(compact_hdr.hit_rect.right < col_rect.right, "Header hit rect does not touch right divider");

    const auto touch_hdr = ComputeHeaderSelectionGeometry(col_rect, scale, TouchDensity::Touch);
    const float hit_w = touch_hdr.hit_rect.right - touch_hdr.hit_rect.left;
    Check(hit_w >= 44.0f * scale - 0.01f, "Touch header hit width is at least 44 DIP");
}

void TestDensityAndPolicyResolution() {
    using namespace pulse::ui;

    // Density resolution
    Check(ResolveTouchDensity(L"touch", false) == TouchDensity::Touch, "Force touch mode wins");
    Check(ResolveTouchDensity(L"compact", true) == TouchDensity::Compact, "Force compact mode wins");
    Check(ResolveTouchDensity(L"auto", true) == TouchDensity::Touch, "Auto resolves to Touch with digitizer");
    Check(ResolveTouchDensity(L"auto", false) == TouchDensity::Compact, "Auto resolves to Compact without digitizer");

    // Policy resolution
    Check(ResolveCheckboxVisibility(L"always") == CheckboxVisibility::Always, "Policy always");
    Check(ResolveCheckboxVisibility(L"never") == CheckboxVisibility::Never, "Policy never");
    Check(ResolveCheckboxVisibility(L"auto") == CheckboxVisibility::Auto, "Policy auto");
    Check(ResolveCheckboxVisibility(L"invalid") == CheckboxVisibility::Auto, "Invalid policy defaults to auto");

    // Visibility checks
    const D2D1_RECT_F r = D2D1::RectF(0, 0, 100, 30);
    Check(ComputeItemSelectionGeometry(ViewMode::Details, r, 1.0f, TouchDensity::Compact, false, false, false, CheckboxVisibility::Never).visible == false,
          "Never policy keeps checkbox hidden");
    Check(ComputeItemSelectionGeometry(ViewMode::Details, r, 1.0f, TouchDensity::Compact, false, false, false, CheckboxVisibility::Always).visible == true,
          "Always policy keeps checkbox visible");
    Check(ComputeItemSelectionGeometry(ViewMode::Details, r, 1.0f, TouchDensity::Compact, false, false, true, CheckboxVisibility::Auto).visible == true,
          "Auto policy shows checkbox when touchscreen is ready");
    Check(ComputeItemSelectionGeometry(ViewMode::Details, r, 1.0f, TouchDensity::Compact, false, false, false, CheckboxVisibility::Auto).visible == false,
          "Auto policy hides unselected unhovered checkbox on mouse-only device");
}

bool Overlaps(const D2D1_RECT_F& a, const D2D1_RECT_F& b) {
    return a.left < b.right - 0.01f && b.left < a.right - 0.01f &&
           a.top < b.bottom - 0.01f && b.top < a.bottom - 0.01f;
}

void TestLayoutOwnsCheckboxGutter() {
    using namespace pulse::ui;
    const ViewMode modes[] = {ViewMode::ExtraLargeIcons, ViewMode::LargeIcons,
        ViewMode::MediumIcons, ViewMode::SmallIcons, ViewMode::List, ViewMode::Details,
        ViewMode::Tiles, ViewMode::Content};
    for (const float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
        for (const float width : {90.0f, 320.0f, 960.0f}) {
            for (const auto density : {TouchDensity::Compact, TouchDensity::Touch}) {
                for (const auto mode : modes) {
                    const auto viewport = D2D1::RectF(13 * scale, 23 * scale,
                        (13 + width) * scale, 623 * scale);
                    const ViewLayout layout(mode, viewport, 100, 0, 0, scale,
                        EffectiveRowHeightDip(28, density), nullptr,
                        SelectionSlotDip(density, CheckboxVisibility::Auto));
                    const auto cell = layout.ItemRect(0);
                    const auto cb = ComputeItemSelectionGeometry(mode, cell, scale, density,
                        false, true, true);
                    const auto icon = layout.IconRect(0);
                    const auto thumb = layout.ThumbnailRect(0);
                    const auto name = layout.NameRect(0);
                    Check(!Overlaps(cb.hit_rect, icon), "Checkbox hit target never covers file icon");
                    Check(!Overlaps(cb.hit_rect, thumb), "Checkbox hit target never covers thumbnail");
                    Check(!Overlaps(cb.hit_rect, name), "Checkbox hit target never covers filename");
                    Check(icon.left <= icon.right && icon.right <= cell.right + 0.01f,
                        "Narrow-pane icon remains inside its cell");
                    Check(cb.hit_rect.left >= cell.left && cb.hit_rect.right <= cell.right &&
                        cb.hit_rect.top >= cell.top && cb.hit_rect.bottom <= cell.bottom,
                        "Checkbox hit area cannot steal an adjacent item");
                    const auto hidden = ComputeItemSelectionGeometry(mode, cell, scale, density,
                        false, false, false);
                    Check(hidden.leading_slot_width == cb.leading_slot_width,
                        "Hover and selection do not move file content");
                    if (density == TouchDensity::Touch)
                        Check(cell.bottom - cell.top >= 44 * scale - 0.01f,
                            "Touch rows meet the 44 DIP minimum");
                    const float cx = (cell.left + (std::min)(cell.right, viewport.right)) * 0.5f;
                    const float cy = (cell.top + cell.bottom) * 0.5f;
                    Check(layout.HitTest(cx, cy) == 0, "Hit testing shares rendered cell geometry");
                }
            }
        }
    }
    Check(SelectionSlotDip(TouchDensity::Touch, CheckboxVisibility::Never) == 0,
        "Never policy removes the reserved checkbox gutter");
    for (const auto mode : {ViewMode::SmallIcons, ViewMode::List}) {
        for (const float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
            const ViewLayout hidden(mode, {0, 0, 640 * scale, 480 * scale}, 100,
                0, 0, scale, 44, nullptr, SelectionSlotDip(TouchDensity::Touch, CheckboxVisibility::Never));
            const auto cell = hidden.ItemRect(0);
            Check(cell.bottom - cell.top >= 44 * scale - 0.01f,
                "Hiding checkboxes preserves the touch target row height");
            const auto cb = ComputeItemSelectionGeometry(mode, cell, scale, TouchDensity::Touch,
                true, true, true, CheckboxVisibility::Never);
            Check(!cb.visible && cb.leading_slot_width == 0,
                "Never remains hidden even on selected or hovered touchscreen items");
        }
    }
}

} // namespace

int main() {
    TestItemSelectionGeometryAllModes();
    TestHeaderSelectionGeometry();
    TestDensityAndPolicyResolution();
    TestLayoutOwnsCheckboxGutter();

    std::printf("item_selection_geometry_test: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}

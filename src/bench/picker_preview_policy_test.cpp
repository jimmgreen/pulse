#include "../ui/picker_preview_layout.h"
#include "../ui/panel_metrics.h"
#include <array>
#include <cstdio>

namespace {
int failures = 0, checks = 0;
void Check(bool ok, const char* label) {
    ++checks;
    if (!ok) { ++failures; std::printf("[FAIL] %s\n", label); }
}
bool Nonempty(D2D1_RECT_F r) { return r.right > r.left && r.bottom > r.top; }
bool Inside(D2D1_RECT_F r, D2D1_RECT_F panel) {
    return !Nonempty(r) || (r.left >= panel.left && r.right <= panel.right && r.top >= panel.top && r.bottom <= panel.bottom);
}
bool Intersects(D2D1_RECT_F a, D2D1_RECT_F b) {
    return Nonempty(a) && Nonempty(b) && a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}
}
int main() {
    using namespace pulse::ui;
    Check(DetailsVisibleWindowDip(false) == 1000 && DetailsVisibleWindowDip(false, true) == 1000,
        "main-window threshold unaffected even if force bit is set");
    Check(DetailsVisibleWindowDip(true) == 760, "picker uses design 760 DIP threshold");
    Check(DetailsVisibleWindowDip(true, true) == 0, "forced native preview bypasses automatic narrow collapse");
    for (const float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
        for (const float width : {640.0f, 759.0f, 760.0f, 999.0f, 1000.0f}) {
            Check(((width * scale) / scale >= DetailsVisibleWindowDip(true)) == (width >= 760), "picker threshold remains DIP-scaled");
            Check(((width * scale) / scale >= DetailsVisibleWindowDip(false)) == (width >= 1000), "main threshold remains DIP-scaled");
        }
        for (const float width : {60.0f, 100.0f, 180.0f, 240.0f, 280.0f, 300.0f, 340.0f, 480.0f}) {
            for (const float height : {20.0f, 55.0f, 56.0f, 90.0f, 100.0f, 160.0f, 240.0f, 400.0f, 700.0f}) {
                const D2D1_RECT_F panel{17 * scale, 23 * scale, (17 + width) * scale, (23 + height) * scale};
                const auto layout = LayoutPickerPreview(panel, scale, 360 * scale);
                const std::array<D2D1_RECT_F, 7> boxes{layout.preview, layout.name, layout.subtitle, layout.modified,
                    layout.location, layout.copy_path, layout.reveal};
                for (const auto box : boxes) Check(Inside(box, panel), "preview metadata/action rect stays within narrow or short panel");
                for (size_t i = 0; i < boxes.size(); ++i)
                    for (size_t j = i + 1; j < boxes.size(); ++j)
                        Check(!Intersects(boxes[i], boxes[j]), "metadata/actions/preview do not overlap at any scale");
                const float inner_width = width - 24;
                const float actions_height = inner_width < 260 ? 70.0f : 32.0f;
                const bool fits_actions = inner_width >= 64 && height - 24 >= actions_height;
                Check(Nonempty(layout.copy_path) == fits_actions && Nonempty(layout.reveal) == fits_actions,
                    "both actions remain reachable whenever their measured rows fit");
                if (Nonempty(layout.preview)) Check(layout.preview.bottom <= layout.name.top, "preview cannot cover filename or action rows");
            }
        }
    }
    Check(PickerSpaceTogglesPreview(true, false, false, false), "plain list Space toggles preview without changing selection");
    Check(!PickerSpaceTogglesPreview(false, false, false, false), "Space outside list retains edit/control behavior");
    Check(!PickerSpaceTogglesPreview(true, true, false, false), "Ctrl+Space remains selection gesture");
    Check(!PickerSpaceTogglesPreview(true, false, true, false), "Shift+Space remains selection gesture");
    Check(!PickerSpaceTogglesPreview(true, false, false, true), "Alt+Space is not consumed by preview policy");
    const auto roomy = LayoutPickerPreview({0, 0, 340, 600}, 1, 360);
    Check(Nonempty(roomy.preview) && Nonempty(roomy.name) && Nonempty(roomy.subtitle) && Nonempty(roomy.modified) && Nonempty(roomy.location),
        "normal picker preview retains all existing-record metadata rows");
    const auto short_panel = LayoutPickerPreview({0, 0, 340, 100}, 1, 360);
    Check(!Nonempty(short_panel.preview) && Nonempty(short_panel.copy_path) && Nonempty(short_panel.reveal),
        "short panel gives up preview rather than clipping its actions");
    std::printf("RESULT checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}

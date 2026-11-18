#pragma once
#include "../app/blank_pane_click.h"

namespace pulse::ui {
enum class PickerBlankAction { None, Back, Up };
inline PickerBlankAction PickerBlankNavigation(int preference, bool can_back, bool can_up) {
    if (app::NormalizeBlankClickAction(preference) == app::kBlankClickOff) return PickerBlankAction::None;
    if (app::BlankClickGoesBack(preference, can_back)) return PickerBlankAction::Back;
    return can_up ? PickerBlankAction::Up : PickerBlankAction::None;
}

class PickerBlankClick {
public:
    void Reset() { *this = {}; }
    void CancelPress() { if (pending_) Reset(); }
    void Begin(bool blank, bool double_click, uint64_t generation, int x, int y) {
        const bool second = blank && double_click && first_ && generation == generation_;
        Reset();
        pending_ = blank;
        second_ = second;
        generation_ = generation;
        x_ = x; y_ = y;
    }
    void Move(int x, int y, int width, int height) {
        if (pending_ && (static_cast<int64_t>(x) - x_ <= -width || static_cast<int64_t>(x) - x_ >= width ||
            static_cast<int64_t>(y) - y_ <= -height || static_cast<int64_t>(y) - y_ >= height)) Reset();
    }
    bool Release(app::BlankPaneClickRelease click, uint64_t generation, int x, int y) {
        click.pending = pending_;
        click.same_context = generation == generation_;
        click.delta_x = x - x_;
        click.delta_y = y - y_;
        const bool valid = app::IsBlankPaneBackClick(click);
        const bool navigate = valid && second_;
        pending_ = second_ = false;
        first_ = valid && !navigate;
        return navigate;
    }
private:
    bool pending_ = false, first_ = false, second_ = false;
    uint64_t generation_ = 0;
    int x_ = 0, y_ = 0;
};
} // namespace pulse::ui

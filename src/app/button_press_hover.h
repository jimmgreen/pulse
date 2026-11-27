#pragma once
#include "button_press.h"

namespace pulse::app {
inline bool IsHoverRowAction(ui::HitTestResult::Region region) {
    using H = ui::HitTestResult;
    return region == H::RowStar || region == H::RowNewTab || region == H::RowMore;
}
} // namespace pulse::app

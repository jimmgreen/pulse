#pragma once
#include <cstdint>

namespace pulse::app {
enum class SearchTrigger : uint8_t { Hotkey, DoubleCtrl, External };
enum class SearchVisibility : uint8_t { Hidden, Opening, VisibleFocused, VisibleUnfocused };
}

#pragma once

namespace pulse::app {
// Call once after COM and localization initialize. Failure leaves startup unaffected.
void UpdateWindowJumpList();
} // namespace pulse::app

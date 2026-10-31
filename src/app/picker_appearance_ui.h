#pragma once
#include "picker_appearance.h"
#include "../ui/FluentTokens.h"

namespace pulse::app {
inline PickerAppearanceColors ResolveSystemPickerAppearanceColors(const PickerAppearanceSnapshot& appearance) noexcept {
    const auto color = ui::GetAccentColor();
    const auto channel = [](float value) { return static_cast<uint32_t>(value * 255.0f + 0.5f); };
    const uint32_t rgb = (channel(color.r) << 16) | (channel(color.g) << 8) | channel(color.b);
    return ResolvePickerAppearanceColors(appearance, ui::ShouldUseDarkMode(ui::ThemeMode::Auto), rgb);
}
}

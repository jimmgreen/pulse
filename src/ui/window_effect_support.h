#pragma once
#include "../common/os_version.h"
#include "../common/windows_compat.h"
#include <string_view>
namespace pulse::ui {
enum class WindowEffect { None = 0, Acrylic, Mica, MicaAlt };
constexpr WindowEffect WindowEffectFromId(std::wstring_view id) noexcept {
    if (id == L"acrylic-material" || id == L"acrylic" || id == L"dwm-blur")
        return WindowEffect::Acrylic;
    if (id == L"mica") return WindowEffect::Mica;
    if (id == L"mica-alt" || id == L"miac-alt") return WindowEffect::MicaAlt;
    if (id == L"none") return WindowEffect::None;
    return WindowEffect::MicaAlt;
}
// All window materials currently use DWMWA_SYSTEMBACKDROP_TYPE (Windows 11 22H2).
constexpr bool WindowEffectSupportedOnBuild(WindowEffect effect, unsigned build, bool modern = true) noexcept {
    return effect == WindowEffect::None || (modern && build >= 22621 &&
        (effect == WindowEffect::Acrylic || effect == WindowEffect::Mica || effect == WindowEffect::MicaAlt));
}
inline bool WindowEffectSupported(WindowEffect effect) noexcept {
    return WindowEffectSupportedOnBuild(effect, pulse::os::TrueWindowsBuild(), compat::ModernWindows());
}
inline WindowEffect SupportedWindowEffect(WindowEffect effect) noexcept {
    return WindowEffectSupported(effect) ? effect : WindowEffect::None;
}
}

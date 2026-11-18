#include <initializer_list>
#include "../ui/window_effect_support.h"
#include <cstdio>
int main() {
    using namespace pulse::ui;
    static_assert(WindowEffectFromId(L"acrylic-material") == WindowEffect::Acrylic);
    static_assert(WindowEffectFromId(L"acrylic") == WindowEffect::Acrylic);
    static_assert(WindowEffectFromId(L"dwm-blur") == WindowEffect::Acrylic);
    static_assert(WindowEffectFromId(L"mica") == WindowEffect::Mica);
    static_assert(WindowEffectFromId(L"mica-alt") == WindowEffect::MicaAlt);
    static_assert(WindowEffectFromId(L"miac-alt") == WindowEffect::MicaAlt);
    static_assert(WindowEffectFromId(L"none") == WindowEffect::None);
    static_assert(WindowEffectFromId(L"") == WindowEffect::MicaAlt);
    static_assert(WindowEffectFromId(L"unknown") == WindowEffect::MicaAlt);
    int failures = 0, checks = 0;
    for (unsigned build : {0u, 9600u, 19045u, 20348u, 22000u, 22620u, 22621u, 26100u}) {
        for (auto effect : {WindowEffect::None, WindowEffect::Acrylic, WindowEffect::Mica, WindowEffect::MicaAlt}) {
            for (bool modern : {false, true}) {
#ifdef PULSE_MATERIAL_BASELINE
                const bool actual = effect == WindowEffect::None || modern;
#else
                const bool actual = WindowEffectSupportedOnBuild(effect, build, modern);
#endif
                const bool expected = effect == WindowEffect::None || (modern && build >= 22621);
                ++checks;
                if (actual != expected) { ++failures; std::printf("[FAIL] build=%u effect=%d modern=%d\n", build, int(effect), int(modern)); }
            }
        }
    }
    std::printf("Checks=%d Failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}


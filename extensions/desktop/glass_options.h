#pragma once
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <string>
#include <string_view>

namespace pulse::desktop {
struct GlassOpticalSettings {
    double thickness=20, ior=1.4, dispersion=7, blur_radius=1;
    double fresnel_range=30, fresnel_hardness=20, fresnel_factor=20;
    double glare_range=30, glare_hardness=20, glare_factor=90;
    double glare_convergence=50, glare_opposite=80, glare_angle=-45;
    double tint_r=255, tint_g=255, tint_b=255, tint_a=0;
    bool blur_edge=true;
    bool operator==(const GlassOpticalSettings&) const noexcept = default;
};
inline GlassOpticalSettings LegacyGlassOptions(double thickness, double ior_x100,
    double dispersion, double glare, double angle) noexcept {
    GlassOpticalSettings result;
    result.thickness=thickness; result.ior=ior_x100/100.0;
    result.dispersion=dispersion; result.glare_factor=glare; result.glare_angle=angle;
    return result; // Legacy frost is not a blur radius.
}
struct GlassOpticalField {
    std::string_view key;
    double GlassOpticalSettings::*member;
    double min, max;
};
inline constexpr std::array<GlassOpticalField,17> kGlassOpticalFields{{
    {"glass2_thickness",&GlassOpticalSettings::thickness,1,80},
    {"glass2_ior",&GlassOpticalSettings::ior,1,4},
    {"glass2_dispersion",&GlassOpticalSettings::dispersion,0,50},
    {"glass2_blur_radius",&GlassOpticalSettings::blur_radius,1,200},
    {"glass2_fresnel_range",&GlassOpticalSettings::fresnel_range,0,100},
    {"glass2_fresnel_hardness",&GlassOpticalSettings::fresnel_hardness,0,100},
    {"glass2_fresnel_factor",&GlassOpticalSettings::fresnel_factor,0,100},
    {"glass2_glare_range",&GlassOpticalSettings::glare_range,0,100},
    {"glass2_glare_hardness",&GlassOpticalSettings::glare_hardness,0,100},
    {"glass2_glare_factor",&GlassOpticalSettings::glare_factor,0,120},
    {"glass2_glare_convergence",&GlassOpticalSettings::glare_convergence,0,100},
    {"glass2_glare_opposite",&GlassOpticalSettings::glare_opposite,0,100},
    {"glass2_glare_angle",&GlassOpticalSettings::glare_angle,-180,180},
    {"glass2_tint_r",&GlassOpticalSettings::tint_r,0,255},
    {"glass2_tint_g",&GlassOpticalSettings::tint_g,0,255},
    {"glass2_tint_b",&GlassOpticalSettings::tint_b,0,255},
    {"glass2_tint_a",&GlassOpticalSettings::tint_a,0,100}
}};
inline double ClampGlassValue(const GlassOpticalField& field, double value) noexcept {
    const GlassOpticalSettings defaults;
    return std::isfinite(value) ? std::clamp(value,field.min,field.max) : defaults.*(field.member);
}
inline std::string GlassNumber(double value) {
    char buffer[64]{};
    const auto result=std::to_chars(buffer,buffer+sizeof(buffer),value,std::chars_format::general,15);
    return result.ec==std::errc{} ? std::string(buffer,result.ptr) : "0";
}
// Fragment appended to existing flat settings JSON; independent from legacy integer units.
inline std::string GlassOpticalJson(const GlassOpticalSettings& settings) {
    std::string out=",\"glass_schema\":2";
    for(const auto& field:kGlassOpticalFields)
        out+=",\""+std::string(field.key)+"\":"+GlassNumber(ClampGlassValue(field,settings.*(field.member)));
    out+=",\"glass2_blur_edge\":"+std::string(settings.blur_edge?"true":"false");
    return out;
}
} // namespace pulse::desktop

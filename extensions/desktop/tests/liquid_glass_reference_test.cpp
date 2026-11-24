#include <algorithm>
#include <cstdio>
#include <cmath>
#ifndef PULSE_OLD_MODEL
#include "host/model/desktop_model.h"
#include "host/render/liquid_glass_params.h"
#include "host/render/shaders/liquid_glass_optics.hlsli"
#else
#include "before/host/model/desktop_model.h"
#endif
int main() {
    using namespace pulse::desktop;
    int failed=0, count=0;
    auto check=[&](bool ok,const char* label){++count;failed+=!ok;std::printf("[%s] %s\n",ok?"PASS":"FAIL",label);};
    model::BeautifySettings settings;
    check(settings.glass_preset==5,"new defaults select explicit reference preset");
    check(settings.glass_thickness==20,"reference Controls thickness=20");
    check(settings.glass_ior_x100==140,"reference Controls IOR=1.4");
    check(settings.glass_dispersion==7,"reference Controls dispersion=7");
    check(settings.glass_glare==90&&settings.glass_glare_angle==-45,"reference Controls glare=90 angle=-45");
#ifndef PULSE_OLD_MODEL
    model::DesktopModelState saved, loaded;
    for (const auto& field : kGlassOpticalFields) saved.settings.glass_options.*(field.member)=field.min+(field.max-field.min)*0.37123;
    saved.settings.glass_options.blur_edge=false;
    const auto json=model::SerializeModelStateJson(saved);
    check(model::DeserializeModelStateJson(json,loaded),"continuous optical JSON parses");
    for (const auto& field : kGlassOpticalFields)
        check(std::abs(saved.settings.glass_options.*(field.member)-loaded.settings.glass_options.*(field.member))<1e-9,"fractional parameter survives save/reopen without integer quantization");
    check(!loaded.settings.glass_options.blur_edge&&loaded.settings.glass_reference,"mode and blur edge survive save/reopen");
    check(model::DeserializeModelStateJson("{\"version\":1,\"settings\":{\"glass_preset\":4,\"glass_thickness\":37,\"glass_ior\":178}}",loaded)&&!loaded.settings.glass_reference&&loaded.settings.glass_thickness==37,"legacy explicit custom fields preserved");
    check(model::DeserializeModelStateJson("{\"version\":1,\"settings\":{}}",loaded)&&loaded.settings.glass_reference&&loaded.settings.glass_preset==5,"missing optical settings initialize reference defaults");
    check(!model::DeserializeModelStateJson("{\"version\":1,\"settings\":{\"glass_schema\":2,\"glass2_ior\":\"NaN\"}}",loaded),"nonnumeric optical JSON rejected");
    check(!model::DeserializeModelStateJson("{\"version\":1,\"settings\":{\"glass_schema\":2,\"glass2_ior\":1e999}}",loaded),"nonfinite overflowing optical JSON rejected");
    check(model::DeserializeModelStateJson("{\"version\":1,\"settings\":{\"glass_schema\":2,\"glass2_ior\":-5.25,\"glass2_thickness\":1e300,\"glass2_tint_a\":-1}}",loaded)&&loaded.settings.glass_options.ior==1&&loaded.settings.glass_options.thickness==80&&loaded.settings.glass_options.tint_a==0,"finite out-of-range values clamp to declared ranges");
    for(const auto& field:kGlassOpticalFields) {
        check(ClampGlassValue(field,field.min-0.25)==field.min&&ClampGlassValue(field,field.max+0.25)==field.max,"each continuous field enforces declared range");
    }
    const auto default_input=render::BoundedGlassInputRect({100,100,420,356},{0,0,3840,2160},126);
    check(default_input.left==0&&default_input.top==0&&default_input.right==546&&default_input.bottom==482,"default sampling rectangle intersects real background");
    const auto maximum_input=render::BoundedGlassInputRect({1000,1000,1320,1256},{0,0,7680,4320},16384);
    check(maximum_input.Width()==1088&&maximum_input.Height()==1024&&render::GlassInputWithinBudget(maximum_input),"extreme request bounded to384px halo rather than viewport-sized allocation");
    check(!render::GlassInputWithinBudget({0,0,7680,4320}),"oversized input uses existing material fallback before effect allocation");
    namespace lg=render::lg_optics;
    check(settings.glass_reference&&settings.glass_blur_radius==1&&settings.glass_blur_edge,"reference blur=1 edge=true");
    render::GlassCardRequest request;
    request.right_dip=320;request.bottom_dip=256;request.reference_mode=true;
    request.thickness_dip=settings.glass_thickness;request.ior=settings.glass_ior_x100/100.0f;
    request.dispersion_gain=settings.glass_dispersion;request.glare_percent=settings.glass_glare;
    auto cb=render::MakeGlassDrawPlan(request,1).constants;
    check(cb.fresnel_factor==0.2f&&cb.fresnel_range==30&&cb.fresnel_hardness==0.2f,"reference Fresnel factor/range/hardness");
    check(cb.glare_range==30&&cb.glare_hardness==0.2f&&cb.convergence==0.5f&&cb.opposite_factor==0.8f,"reference glare range/hardness/convergence/opposite");
    check(cb.tint_r==1&&cb.tint_g==1&&cb.tint_b==1&&cb.tint_opacity==0&&cb.rim_alpha==0,"reference white transparent tint with no extra colored rim");
    for(float inner:{0.0f,0.5f,5.0f,10.0f,19.0f,20.0f,30.0f}) {
        const double ratio=std::clamp(1.0-double(inner)/20.0,0.0,1.0);
        const double incident=std::asin(ratio*ratio);
        const double expected=inner>=20?0:-std::tan(std::asin(std::sin(incident)/1.4)-incident);
        const float actual=lg::StudioSnell(inner,20,1.4f);
        check(std::isfinite(actual)&&std::abs(actual-expected)<1e-5,"Snell numeric agreement with independent GLSL formula");
    }
    for(float sd:{-20.0f,-4.0f,-1.0f,0.0f}) {
        const double base=std::max(0.0,1.0+sd/1500.0*std::pow(500.0/30,2)+0.2);
        check(std::abs(lg::StudioFalloff(sd,30,0.2f)-std::clamp(std::pow(base,5),0.0,1.0))<1e-5,"Fresnel falloff agrees with reference formula");
    }
    check(lg::StudioFalloff(-1,0,0.2f)==0&&std::isfinite(lg::StudioSnell(0,1,1)),"zero range and IOR1 remain finite");
    model::ApplyGlassPreset(2,settings);check(!settings.glass_reference&&settings.glass_thickness==44,"legacy preset remains opt-in unchanged");
    model::ApplyGlassPreset(5,settings);check(settings.glass_reference&&settings.glass_thickness==20,"reference preset resets all optical defaults");
    check(settings.glass_frost==model::BeautifySettings{}.glass_frost && settings.glass_options==model::BeautifySettings{}.glass_options,
          "reset after legacy preset matches fresh optical and frost defaults");
#endif
    printf("STUDIO_REFERENCE_RESULT cases=%d failed=%d\n",count,failed);return failed?1:0;
}

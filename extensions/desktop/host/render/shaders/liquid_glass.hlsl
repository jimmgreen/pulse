// liquid_glass.hlsl - Direct2D Custom Effect Pixel Shader (ps_4_0)
// Input 0: Sharp wallpaper / live capture aligned to target pixel space
// Input 1: Frosted wallpaper / live capture aligned to target pixel space

#define D2D_INPUT_COUNT 2
#define D2D_INPUT0_COMPLEX
#define D2D_INPUT1_COMPLEX
#define D2D_REQUIRES_SCENE_POSITION
#include "d2d1effecthelpers.hlsli"

cbuffer Constants : register(b0) {
    float4 c_rect_xywh;         // [0] x, y, w, h (target pixels)
    float4 c_shape_bezel;       // [1] radius, bezel, ref_ior, lens_scale
    float4 c_disp_frost_angle;  // [2] dispersion_gain, frost, glare_angle_rad, refraction_pad
    float4 c_glare_fresnel_clr; // [3] glare_factor, fresnel_factor, clear_interior, feather
    float4 c_ranges_dark_k;     // [4] glare_range, fresnel_range, dark_mode, smooth_k
    float4 c_nb_opp_tint_rg;    // [5] neighbor_count, opposite_factor, tint_r, tint_g
    float4 c_tint_ba_alpha_rim; // [6] tint_b, tint_opacity, overall_alpha, rim_alpha
    float4 c_nb_radii;          // [7] neighbor radii [0..3]
    float4 c_nb0;               // [8] (cx, cy, hw, hh)
    float4 c_nb1;               // [9]
    float4 c_nb2;               // [10]
    float4 c_nb3;               // [11]
    float4 c_reference;
    float4 c_studio_controls;
};

float4 LgSampleBackdrop(float2 off, float frost_amt) {
    float4 uv0 = D2DGetInputCoordinate(0);
    float4 uv1 = D2DGetInputCoordinate(1);
    float4 res = float4(0.0, 0.0, 0.0, 0.0);
    if (c_reference.x > 0.5 && c_reference.w <= 1.0 && frost_amt > 0.0) {
        float4 blurred = 0;
        const float side = 0.0108675415748; // exp(-4.5)/(1+2*exp(-4.5))
        const float middle = 1.0 - 2.0 * side;
        [unroll] for(int y=-1;y<=1;++y) [unroll] for(int x=-1;x<=1;++x) {
            float weight=(x==0?middle:side)*(y==0?middle:side);
            blurred += InputTexture0.SampleLevel(InputSampler0,uv0.xy+(off+float2(x,y))*uv0.zw,0)*weight;
        }
        float4 sharp=InputTexture0.SampleLevel(InputSampler0,uv0.xy+off*uv0.zw,0);
        res = lerp(sharp,blurred,frost_amt);
    } else if (frost_amt <= 0.002) {
        res = InputTexture0.SampleLevel(InputSampler0, uv0.xy + off * uv0.zw, 0.0);
    } else if (frost_amt >= 0.998) {
        res = InputTexture1.SampleLevel(InputSampler1, uv1.xy + off * uv1.zw, 0.0);
    } else {
        float4 s0 = InputTexture0.SampleLevel(InputSampler0, uv0.xy + off * uv0.zw, 0.0);
        float4 s1 = InputTexture1.SampleLevel(InputSampler1, uv1.xy + off * uv1.zw, 0.0);
        res = lerp(s0, s1, frost_amt);
    }
    return res;
}

#include "liquid_glass_optics.hlsli"

D2D_PS_ENTRY(main) {
    float4 result = float4(0,0,0,0);
    if (c_reference.x > 0.5) {
        result = LgShadeStudio(D2DGetScenePosition().xy,c_rect_xywh,c_shape_bezel,c_disp_frost_angle,
            c_glare_fresnel_clr,c_ranges_dark_k,c_reference,c_studio_controls,c_nb_opp_tint_rg,c_tint_ba_alpha_rim,
            c_nb_radii,c_nb0,c_nb1,c_nb2,c_nb3) * c_tint_ba_alpha_rim.z;
    } else {
    result = LgShadePixel(
        D2DGetScenePosition().xy,
        c_rect_xywh,
        c_shape_bezel,
        c_disp_frost_angle,
        c_glare_fresnel_clr,
        c_ranges_dark_k,
        c_nb_opp_tint_rg,
        c_tint_ba_alpha_rim,
        c_nb_radii,
        c_nb0, c_nb1, c_nb2, c_nb3);
    }
    return result;
}

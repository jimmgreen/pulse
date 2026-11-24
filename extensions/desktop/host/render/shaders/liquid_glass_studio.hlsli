// Adapted from local Liquid Glass Studio STEP 9 and lib/color.glsl.
// Copyright (c) 2024 Charles Yin, MIT; see THIRD_PARTY_NOTICES.md.
#ifdef __cplusplus
inline float studio_atan(float y, float x) { return std::atan2(y, x); }
inline float studio_asin(float x) { return std::asin(std::clamp(x, -1.0f, 1.0f)); }
inline float studio_tan(float x) { return std::tan(x); }
#else
#define studio_atan(y,x) atan2(y,x)
#define studio_asin(x) asin(clamp(x,-1.0,1.0))
#define studio_tan(x) tan(x)
#endif
inline float StudioLinear(float x) { return x > 0.04045f ? lg_pow((x + 0.055f) / 1.055f, 2.4f) : x / 12.92f; }
inline float StudioSrgb(float x) { return x <= 0.0031308f ? 12.92f * x : 1.055f * lg_pow(x, 0.41666666666f) - 0.055f; }
inline float StudioLabF(float x) { return x > 0.00885645167f ? lg_pow(x, 0.333333333f) : 7.78703703704f*x + 0.13793103448f; }
inline float StudioLabInv(float x) { return x > 0.206897f ? x*x*x : 0.12841854934f*(x-0.137931034f); }
inline float3 StudioToLch(float3 c) {
    float3 r = float3(StudioLinear(c.x), StudioLinear(c.y), StudioLinear(c.z));
    float x = StudioLabF((0.4124f*r.x+0.3576f*r.y+0.1805f*r.z)/0.95045592705f);
    float y = StudioLabF(0.2126f*r.x+0.7152f*r.y+0.0722f*r.z);
    float z = StudioLabF((0.0193f*r.x+0.1192f*r.y+0.9505f*r.z)/1.08905775076f);
    float a=500.0f*(x-y), b=200.0f*(y-z);
    return float3(116.0f*y-16.0f,lg_sqrt(a*a+b*b),studio_atan(b,a));
}
inline float3 StudioFromLch(float3 c) {
    float w=(c.x+16.0f)/116.0f;
    float x=0.95045592705f*StudioLabInv(w+c.y*lg_cos(c.z)/500.0f);
    float y=StudioLabInv(w);
    float z=1.08905775076f*StudioLabInv(w-c.y*lg_sin(c.z)/200.0f);
    return float3(StudioSrgb(3.2406255f*x-1.537208f*y-0.4986286f*z),
        StudioSrgb(-0.9689307f*x+1.8757561f*y+0.0415175f*z),
        StudioSrgb(0.0557101f*x-0.2040211f*y+1.0569959f*z));
}
inline float StudioSnell(float inner_dip, float thickness, float ior) {
    if (inner_dip >= thickness) return 0;
    float ratio=lg_clamp(1.0f-inner_dip/lg_max(thickness,0.001f),0.0f,1.0f);
    float incident=studio_asin(ratio*ratio);
    float transmitted=studio_asin(lg_sin(incident)/lg_max(ior,1.0f));
    return -studio_tan(transmitted-incident);
}
inline float StudioFalloff(float sd_dip, float range, float hardness) {
    if(range<=0) return 0;
    return lg_saturate(lg_pow(1.0f+sd_dip/1500.0f*(500.0f/range)*(500.0f/range)+hardness,5.0f));
}
inline float2 StudioBoundOffset(float2 offset,float limit) {
    return float2(lg_clamp(offset.x,-limit,limit),lg_clamp(offset.y,-limit,limit));
}
#ifdef __cplusplus
template<class SampleFn>
#endif
inline float4 LgShadeStudio(float2 scene, float4 rect, float4 shape, float4 dispersion,
    float4 glare_fresnel, float4 ranges, float4 reference, float4 controls,
    float4 neighbors, float4 tint_ba, float4 radii, float4 nb0,float4 nb1,float4 nb2,float4 nb3
#ifdef __cplusplus
    , SampleFn&& sample_fn
#endif
) {
    float2 half_sz=float2(rect.z*0.5f,rect.w*0.5f), center=float2(rect.x,rect.y)+half_sz;
    float self=0; bool owned=true;
    float sd=LgEvalFusedSdf(scene,center,half_sz,shape.x,(int)neighbors.x,ranges.w,nb0,nb1,nb2,nb3,radii,self,owned);
    if(!owned || sd>0.5f) return float4(0,0,0,0);
    float dpi=lg_max(reference.z,0.01f), inner=lg_max(-sd/dpi,0.0f);
    float edge=StudioSnell(inner,shape.y/dpi,shape.z);
    float3 tint=float3(neighbors.z,neighbors.w,tint_ba.x);
    float tint_alpha=lg_saturate(tint_ba.y);
    float3 col=lg_lerp3(LG_RGB(LG_SAMPLE(float2(0,0),1.0f)),tint,tint_alpha*0.8f);
    if(edge>0.0f) {
        // D2D Y grows down; GLSL's normal and angular formula use Y up.
        float eps=0.25f*dpi;
        float gx=LgEvalFusedSdfOnly(scene+float2(eps,0),center,half_sz,shape.x,(int)neighbors.x,ranges.w,nb0,nb1,nb2,nb3,radii)-LgEvalFusedSdfOnly(scene-float2(eps,0),center,half_sz,shape.x,(int)neighbors.x,ranges.w,nb0,nb1,nb2,nb3,radii);
        float gy=LgEvalFusedSdfOnly(scene+float2(0,eps),center,half_sz,shape.x,(int)neighbors.x,ranges.w,nb0,nb1,nb2,nb3,radii)-LgEvalFusedSdfOnly(scene-float2(0,eps),center,half_sz,shape.x,(int)neighbors.x,ranges.w,nb0,nb1,nb2,nb3,radii);
        float2 n=float2(gx,gy)/(2.0f*eps); float strength=lg_len2(n);
        float2 off=-n*(edge*0.05f*reference.y*dpi);
        float mix_rate=controls.w>0.5f?1.0f:lg_saturate(inner/(shape.y/dpi));
        float limit=lg_max(1.0f,dispersion.w-reference.w-2.0f);
        float3 red=LG_RGB(LG_SAMPLE(StudioBoundOffset(off*(1.0f+0.02f*dispersion.x),limit),mix_rate));
        float3 green=LG_RGB(LG_SAMPLE(StudioBoundOffset(off,limit),mix_rate));
        float3 blue=LG_RGB(LG_SAMPLE(StudioBoundOffset(off*(1.0f-0.02f*dispersion.x),limit),mix_rate));
        float3 refracted=float3(red.x,green.y,blue.z); col=lg_lerp3(refracted,tint,tint_alpha*0.8f);
        float fresnel=StudioFalloff(sd/dpi,ranges.y,controls.x);
        float3 white=StudioToLch(lg_lerp3(float3(1,1,1),tint,tint_alpha*0.5f)); white.x=lg_clamp(white.x+20.0f*fresnel*glare_fresnel.y,0.0f,100.0f);
        col=lg_lerp3(col,StudioFromLch(white),fresnel*glare_fresnel.y*0.7f*strength);
        float angle=studio_atan(-n.y,n.x); if(angle<0)angle+=6.28318530718f;
        angle=(angle-0.78539816339f+dispersion.z)*2.0f;
        bool far_side=(angle>4.71238898038f&&angle<10.9955742876f)||angle< -1.57079632679f;
        float angular=(0.5f+lg_sin(angle)*0.5f)*(far_side?1.2f*neighbors.y:1.2f)*glare_fresnel.x;
        angular=lg_saturate(lg_pow(angular,0.1f+controls.z*2.0f));
        float glare=angular*StudioFalloff(sd/dpi,ranges.x,controls.y);
        float3 lch=StudioToLch(lg_lerp3(refracted,tint,tint_alpha*0.5f));lch.x=lg_clamp(lch.x+150.0f*glare,0.0f,120.0f);lch.y+=30.0f*glare;
        col=lg_lerp3(col,StudioFromLch(lch),glare*strength);
    }
    float alpha=lg_saturate(0.5f-sd);
    return float4(lg_saturate3(col)*alpha,alpha);
}

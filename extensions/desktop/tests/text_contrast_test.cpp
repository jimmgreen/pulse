#include "host/render/text_contrast.h"
#include <cstdio>
#include <vector>
#include <limits>
using namespace pulse::desktop::render::text_contrast;
int main() {
    int failed=0, cases=0;
    auto check=[&](bool ok,const char* name){++cases; failed+=!ok; std::printf("[%s] %s\n",ok?"PASS":"FAIL",name);};
    auto near=[](float a,float b){return std::abs(a-b)<0.0001f;};
    check(near(RelativeLuminance({1,1,1}),1)&&near(RelativeLuminance({0,0,0}),0),"linear luminance endpoints");
    check(near(RelativeLuminance({0.5f,0.5f,0.5f}),0.214041f),"sRGB mid gray is decoded before luminance");
    check(near(LinearChannel(0.04045f),0.0031308f),"sRGB piecewise boundary");
    check(near(RelativeLuminance({1,0,0}),0.2126f)&&near(RelativeLuminance({0,1,0}),0.7152f),"saturated colors use linear weighted luminance");
    check(near(ContrastRatio(1,0),21),"black white contrast is 21");
    check(ChooseInk(Fill(0xFF000000)).light&&!ChooseInk(Fill(0xFFFFFFFF)).light,"black and white wallpaper select opposite ink");
    check(!ChooseInk(Fill(0xFF808080)).light,"actual near-black candidate wins on middle gray");
    check(!ChooseInk(Fill(0xFF00FF00)).light&&ChooseInk(Fill(0xFF0000FF)).light,"green and blue backgrounds use their distinct luminance");
    check(!ChooseInk({}).valid&&ChooseInk({},kLightInk,kDarkInk,true).light,"missing sample preserves explicit fallback");
    const auto opaque=ChooseInk(Fill(0xFF000000));
    const auto translucent=ChooseInk(Fill(0xFF000000),0x80FFFFFF);
    check(translucent.light_contrast<opaque.light_contrast&&translucent.light_contrast>1,"ink alpha is composed before contrast measurement");
    const auto invisible=ChooseInk(Fill(0xFF808080),0x00FFFFFF,0x001B2028,true);
    check(near(invisible.light_contrast,1)&&near(invisible.dark_contrast,1)&&invisible.light,"fully transparent ink has unit contrast and stable tie fallback");
    Samples mixed; mixed.count=100;
    for(size_t i=0;i<100;++i)mixed.colors[i]=i<20?Srgb{0,0,0}:Srgb{1,1,1};
    auto choice=ChooseInk(mixed);
    check(choice.light_contrast==1&&choice.dark_contrast<1.4f,"bimodal low-percentile scores expose poor local contrast instead of averaging");
    Samples tail;tail.count=100;
    for(size_t i=0;i<100;++i)tail.colors[i]=i<9?Srgb{1,1,1}:Srgb{0,0,0};
    check(near(ChooseInk(tail).light_contrast,21),"10th percentile tolerates fewer than ten percent bright outliers");
    tail.colors[9]={1,1,1};
    check(ChooseInk(tail).light_contrast==1,"10th percentile includes ten percent poor-contrast samples");
    mixed=Fill(0xFF000000);TransformSamples(mixed,[](Srgb c){return SourceOver(c,0x80FFFFFF);});
    check(near(mixed.colors[0].r,128.0f/255)&&RelativeLuminance(mixed.colors[0])<0.22f,"encoded source over precedes linear luminance");
    std::vector<uint8_t> image(31*31*4,255);
    auto sampled=SamplePbgra(image,31,31,0,0,31,31);
    check(sampled.count==256,"31x31 region is capped at 16x16 not 31x31");
    check(SamplePbgra(image,31,31,0,0,1,1).count==1,"one pixel produces one sample");
    check(SamplePbgra(image,31,31,-10,-10,2,2).count==4,"region clips to image");
    check(!SamplePbgra(image,31,31,32,0,40,2).count&&!SamplePbgra(image,31,31,2,2,1,1).count,"outside and inverted rectangles are empty");
    check(!SamplePbgra(image,31,31,0,0,std::numeric_limits<float>::infinity(),2).count,"invalid geometry is empty");
    check(!SamplePbgra(std::span<const uint8_t>(image.data(),4),31,31,0,0,31,31).count,"undersized cache is rejected");
    std::vector<uint8_t> photo(64*32*4);
    for(size_t y=0;y<32;++y)for(size_t x=0;x<64;++x) {
        const size_t n=(y*64+x)*4;
        photo[n]=photo[n+1]=photo[n+2]=(x<16||x>=48)?255:0;photo[n+3]=255;
    }
    auto grid=MakePhotoGrid(photo,64,32);
    check(grid.width==16&&grid.height==16&&grid.source_width==64&&grid.source_height==32,"photo cache keeps bounded grid and original aspect ratio");
    const auto cover=SamplePhotoGrid(grid,{0,0,100,100},{0,0,100,100});
    check(cover.colors.count==256&&ChooseInk(cover.colors).light,"wide photo Cover crop samples center black not white side margins");
    bool all_black=true;for(size_t i=0;i<cover.colors.count;++i)all_black &= cover.colors.colors[i].r==0;
    check(all_black,"Cover crop mapping matches center region exactly");
    for(size_t i=0;i<photo.size();i+=4){photo[i]=0;photo[i+1]=0;photo[i+2]=128;photo[i+3]=128;}
    const auto alpha=SamplePhotoGrid(MakePhotoGrid(photo,64,32),{0,0,100,100},{0,0,100,30});
    check(alpha.colors.count==256&&near(alpha.alpha[0],128.0f/255)&&near(alpha.colors.colors[0].r,1),"photo samples preserve alpha and unpremultiply color");
    check(!SamplePhotoGrid({}, {0,0,100,100},{0,0,10,10}).colors.count,"missing photo grid returns empty fallback");
    check(!SamplePhotoGrid(grid,{0,0,100,100},{110,0,120,10}).colors.count,"photo text outside tile returns empty fallback");
    check(near(PerceivedLuma({1,1,1}),1)&&near(PerceivedLuma({0,0,0}),0),"perceived luma endpoints");
    check(near(MeanPerceivedLuma(Fill(0xFF808080)),128.0f/255.0f)&&MeanPerceivedLuma({})<0,"mean perceived luma");
    check(!ResolveHysteresisLightInk(0.75f,false,false,true),"bright background > 0.60 selects dark ink regardless of theme");
    check(ResolveHysteresisLightInk(0.25f,false,false,false),"dark background < 0.40 selects light ink regardless of theme");
    check(!ResolveHysteresisLightInk(0.50f,false,true,false)&&ResolveHysteresisLightInk(0.50f,false,false,true),
          "initial deadband [0.40,0.60] follows system theme");
    check(ResolveHysteresisLightInk(0.55f,true,true,false)&&!ResolveHysteresisLightInk(0.45f,true,false,true),
          "subsequent deadband [0.40,0.60] retains previous ink without flicker");
    {
        // Clear menu glass veil: light over a backdrop that suits the ink, denser where it fights it.
        const Srgb dark_veil{0.11f,0.115f,0.135f}, light_veil{0.975f,0.98f,0.99f};
        const float on_night=SolveGlassVeilAlpha(Fill(0xFF101820),dark_veil,0xF0FFFFFFu,1.8f,0.30f,0.82f,0.6f,4.6f);
        const float on_snow=SolveGlassVeilAlpha(Fill(0xFFF4F6F8),dark_veil,0xF0FFFFFFu,1.8f,0.30f,0.82f,0.6f,4.6f);
        check(near(on_night,0.30f),"dark menu over a dark backdrop keeps the clear floor veil");
        check(on_snow>0.55f&&on_snow<=0.82f,"dark menu over a bright backdrop veils just enough");
        const auto after=[&](Srgb bg,Srgb veil,float a,uint32_t ink){const Srgb t=SourceOver(bg,veil,a);
            return ContrastRatio(RelativeLuminance(t),RelativeLuminance(SourceOver(t,ink)));};
        check(after({0.957f,0.965f,0.973f},dark_veil,on_snow,0xF0FFFFFFu)>=4.5f,"solved dark veil reaches WCAG AA");
        const float light_on_snow=SolveGlassVeilAlpha(Fill(0xFFF4F6F8),light_veil,0xEB0F1217u,1.8f,0.32f,0.82f,0.6f,4.6f);
        const float light_on_night=SolveGlassVeilAlpha(Fill(0xFF101820),light_veil,0xEB0F1217u,1.8f,0.32f,0.82f,0.6f,4.6f);
        check(near(light_on_snow,0.32f)&&light_on_night>0.4f,"light menu veils only over dark backdrops");
        check(after({0.063f,0.094f,0.125f},light_veil,light_on_night,0xEB0F1217u)>=4.5f,"solved light veil reaches WCAG AA");
        Samples mostly_dark;mostly_dark.count=100;
        for(size_t i=0;i<100;++i)mostly_dark.colors[i]=i<5?Srgb{1,1,1}:Srgb{0.06f,0.08f,0.1f};
        check(near(SolveGlassVeilAlpha(mostly_dark,dark_veil,0xF0FFFFFFu,1.8f,0.30f,0.82f,0.6f),0.30f),
              "a few bright specks under ten percent do not fog the whole menu");
        check(near(SolveGlassVeilAlpha({},dark_veil,0xF0FFFFFFu,1.8f,0.30f,0.82f,0.6f),0.6f),"missing samples use the fallback veil");
    }
    std::printf("Text contrast: %d cases, %d failures\n",cases,failed);return failed?1:0;
}


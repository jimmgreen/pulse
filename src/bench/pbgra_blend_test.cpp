#include "../preview_host/pbgra_blend.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <string_view>
#include <vector>

namespace {
void Reference(uint8_t* dst, const uint8_t* src, size_t pixels) {
    for (size_t i=0; i<pixels; ++i,src+=4,dst+=4) {
        const uint32_t a=src[3];
        if(a==255) {dst[0]=src[0];dst[1]=src[1];dst[2]=src[2];dst[3]=255;continue;}
        if(a==0) continue;
        const uint32_t inverse=255-a;
        dst[0]=static_cast<uint8_t>(src[0]+(dst[0]*inverse+127)/255);
        dst[1]=static_cast<uint8_t>(src[1]+(dst[1]*inverse+127)/255);
        dst[2]=static_cast<uint8_t>(src[2]+(dst[2]*inverse+127)/255);
        dst[3]=static_cast<uint8_t>(a+(dst[3]*inverse+127)/255);
    }
}
void Fill(std::vector<uint8_t>& pixels, std::mt19937& random, int mode) {
    for(size_t i=0;i+3<pixels.size();i+=4) {
        const uint8_t alpha=mode==1 ? uint8_t{255} : mode==2 ? uint8_t{0} : static_cast<uint8_t>(random());
        pixels[i+3]=alpha;
        for(size_t c=0;c<3;++c) pixels[i+c]=static_cast<uint8_t>(random()%(uint32_t{alpha}+1));
    }
}
}
int main(int argc, char** argv) {
    std::mt19937 random(73119);
    bool equal=true;
    for(size_t count=0;count<260;++count) for(size_t offset=0;offset<16;++offset) {
        std::vector<uint8_t> src(count*4+32), old(src.size()), next;
        Fill(src,random,0); Fill(old,random,0); next=old;
        Reference(old.data()+offset,src.data()+offset,count);
        pulse::preview::BlendPbgraSpan(next.data()+offset,src.data()+offset,count);
        equal=equal && old==next;
    }
    // Exhaust all alpha/target-byte pairs; lanes include valid and malformed
    // colors to verify the old byte wrap and transparent-source behavior too.
    std::vector<uint8_t> src(256*256*4),old(src.size()),next;
    for(size_t a=0;a<256;++a) for(size_t d=0;d<256;++d) {
        const size_t i=(a*256+d)*4;
        src[i]=static_cast<uint8_t>(a);src[i+1]=255;src[i+2]=0;src[i+3]=static_cast<uint8_t>(a);
        std::fill_n(old.data()+i,4,static_cast<uint8_t>(d));
    }
    next=old; Reference(old.data(),src.data(),src.size()/4);
    pulse::preview::BlendPbgraSpan(next.data(),src.data(),src.size()/4);
    equal=equal && old==next;
    printf("[%s] exhaustive alpha/target pairs, unaligned offsets 0..15, tails 0..259 and guard bytes\n",equal?"PASS":"FAIL");
    if(!equal) return 1;
    if(argc==2 && std::string_view(argv[1])=="--correctness") return 0;
    uint64_t checksum=0;
    for(const auto dims : {std::pair{64,64},std::pair{1920,1080},std::pair{3840,2160}}) {
        const size_t pixels=static_cast<size_t>(dims.first)*dims.second;
        src.resize(pixels*4);old.resize(src.size());next.resize(src.size());
        for(int mode=0;mode<3;++mode) {
            Fill(src,random,mode); Fill(old,random,0); next=old;
            std::vector<double> scalar,simd;
            const int iterations=pixels<10000 ? 500 : 3;
            for(int run=0;run<10;++run) for(int order=0;order<2;++order) {
                const bool optimized=(run+order)%2!=0;
                auto& dst=optimized?next:old;
                const auto start=std::chrono::steady_clock::now();
                for(int i=0;i<iterations;++i) {
                    if(optimized) pulse::preview::BlendPbgraSpan(dst.data(),src.data(),pixels);
                    else Reference(dst.data(),src.data(),pixels);
                }
                const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()/iterations;
                (optimized?simd:scalar).push_back(ms);
                checksum+=dst[(static_cast<size_t>(run)*4)%dst.size()];
            }
            if(old!=next) {printf("[FAIL] repeated blend differs\n");return 1;}
            printf("samples %dx%d alpha=%d scalar_ms=",dims.first,dims.second,mode);
            for(double sample:scalar) printf("%.6f,",sample);
            printf(" sse2_ms=");
            for(double sample:simd) printf("%.6f,",sample);
            printf("\n");
            std::sort(scalar.begin(),scalar.end());std::sort(simd.begin(),simd.end());
            printf("%dx%d alpha=%s scalar_ms=%.5f sse2_ms=%.5f ratio=%.2f\n",dims.first,dims.second,
                mode==0?"mixed":mode==1?"opaque":"transparent",scalar[5],simd[5],scalar[5]/simd[5]);
        }
    }
    printf("checksum=%llu\n",static_cast<unsigned long long>(checksum));
}

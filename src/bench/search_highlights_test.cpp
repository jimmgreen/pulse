#include "../ui/search_highlights.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <random>
#include <string_view>
size_t allocations=0,allocated_bytes=0;
void* operator new(size_t n) {if(void* p=std::malloc(n?n:1)){++allocations;allocated_bytes+=n;return p;}throw std::bad_alloc();}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,size_t) noexcept {std::free(p);}
using pulse::ui::SearchHighlight;
using pulse::ui::SearchHighlights;
bool Same(const std::vector<SearchHighlight>& a,const std::vector<SearchHighlight>& b){
    if(a.size()!=b.size())return false;
    for(size_t i=0;i<a.size();++i)if(a[i].start!=b[i].start||a[i].length!=b[i].length||a[i].current!=b[i].current)return false;
    return true;
}
std::vector<SearchHighlight> Collect(SearchHighlights view,uint64_t a,uint64_t b){
    std::vector<SearchHighlight> out;for(auto h:view.Intersecting(a,b))out.push_back(h);return out;
}
int main(int argc,char** argv){
    bool good=true;std::mt19937 rng(71037);
    for(int run=0;run<5000;++run){
        std::vector<uint32_t> offsets;for(int i=0;i<100;++i)offsets.push_back(rng()%1000);
        std::sort(offsets.begin(),offsets.end());
        const uint32_t length=rng()%40;const size_t current=rng()%120;
        const uint64_t a=rng()%1100,b=rng()%1100;
        std::vector<SearchHighlight> expected,legacy;
        for(size_t i=0;i<offsets.size();++i){
            SearchHighlight h{offsets[i],length,i==current};legacy.push_back(h);
            if(a<b&&length&&uint64_t{h.start}<b&&uint64_t{h.start}+length>a)expected.push_back(h);
        }
        good=Same(expected,Collect(SearchHighlights(offsets,length,current),a,b))&&Same(expected,Collect(legacy,a,b))&&good;
    }
    const std::vector<uint32_t> boundary{0,5,10,15,UINT32_MAX-2};
    good=Collect(SearchHighlights(boundary,5,1),5,10).size()==1&&good;
    const auto crossing=Collect(SearchHighlights(boundary,7,1),6,11);
    good=crossing.size()==3&&crossing[1].current&&good;
    good=Collect(SearchHighlights(boundary,8,4),UINT32_MAX,uint64_t{UINT32_MAX}+4).size()==1&&good;
    good=Collect(SearchHighlights(boundary,0,0),0,100).empty()&&Collect(SearchHighlights{},0,100).empty()&&good;
    std::vector<SearchHighlight> varying{{0,100,true},{1,1,false},{20,2,false}};
    good=Collect(varying,50,60).size()==1&&good;
    auto mutable_offsets=std::vector<uint32_t>{1};SearchHighlights borrowed(mutable_offsets,2,0);mutable_offsets[0]=8;
    good=Collect(borrowed,8,9).size()==1&&good;
    printf("[%s] 5000 randomized ranges, duplicates/current, cross-block overlaps, exact endpoints, empty/zero-query, 64bit end, explicit variable-length compatibility and borrowed ownership\n",good?"PASS":"FAIL");
    if(!good)return 1;
    if(argc==2&&std::string_view(argv[1])=="--correctness")return 0;
    std::vector<uint32_t> offsets(100000);for(size_t i=0;i<offsets.size();++i)offsets[i]=static_cast<uint32_t>(i*2);
    for(bool old_full_scan:{false,true}){
        std::vector<double> times[2];uint64_t checksums[2]{};size_t visits[2]{},counts[2]{},bytes[2]{};
        for(int sample=0;sample<5;++sample)for(int order=0;order<2;++order){
            const int kind=(sample+order)%2;const size_t old_alloc=allocations,old_bytes=allocated_bytes;
            uint64_t checksum=0;size_t visit=0;
            const auto start=std::chrono::steady_clock::now();
            for(int frame=0;frame<10;++frame){
                std::vector<SearchHighlight> legacy;
                if(kind==0){legacy.reserve(offsets.size());for(size_t i=0;i<offsets.size();++i)legacy.push_back({offsets[i],3,i==50000});}
                SearchHighlights view(offsets,3,50000);
                for(uint32_t block=0;block<40;++block){
                    const uint32_t a=100000+block*20,b=a+20;
                    if(kind==1){for(auto h:view.Intersecting(a,b)){++visit;checksum+=h.start+h.current;}}
                    else if(old_full_scan){for(const auto& h:legacy){++visit;if(h.start<b&&h.start+h.length>a)checksum+=h.start+h.current;}}
                    else {auto it=std::lower_bound(legacy.begin(),legacy.end(),a,[](const auto& h,uint32_t first){return h.start+h.length<=first;});
                        for(;it!=legacy.end()&&it->start<b;++it){++visit;checksum+=it->start+it->current;}}
                }
            }
            const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            const size_t nalloc=allocations-old_alloc,nbytes=allocated_bytes-old_bytes;
            times[kind].push_back(ms);checksums[kind]=checksum;visits[kind]=visit;counts[kind]=nalloc;bytes[kind]=nbytes;
            printf("sample mode=%s index=%d variant=%s ten_frames_ms=%.6f allocations=%zu allocated_bytes=%zu visited=%zu checksum=%llu\n",old_full_scan?"markdown":"table-tree",sample,kind?"borrowed":"old",ms,nalloc,nbytes,visit,static_cast<unsigned long long>(checksum));
        }
        if(checksums[0]!=checksums[1]||counts[1]!=0)return 1;
        std::sort(times[0].begin(),times[0].end());std::sort(times[1].begin(),times[1].end());
        printf("median mode=%s old_ms=%.6f borrowed_ms=%.6f ten_frame_allocations=%zu/%zu bytes=%zu/%zu visited=%zu/%zu\n",old_full_scan?"markdown":"table-tree",times[0][2],times[1][2],counts[0],counts[1],bytes[0],bytes[1],visits[0],visits[1]);
    }
}

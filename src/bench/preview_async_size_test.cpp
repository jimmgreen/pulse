#include "../ui/quick_preview_size_probe.h"
#include <atomic>
#include <cstdio>
#include <psapi.h>
#include <tlhelp32.h>
using pulse::ui::QuickPreviewSizeProbe;
int failures = 0;
void Check(bool ok, const char* label) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); if (!ok) ++failures; }
DWORD Threads() {
    DWORD count=0; HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
    THREADENTRY32 entry{sizeof(entry)};
    if(Thread32First(snapshot,&entry)) do { if(entry.th32OwnerProcessID==GetCurrentProcessId()) ++count; } while(Thread32Next(snapshot,&entry));
    CloseHandle(snapshot); return count;
}
void Resources(const char* phase) {
    DWORD handles=0; GetProcessHandleCount(GetCurrentProcess(),&handles);
    PROCESS_MEMORY_COUNTERS_EX counters{}; counters.cb=sizeof(counters); GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),sizeof(counters));
    std::printf("[RESOURCE] %s threads=%lu handles=%lu private_bytes=%zu\n",phase,Threads(),handles,counters.PrivateUsage);
}
int main() {
    Resources("before");
    auto entered=std::make_shared<std::atomic<bool>>(false);
    auto release=std::make_shared<std::atomic<bool>>(false);
    auto finished=std::make_shared<std::atomic<bool>>(false);
    auto delay=[=](QuickPreviewSizeProbe::Result& result) {
        if(result.path==L"slow") { *entered=true; while(!*release) Sleep(1); }
        result.width=640;result.height=480; *finished=true;
    };
#ifdef BASELINE_SYNC
    QuickPreviewSizeProbe::Result result{1,L"slow"};
    std::thread unblock([=]{Sleep(250);*release=true;});
    const auto begin=GetTickCount64(); delay(result); const auto elapsed=GetTickCount64()-begin;
    unblock.join(); std::printf("[TIME] synchronous header probe=%llu ms\n",elapsed);
    Check(elapsed<100,"slow header probe does not block caller");
#else
    {
        QuickPreviewSizeProbe probe(delay);
        const auto begin=GetTickCount64();probe.Start(1,L"slow");
        Check(GetTickCount64()-begin<100,"slow header probe does not block caller");
        for(int n=0;n<1000&&!*entered;++n)Sleep(1);
        Check(*entered,"delayed worker entered");
        const DWORD threads=Threads();
        for(uint64_t generation=2;generation<=1001;++generation)probe.Start(generation,L"latest");
        Check(Threads()==threads,"rapid switching retains a single worker");
        Check(!probe.Take(1001,L"latest"),"old in-flight size cannot resize current file");
        *release=true;
        std::optional<QuickPreviewSizeProbe::Result> result;
        for(int n=0;n<1000&&!result;++n) { result=probe.Take(1001,L"latest");Sleep(1); }
        Check(result&&result->width==640&&result->height==480,"latest generation receives dimensions");
        probe.Start(1002,L"latest"); Sleep(20);
        Check(!probe.Take(1001,L"latest"),"generation mismatch discards completion");
        probe.Start(1003,L"latest"); Sleep(20);
        Check(!probe.Take(1003,L"other"),"path mismatch discards completion");
        probe.Start(1004,L"latest");probe.Cancel();Sleep(20);
        Check(!probe.Take(1004,L"latest"),"close cancellation clears completion");
    }
    *entered=false;*release=false;*finished=false;
    auto probe=std::make_unique<QuickPreviewSizeProbe>(delay);probe->Start(2,L"slow");
    for(int n=0;n<1000&&!*entered;++n)Sleep(1);
    const auto begin=GetTickCount64();probe.reset();
    Check(GetTickCount64()-begin<100,"destruction does not wait for slow probe");
    *release=true;
    for(int n=0;n<1000&&!*finished;++n)Sleep(1);
    Check(*finished,"detached worker completes with owned state after destruction");
    for (int n=0;n<200 && Threads()>1;++n) Sleep(5);
    {
    *release=false;*entered=false;
    std::vector<std::unique_ptr<QuickPreviewSizeProbe>> blocked;
    unsigned admitted=0;
    for(unsigned i=0;i<20;++i) {
        auto item=std::make_unique<QuickPreviewSizeProbe>(delay);
        admitted+=item->Start(i,L"slow"); blocked.push_back(std::move(item));
    }
    Check(admitted==4,"process-wide header workers capped at four including blocked probes");
    blocked.clear();
    QuickPreviewSizeProbe extra(delay);
    Check(!extra.Start(50,L"slow"),"retired blocked header probes retain budget until actual exit");
    *release=true;
    for(int n=0;n<500 && !extra.Start(51,L"latest");++n) Sleep(2);
    std::optional<QuickPreviewSizeProbe::Result> reopened;
    for(int n=0;n<500 && !reopened;++n){reopened=extra.Take(51,L"latest");Sleep(2);}
    Check(reopened.has_value(),"header budget recovers after blocked probes finish");
    }
    {
    wchar_t filename[MAX_PATH]{};GetFullPathNameW(L"bench_data/preview_async_size_fix/owned-header.gif",MAX_PATH,filename,nullptr);
    const unsigned char gif[]={71,73,70,56,57,97,64,1,180,0,0,0,0};
    HANDLE f=CreateFileW(filename,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_TEMPORARY,nullptr);
    DWORD written=0;
    Check(f!=INVALID_HANDLE_VALUE && WriteFile(f,gif,sizeof(gif),&written,nullptr) && written==sizeof(gif),"create isolated real header fixture");CloseHandle(f);
    QuickPreviewSizeProbe real;real.Start(100,filename);
    std::optional<QuickPreviewSizeProbe::Result> dimensions;
    for(int n=0;n<500&&!dimensions;++n){dimensions=real.Take(100,filename);Sleep(2);}
    Check(dimensions && dimensions->width==320 && dimensions->height==180,"worker reads real GIF header from isolated file");
    DeleteFileW(filename);
    }
#endif
    for (int n=0; n<200 && Threads()>1; ++n) Sleep(5);
    Resources("after");
    std::printf("failures=%d\n",failures);return failures?1:0;
}

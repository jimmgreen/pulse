#include "../ui/ui_renderer.h"
#include <psapi.h>
#include <tlhelp32.h>
#include <cstdio>
using namespace pulse::ui;
static void Pump(unsigned ms) {
    const auto end = GetTickCount64() + ms;
    while (GetTickCount64() < end) { MSG msg{}; while (GetTickCount64() < end && PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {TranslateMessage(&msg);DispatchMessageW(&msg);} Sleep(1); }
}
static void Resource(const char* phase) {
    PROCESS_MEMORY_COUNTERS_EX m{};m.cb=sizeof(m);GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m),sizeof(m));
    DWORD handles=0,threads=0;GetProcessHandleCount(GetCurrentProcess(),&handles);
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);THREADENTRY32 e{sizeof(e)};
    if(Thread32First(snap,&e))do{if(e.th32OwnerProcessID==GetCurrentProcessId())++threads;}while(Thread32Next(snap,&e));CloseHandle(snap);
    printf("RESOURCE %s private=%zu handles=%lu threads=%lu\n",phase,m.PrivateUsage,handles,threads);
}
int wmain() {
    setvbuf(stdout,nullptr,_IONBF,0);
    int fails=0; auto check=[&](bool ok,const char* text){printf("[%s] %s\n",ok?"PASS":"FAIL",text);fails+=!ok;};
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    HWND a=CreateWindowW(L"STATIC",L"wall A",WS_OVERLAPPEDWINDOW,0,0,800,600,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    HWND b=CreateWindowW(L"STATIC",L"wall B",WS_OVERLAPPEDWINDOW,0,0,800,600,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Resource("before");
    {
        MainRenderer keeper; keeper.SetIconNotifyWindow(b);
        PaneViewModel vm;vm.wall=true;vm.view_mode=ViewMode::ExtraLargeIcons;vm.path=L"C:\\wall-fixture";vm.wall_view_identity=2;
        auto kept=std::make_shared<std::vector<pulse::fs::DirEntry>>(20000);for(auto& e:*kept)e.name=L"owned.txt";
        vm.snapshot=kept;std::weak_ptr<const std::vector<pulse::fs::DirEntry>> other=kept;
        keeper.ReconcileWallAnchor(vm,D2D1::RectF(0,0,800,600));
        unsigned released=0;
        for(unsigned cycle=0;cycle<20;++cycle){
            MainRenderer closing;closing.SetIconNotifyWindow(a);
            auto source=std::make_shared<std::vector<pulse::fs::DirEntry>>(20000);for(auto& e:*source)e.name=L"owned.txt";
            PaneViewModel current=vm;current.snapshot=source;current.wall_view_identity=100+cycle;
            std::weak_ptr<const std::vector<pulse::fs::DirEntry>> weak=source;
            closing.ReconcileWallAnchor(current,D2D1::RectF(0,0,800,600));Pump(20);
            current.snapshot.reset();source.reset();closing.SetIconNotifyWindow(nullptr);
            for(unsigned n=0;n<100&&!weak.expired();++n)Pump(2);
            released+=weak.expired();
        }
        check(released==20,"closing 20 large-directory renderers releases every snapshot before destruction");
        check(!other.expired(),"closing another window preserves live window snapshot");
        vm.snapshot.reset();kept.reset();keeper.SetIconNotifyWindow(nullptr);Pump(100);
        check(other.expired(),"last registered owner releases its own snapshot");
    }
    Pump(2400);Resource("after");DestroyWindow(a);DestroyWindow(b);CoUninitialize();return fails?1:0;
}

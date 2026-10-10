#include <windows.h>
#include "../shell_host/ctx_handlers.h"
#include <atomic>
#include <mutex>
#include <set>
#include <cstdio>
static std::atomic<unsigned> event_calls{0},thread_calls{0};
static unsigned fail_event=0;
static bool fail_thread=false;
static std::mutex handles_mutex;
static std::set<HANDLE> events;
static HANDLE entered=nullptr,release_query=nullptr;
static std::atomic<bool> block_query{false};
static HANDLE WINAPI TestEvent(LPSECURITY_ATTRIBUTES attributes,BOOL manual,BOOL initial,LPCWSTR name){
    if(++event_calls==fail_event){SetLastError(ERROR_NOT_ENOUGH_MEMORY);return nullptr;}
    HANDLE event=CreateEventW(attributes,manual,initial,name);
    if(event){std::lock_guard lock(handles_mutex);events.insert(event);}return event;
}
static HANDLE WINAPI TestThread(LPSECURITY_ATTRIBUTES attributes,SIZE_T stack,LPTHREAD_START_ROUTINE entry,LPVOID data,DWORD flags,LPDWORD id){
    ++thread_calls;if(fail_thread){SetLastError(ERROR_NOT_ENOUGH_MEMORY);return nullptr;}
    return CreateThread(attributes,stack,entry,data,flags,id);
}
static BOOL WINAPI TestClose(HANDLE handle){
    {std::lock_guard lock(handles_mutex);events.erase(handle);}return CloseHandle(handle);
}
namespace pulse::shell {
static std::vector<CtxHandlerDesc> TestHandlers(bool,const std::wstring&,const std::vector<std::wstring>&){CtxHandlerDesc desc;desc.clsid_text=L"isolated-test-handler";return {desc};}
static bool TestBind(const std::vector<std::wstring>&,bool,CtxBind&){if(block_query){SetEvent(entered);WaitForSingleObject(release_query,10000);}return false;}
}
#define CreateEventW TestEvent
#define CreateThread TestThread
#define CloseHandle TestClose
#define EnumerateCtxHandlers TestHandlers
#define BindCtxSelection TestBind
#define wWinMain AuditShellMain
#include "../shell_host/main.cpp"
#undef wWinMain
#undef BindCtxSelection
#undef EnumerateCtxHandlers
#undef CloseHandle
#undef CreateThread
#undef CreateEventW
// Production entry with its default API, as the crash-guarded thread calls it.
static DWORD WINAPI SessionEntry(LPVOID data){return CtxSessionThreadImpl(data);}
int wmain(){
    int failures=0;
    auto check=[&](bool ok,const char* label){printf("[%s] %s\n",ok?"PASS":"FAIL",label);fflush(stdout);failures+=!ok;};
    auto empty=[](){std::lock_guard lock(handles_mutex);return events.empty();};
    for(unsigned mode:{1u,2u,3u,0u}){
        event_calls=thread_calls=0;fail_event=mode<=2?mode:0;fail_thread=mode==3;
        bool started=false;
        {
            auto worker=std::make_unique<HandlerWorker>();started=StartHandlerWorker(*worker);
            if(started){std::vector<std::unique_ptr<HandlerWorker>> workers;workers.push_back(std::move(worker));JoinHandlerWorkers(workers);}
        }
        check(mode? !started:started,"worker event/thread failure and success results are explicit");
        check(mode==1||mode==2?thread_calls==0:thread_calls==1,"worker never starts before both events exist");
        check(empty(),"worker startup failure or join releases every acquired event");
    }
    fail_event=0;fail_thread=false;block_query=true;
    entered=CreateEventW(nullptr,TRUE,FALSE,nullptr);release_query=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    for(uint32_t sid=1;sid<=4;++sid){
        ResetEvent(entered);ResetEvent(release_query);
        {std::lock_guard lock(g_ctx_mutex);g_ctx_sessions[sid]={};}
        auto* data=new CtxSessionData;data->session_id=sid;data->paths={L"C:\\isolated-test.fixture"};
        DWORD thread_id=0;HANDLE thread=CreateThread(nullptr,0,SessionEntry,data,0,&thread_id);
        const bool querying=thread&&WaitForSingleObject(entered,2000)==WAIT_OBJECT_0;
        check(querying,"controlled handler entered production session query");
        if(thread)PostThreadMessageW(thread_id,WM_CTX_CLOSE,0,0);
        const auto deadline=GetTickCount64()+2000;
        while(!SessionCloseRequested(sid)&&GetTickCount64()<deadline)Sleep(5);
        const bool consumed=SessionCloseRequested(sid);
        SetEvent(release_query);
        const bool ended=thread&&WaitForSingleObject(thread,2000)==WAIT_OBJECT_0;
        check(consumed&&ended,"Close consumed during query reaches cleanup without 120-second session wait");
        if(thread&&!ended){PostThreadMessageW(thread_id,WM_CTX_CLOSE,0,0);WaitForSingleObject(thread,3000);}
        if(thread)CloseHandle(thread);
        check(empty(),"repeated early closes return worker event handles to baseline");
    }
    CloseHandle(entered);CloseHandle(release_query);return failures?1:0;
}

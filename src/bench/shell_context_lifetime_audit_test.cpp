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
static std::atomic<uint32_t> routed_id{0};
static DWORD WINAPI RouteWorker(LPVOID param) {
    auto* worker = static_cast<HandlerWorker*>(param);
    MSG msg{};
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    worker->thread_id = GetCurrentThreadId();
    CtxItemOut item;
    item.id = worker->id_first + 7;
    item.verb = L"isolated-verb";
    item.text = L"Isolated action";
    worker->items.push_back(item);
    SetEvent(worker->done_event);
    SetEvent(entered);
    for (;;) {
        MsgWaitForMultipleObjects(1, &worker->exit_event, FALSE, 3000, QS_ALLINPUT);
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_CTX_INVOKE) {
                std::unique_ptr<CtxInvokeMsg> invoke(reinterpret_cast<CtxInvokeMsg*>(msg.lParam));
                routed_id = invoke->item_id;
                return 0;
            }
        }
        if (WaitForSingleObject(worker->exit_event, 0) == WAIT_OBJECT_0) return 0;
    }
}
static HANDLE WINAPI RouteThread(LPSECURITY_ATTRIBUTES attributes, SIZE_T stack,
    LPTHREAD_START_ROUTINE, LPVOID data, DWORD flags, LPDWORD id) {
    return CreateThread(attributes, stack, RouteWorker, data, flags, id);
}
static DWORD WINAPI RouteSession(LPVOID data) {
    CtxSessionApi api;
    api.worker.create_thread = RouteThread;
    return CtxSessionThreadImpl(data, api);
}
static std::vector<pulse::shell::CtxHandlerDesc> MultiEnumerate(bool, const std::wstring&, const std::vector<std::wstring>&) {
    pulse::shell::CtxHandlerDesc w1, w2;
    w1.clsid_text = L"{11111111-0000-0000-0000-000000000001}";
    w2.clsid_text = L"{11111111-0000-0000-0000-000000000002}";
    return {w1, w2};
}
static std::mutex multi_mutex;
static std::vector<CtxItemOut> multi_final_items;
static std::atomic<bool> multi_sent_final{false};
static std::atomic<int> multi_invoked_worker{0};
static std::atomic<uint32_t> multi_routed_raw_id{0};
static void MultiCaptureSendItems(uint32_t, const std::vector<CtxItemOut>& items, uint32_t flags,
                                  const std::vector<std::wstring>&,
                                  const std::vector<pulse::ipc::CtxHandlerStatus>&) {
    if (flags & CTX_ITEMS_PARTIAL) return;
    std::lock_guard lock(multi_mutex);
    multi_final_items = items;
    multi_sent_final.store(true);
}
static DWORD WINAPI MultiRouteWorker(LPVOID param) {
    auto* worker = static_cast<HandlerWorker*>(param);
    MSG msg{};
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    worker->thread_id = GetCurrentThreadId();
    const bool is_second = worker->desc.clsid_text == L"{11111111-0000-0000-0000-000000000002}";
    auto add_item = [&](uint32_t raw_id, const wchar_t* verb, const wchar_t* text) {
        CtxItemOut item;
        item.id = raw_id;
        item.verb = verb;
        item.text = text;
        item.clsid = worker->desc.clsid_text;
        worker->items.push_back(item);
    };
    if (!is_second) {
        add_item(1, L"w1_1", L"W1_1");
        add_item(2, L"w1_2", L"W1_2");
        add_item(300, L"w1_300", L"W1_300");
    } else {
        add_item(1, L"w2_1", L"W2_1");
        add_item(2, L"w2_2", L"W2_2");
        add_item(40001, L"w2_40001", L"W2_40001");
    }
    SetEvent(worker->done_event);
    for (;;) {
        MsgWaitForMultipleObjects(1, &worker->exit_event, FALSE, 3000, QS_ALLINPUT);
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_CTX_INVOKE) {
                std::unique_ptr<CtxInvokeMsg> invoke(reinterpret_cast<CtxInvokeMsg*>(msg.lParam));
                multi_invoked_worker = is_second ? 2 : 1;
                multi_routed_raw_id = invoke->item_id;
                return 0;
            }
        }
        if (WaitForSingleObject(worker->exit_event, 0) == WAIT_OBJECT_0) return 0;
    }
}
static HANDLE WINAPI MultiRouteThread(LPSECURITY_ATTRIBUTES attributes, SIZE_T stack,
    LPTHREAD_START_ROUTINE, LPVOID data, DWORD flags, LPDWORD id) {
    return CreateThread(attributes, stack, MultiRouteWorker, data, flags, id);
}
static DWORD WINAPI MultiRouteSession(LPVOID data) {
    CtxSessionApi api;
    api.enumerate = MultiEnumerate;
    api.send_items = MultiCaptureSendItems;
    api.worker.create_thread = MultiRouteThread;
    return CtxSessionThreadImpl(data, api);
}

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
    block_query = false;
    for (uint32_t mode = 0; mode != 3; ++mode) {
        const uint32_t sid = 100 + mode;
        ResetEvent(entered);
        routed_id = 0;
        { std::lock_guard lock(g_ctx_mutex); g_ctx_sessions[sid] = {}; }
        auto* data = new CtxSessionData;
        data->session_id = sid;
        data->paths = {L"C:\\isolated-test.fixture"};
        HANDLE thread = CreateThread(nullptr, 0, RouteSession, data, 0, nullptr);
        const bool ready = thread && WaitForSingleObject(entered, 2000) == WAIT_OBJECT_0;
        if (ready) {
            auto* invoke = new CtxInvokeMsg;
            invoke->item_id = mode == 0 ? 0 : mode == 1 ? 9999 : kCtxIdFirst + 7;
            if (mode == 0) invoke->verb = L"isolated-verb";
            if (mode == 1) invoke->text = L"Isolated action";
            PostCtxMessage(sid, WM_CTX_INVOKE, 0, reinterpret_cast<LPARAM>(invoke));
        }
        const bool ended = thread && WaitForSingleObject(thread, 3000) == WAIT_OBJECT_0;
        check(ready && ended && routed_id == kCtxIdFirst + 7,
            mode == 0 ? "verb-resolved ID reaches handler" :
            mode == 1 ? "text-resolved ID reaches handler" : "live ID reaches handler unchanged");
        if (thread && !ended) { PostCtxMessage(sid, WM_CTX_CLOSE, 0, 0); WaitForSingleObject(thread, 3000); }
        if (thread) CloseHandle(thread);
        check(empty(), "invoke route releases worker events");
    }
    {
        struct FakeModelessMenu final : IContextMenu {
            HWND created = nullptr;
            ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
            ULONG STDMETHODCALLTYPE Release() override { return 1; }
            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
                if (!ppv) return E_POINTER;
                if (riid == IID_IUnknown || riid == IID_IContextMenu) { *ppv = this; return S_OK; }
                *ppv = nullptr;
                return E_NOINTERFACE;
            }
            HRESULT STDMETHODCALLTYPE QueryContextMenu(HMENU, UINT, UINT, UINT, UINT) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE GetCommandString(UINT_PTR, UINT, UINT*, CHAR*, UINT) override { return E_NOTIMPL; }
            HRESULT STDMETHODCALLTYPE InvokeCommand(CMINVOKECOMMANDINFO*) override {
                created = CreateWindowExW(0, L"STATIC", L"OpenHashTabFixture", WS_POPUP | WS_VISIBLE,
                                          -30000, -30000, 80, 40, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
                if (created) SetTimer(created, 1, 80, [](HWND hwnd, UINT, UINT_PTR id, DWORD) {
                    KillTimer(hwnd, id);
                    DestroyWindow(hwnd);
                });
                return created ? S_OK : E_FAIL;
            }
        } menu;
        CtxSessionData d;
        d.paths = {L"C:\\isolated-test.fixture"};
        const ULONGLONG t0 = GetTickCount64();
        CtxInvoke(d, &menu, kCtxIdFirst, 777, kCtxIdFirst);
        const ULONGLONG elapsed = GetTickCount64() - t0;
        check(menu.created != nullptr && elapsed >= 50 && !IsWindow(menu.created),
              "CtxInvoke keeps pumping a modeless invoke window until it closes (#618)");
    }
    {
        for (int run = 0; run < 2; ++run) {
            const uint32_t sid = static_cast<uint32_t>(180 + run);
            multi_sent_final.store(false);
            multi_invoked_worker = 0;
            multi_routed_raw_id = 0;
            { std::lock_guard lock(multi_mutex); multi_final_items.clear(); }
            { std::lock_guard lock(g_ctx_mutex); g_ctx_sessions[sid] = {}; }
            auto* data = new CtxSessionData;
            data->session_id = sid;
            data->paths = {L"C:\\isolated-test.fixture"};
            HANDLE thread = CreateThread(nullptr, 0, MultiRouteSession, data, 0, nullptr);
            const auto deadline = GetTickCount64() + 3000;
            while (!multi_sent_final.load() && GetTickCount64() < deadline) Sleep(5);
            std::vector<CtxItemOut> snapshot;
            { std::lock_guard lock(multi_mutex); snapshot = multi_final_items; }
            std::set<uint32_t> unique_wire_ids;
            uint32_t target_wire = 0;
            const std::wstring target_text = run == 0 ? L"W2_40001" : L"W2_1";
            const uint32_t expected_raw = run == 0 ? 40001u : 1u;
            bool wire_bounds_ok = snapshot.size() == 6;
            for (const auto& item : snapshot) {
                if (item.id < kCtxIdFirst || item.id > 0x7EFFu) wire_bounds_ok = false;
                unique_wire_ids.insert(item.id);
                if (item.text == target_text) target_wire = item.id;
            }
            wire_bounds_ok = wire_bounds_ok && unique_wire_ids.size() == 6 && target_wire != 0;
            if (wire_bounds_ok) {
                auto* inv = new CtxInvokeMsg;
                inv->item_id = target_wire;
                PostCtxMessage(sid, WM_CTX_INVOKE, 0, reinterpret_cast<LPARAM>(inv));
            } else if (thread) {
                PostCtxMessage(sid, WM_CTX_CLOSE, 0, 0);
            }
            const bool ended = thread && WaitForSingleObject(thread, 4000) == WAIT_OBJECT_0;
            if (thread && !ended) { PostCtxMessage(sid, WM_CTX_CLOSE, 0, 0); WaitForSingleObject(thread, 3000); }
            if (thread) CloseHandle(thread);
            check(wire_bounds_ok && ended && multi_invoked_worker == 2 &&
                      multi_routed_raw_id == expected_raw,
                  run == 0
                      ? "multi-worker wire IDs isolate high resource IDs and deliver raw 40001 to worker 2"
                      : "multi-worker wire IDs resolve colliding raw ID 1 unambiguously to worker 2");
            check(empty(), "multi-worker route releases all worker events");
        }
    }
    {
        static std::atomic<bool> hidden_timer_fired{false};
        hidden_timer_fired.store(false);
        struct FakeHiddenInvokeMenu final : IContextMenu {
            HWND created = nullptr;
            bool info_ok = false;
            ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
            ULONG STDMETHODCALLTYPE Release() override { return 1; }
            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
                if (!ppv) return E_POINTER;
                if (riid == IID_IUnknown || riid == IID_IContextMenu) { *ppv = this; return S_OK; }
                *ppv = nullptr;
                return E_NOINTERFACE;
            }
            HRESULT STDMETHODCALLTYPE QueryContextMenu(HMENU, UINT, UINT, UINT, UINT) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE GetCommandString(UINT_PTR, UINT, UINT*, CHAR*, UINT) override { return E_NOTIMPL; }
            HRESULT STDMETHODCALLTYPE InvokeCommand(CMINVOKECOMMANDINFO* ci) override {
                if (ci && ci->cbSize >= sizeof(CMINVOKECOMMANDINFOEX)) {
                    const auto* ex = reinterpret_cast<const CMINVOKECOMMANDINFOEX*>(ci);
                    const DWORD req = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
                    info_ok = (ex->fMask & req) == req &&
                              ex->lpDirectory != nullptr && ex->lpDirectoryW != nullptr;
                }
                created = CreateWindowExW(0, L"STATIC", L"PulseHiddenInvokeTimer", WS_POPUP,
                                          -30000, -30000, 10, 10, nullptr, nullptr,
                                          GetModuleHandleW(nullptr), nullptr);
                if (created) {
                    SetTimer(created, 1, 120, [](HWND hwnd, UINT, UINT_PTR id, DWORD) {
                        KillTimer(hwnd, id);
                        hidden_timer_fired.store(true);
                        DestroyWindow(hwnd);
                    });
                }
                return created ? S_OK : E_FAIL;
            }
        } menu;
        CtxSessionData d;
        d.paths = {L"C:\\isolated\\fixture.txt"};
        CtxInvoke(d, &menu, kCtxIdFirst, 778, kCtxIdFirst);
        check(menu.info_ok && menu.created != nullptr &&
                  hidden_timer_fired.load() && !IsWindow(menu.created),
              "CtxInvoke populates ANSI/Unicode dirs + ptInvoke and pumps hidden post-invoke timer window");
    }
    {
        const uint32_t sid = 250;
        ResetEvent(entered);
        routed_id = 0;
        { std::lock_guard lock(g_ctx_mutex); g_ctx_sessions[sid] = {}; }
        auto* inv = new CtxInvokeMsg;
        inv->item_id = kCtxIdFirst + 7;
        PostCtxMessage(sid, WM_CTX_INVOKE, 0, reinterpret_cast<LPARAM>(inv));
        auto* data = new CtxSessionData;
        data->session_id = sid;
        data->paths = {L"C:\\isolated-test.fixture"};
        HANDLE thread = CreateThread(nullptr, 0, RouteSession, data, 0, nullptr);
        const bool ended = thread && WaitForSingleObject(thread, 4000) == WAIT_OBJECT_0;
        if (thread && !ended) { PostCtxMessage(sid, WM_CTX_CLOSE, 0, 0); WaitForSingleObject(thread, 3000); }
        if (thread) CloseHandle(thread);
        check(ended && routed_id == kCtxIdFirst + 7 && empty(),
              "early WM_CTX_INVOKE while thread_id==0 queues and executes once session initializes");
    }
    CloseHandle(entered);CloseHandle(release_query);return failures?1:0;
}

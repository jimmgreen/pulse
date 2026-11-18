#include "file_dialog_remote_provider.h"
#include "../ipc/picker_remote_protocol.h"
#include "../ipc/picker_transport.h"
#include "../common/picker_owner_guard.h"
#include <objbase.h>
#include <atomic>
#include <thread>
#include <cstring>
#include <functional>

namespace pulse::shell_bridge {
namespace {
namespace wire = ipc::picker::remote;
using ipc::picker::PipeHandle;
using ipc::picker::TransportResult;
struct Context;
struct Binding;
thread_local std::weak_ptr<Binding> pending;
thread_local std::shared_ptr<Context> active;
std::atomic<uint64_t> tokens{1};
bool Sta() { APTTYPE type; APTTYPEQUALIFIER qualifier; return SUCCEEDED(CoGetApartmentType(&type,&qualifier)) && (type==APTTYPE_STA || type==APTTYPE_MAINSTA); }
struct Context {
    DWORD thread=GetCurrentThreadId(),pid=0;
    uint64_t token=tokens.fetch_add(1),sequence=0,event_sequence=0;
    ipc::picker::RequestId id{};
    ipc::picker::Channel channel;
    PipeHandle process,job;
    bool initialized=false,showing=false,callback=false,io=false,dead=false,quit=false;
    int quit_code=0;
    PickerOwnerGuard* owner=nullptr;
    uint64_t show_id=0,window=0;
    bool close_requested=false;
    HRESULT close_result=S_OK;
    uint64_t pending_close=0; // sequence of a Close sent while Show waits for the host
    wire::Snapshot accepted;
    ~Context() { if(quit) PostQuitMessage(quit_code); }
    bool Window(uint64_t value) const { DWORD actual=0; return value && value<=UINTPTR_MAX && GetWindowThreadProcessId(reinterpret_cast<HWND>(static_cast<uintptr_t>(value)),&actual) && actual==pid; }
    HRESULT Io(const std::function<TransportResult(HANDLE)>& operation) {
        if(io) return RPC_E_CALL_REJECTED;
        if(dead || quit) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        PipeHandle done(CreateEventW(nullptr,TRUE,FALSE,nullptr)),stop(CreateEventW(nullptr,TRUE,FALSE,nullptr));
        if(!done || !stop) return HRESULT_FROM_WIN32(GetLastError());
        io=true; struct Reset { bool& v; ~Reset(){v=false;} } reset{io};
        TransportResult result{ERROR_GEN_FAILURE};
        std::thread worker([&]{try {result=operation(stop.Get());}catch(...){result={ERROR_NOT_ENOUGH_MEMORY};} SetEvent(done.Get());});
        DWORD error=0;
        for(;;) {
            HANDLE handle=done.Get();
            DWORD wait=MsgWaitForMultipleObjectsEx(1,&handle,25,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
            if(owner && !owner->Alive()){error=ERROR_CANCELLED;break;}
            if(wait==WAIT_OBJECT_0) break;
            if(wait==WAIT_FAILED){error=GetLastError();break;}
            MSG message{};
            while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){
                if(message.message==WM_QUIT){quit=true;quit_code=static_cast<int>(message.wParam);error=ERROR_CANCELLED;break;}
                TranslateMessage(&message);DispatchMessageW(&message);
                if(owner && !owner->Alive()){error=ERROR_CANCELLED;break;}
                if(WaitForSingleObject(done.Get(),0)==WAIT_OBJECT_0)break;
            }
            if(error)break;
        }
        if(error)SetEvent(stop.Get());
        worker.join();
        MSG pending_quit{};
        if(!quit && PeekMessageW(&pending_quit,nullptr,WM_QUIT,WM_QUIT,PM_REMOVE)){quit=true;quit_code=static_cast<int>(pending_quit.wParam);}
        if(quit || (owner && !owner->Alive()))error=ERROR_CANCELLED;
        if(error || !result){dead=true;job.Reset();return HRESULT_FROM_WIN32(error ? error:result.error);}
        return S_OK;
    }
    HRESULT Send(wire::Message& m){
        std::vector<uint8_t> bytes;if(!wire::Encode(m,bytes))return E_INVALIDARG;
        return Io([&](HANDLE stop){return channel.SendFrame(bytes,{stop,10000});});
    }
    HRESULT Receive(wire::Message& m,DWORD timeout=INFINITE){
        std::vector<uint8_t> bytes;HRESULT hr=Io([&](HANDLE stop){return channel.ReceiveFrame(bytes,{stop,timeout});});
        if(FAILED(hr))return hr;
        if(!wire::Decode(bytes,m)||m.request_id!=id){dead=true;return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);}
        return S_OK;
    }
    HRESULT Call(wire::Message& m,wire::Message& reply){
        if(io)return RPC_E_CALL_REJECTED;
        if(sequence==UINT64_MAX)return E_UNEXPECTED;
        m.request_id=id;m.sequence=++sequence;
        HRESULT hr=Send(m);if(SUCCEEDED(hr))hr=Receive(reply,10000);
        if(FAILED(hr))return hr;
        if(reply.kind!=wire::Kind::Reply || reply.sequence!=m.sequence){dead=true;return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);}
        return static_cast<HRESULT>(reply.hresult);
    }
};
struct Binding {
    std::wstring executable;
    DWORD thread=GetCurrentThreadId();
    uint64_t capabilities=0,features=0;
    std::shared_ptr<Context> current;
};
HRESULT Start(const std::wstring&,const std::shared_ptr<Context>&,wire::Message&);
HRESULT Get(PulsePickerUiV2Session token,std::shared_ptr<Context>& c){
    c=active;if(!c)return RPC_E_WRONG_THREAD;if(c->token!=token)return E_INVALIDARG;
    if(c->thread!=GetCurrentThreadId() || !Sta())return RPC_E_WRONG_THREAD;
    if(c->io)return RPC_E_CALL_REJECTED;return S_OK;
}
template<class F> HRESULT Boundary(F&& f) noexcept {try{return f();}catch(...){return E_UNEXPECTED;}}
HRESULT WINAPI Initialize(const PulsePickerUiV2Initialize* init,PulsePickerUiV2Session* out){return Boundary([&]()->HRESULT{
    if(!Sta())return RPC_E_WRONG_THREAD;
    if(!init || !out || init->size!=sizeof(*init)||init->version!=2||init->reserved[0]||init->reserved[1])return E_INVALIDARG;
    if(active)return HRESULT_FROM_WIN32(ERROR_BUSY);
    auto binding=pending.lock();if(!binding || binding->thread!=GetCurrentThreadId())return E_UNEXPECTED;
    *out=0;
    auto c=binding->current;
    if(!c || c->dead){
        c=std::make_shared<Context>();
        active=c; // Startup pumps messages; reject nested Initialize.
        struct Reset { bool committed=false; ~Reset(){if(!committed)active.reset();} } reset;
        wire::Message reply;HRESULT hr=Start(binding->executable,c,reply);
        if(FAILED(hr))return hr;
        if(reply.capabilities!=binding->capabilities || reply.features!=binding->features)return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        binding->current=c;reset.committed=true;
    }
    c->initialized=true;active=c;*out=c->token;return S_OK;});}
HRESULT WINAPI Shutdown(PulsePickerUiV2Session token){return Boundary([&]()->HRESULT{std::shared_ptr<Context> c;HRESULT hr=Get(token,c);if(FAILED(hr))return hr;if(c->showing)return HRESULT_FROM_WIN32(ERROR_BUSY);c->dead=true;c->job.Reset();active.reset();return S_OK;});}
HRESULT WINAPI Validate(PulsePickerUiV2Session token,const PulsePickerUiV2Config* config){return Boundary([&]()->HRESULT{std::shared_ptr<Context> c;HRESULT hr=Get(token,c);if(FAILED(hr))return hr;if(!config||c->showing)return E_INVALIDARG;wire::Message m,r;m.kind=wire::Kind::ValidateConfig;if(!wire::FromAbi(*config,m.config))return E_INVALIDARG;return c->Call(m,r);});}
HRESULT Command(PulsePickerUiV2Session token,wire::Message& m,wire::Message& r){std::shared_ptr<Context> c;HRESULT hr=Get(token,c);if(FAILED(hr))return hr;if(!c->showing||!c->callback)return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);return c->Call(m,r);}
HRESULT WINAPI Query(PulsePickerUiV2Session token,uint64_t show,uint64_t generation,PulsePickerUiV2Snapshot* out){return Boundary([&]()->HRESULT{if(!out)return E_POINTER;wire::Message m,r;m.kind=wire::Kind::Query;m.show_id=show;m.expected_generation=generation;HRESULT hr=Command(token,m,r);if(FAILED(hr))return hr;if(r.snapshot.show_id!=show || (generation && r.snapshot.generation!=generation) || !active->Window(r.snapshot.window) || (active->window && active->window!=r.snapshot.window))return E_ACCESSDENIED;return wire::CopyToAbi(r.snapshot,*out);});}
HRESULT WINAPI GetWindow(PulsePickerUiV2Session token,uint64_t show,HWND* out){return Boundary([&]()->HRESULT{if(!out)return E_POINTER;wire::Message m,r;m.kind=wire::Kind::GetWindow;m.show_id=show;HRESULT hr=Command(token,m,r);if(FAILED(hr))return hr;if(show!=active->show_id || !active->Window(r.window) || (active->window && active->window!=r.window))return E_ACCESSDENIED;*out=reinterpret_cast<HWND>(static_cast<uintptr_t>(r.window));return S_OK;});}
HRESULT WINAPI Close(PulsePickerUiV2Session token,uint64_t show,HRESULT reason){return Boundary([&]()->HRESULT{
    // As the in-process API, Close works at any time during Show, not only
    // inside a callback. While Show waits for the host (its read is pending),
    // the frame is written beside that read and the host's reply is taken by
    // Show's own loop; the local checks mirror the in-process ones.
    std::shared_ptr<Context> c=active;
    if(c && c->token==token && c->thread==GetCurrentThreadId() && Sta() && c->showing && !c->callback){
        if(!c->show_id)return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
        if(show!=c->show_id)return HRESULT_FROM_WIN32(ERROR_RETRY);
        if(c->close_requested)return S_FALSE;
        if(c->dead||c->sequence==UINT64_MAX)return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        wire::Message m;m.kind=wire::Kind::Close;m.show_id=show;m.hresult=static_cast<uint32_t>(reason);m.request_id=c->id;m.sequence=++c->sequence;
        std::vector<uint8_t> bytes;if(!wire::Encode(m,bytes))return E_INVALIDARG;
        const auto sent=c->channel.SendFrame(bytes,{nullptr,5000});
        if(!sent){c->dead=true;c->job.Reset();return HRESULT_FROM_WIN32(sent.error);}
        c->pending_close=m.sequence;c->close_requested=true;c->close_result=reason;c->accepted={};
        return S_OK;
    }
    wire::Message m,r;m.kind=wire::Kind::Close;m.show_id=show;m.hresult=static_cast<uint32_t>(reason);HRESULT hr=Command(token,m,r);if(SUCCEEDED(hr)&&!active->close_requested){active->close_requested=true;active->close_result=reason;active->accepted={};}return hr;});}
HRESULT WINAPI Update(PulsePickerUiV2Session token,uint64_t show,const PulsePickerUiV2Control* control){return Boundary([&]()->HRESULT{wire::Message m,r;m.kind=wire::Kind::UpdateControl;m.show_id=show;if(!control||!wire::FromAbi(*control,m.control))return E_INVALIDARG;return Command(token,m,r);});}
HRESULT WINAPI Navigate(PulsePickerUiV2Session token,uint64_t show,PulsePickerUiV2Text folder){return Boundary([&]()->HRESULT{if(!folder.data&&folder.units)return E_INVALIDARG;wire::Message m,r;m.kind=wire::Kind::Navigate;m.show_id=show;if(folder.units)m.folder.assign(folder.data,folder.units);return Command(token,m,r);});}
HRESULT WINAPI Show(PulsePickerUiV2Session token,const PulsePickerUiV2Config* config,const PulsePickerUiV2Callbacks* callbacks,PulsePickerUiV2Result* output){ bool terminal=false; HRESULT returned=Boundary([&]()->HRESULT{
    std::shared_ptr<Context> c;HRESULT hr=Get(token,c);if(FAILED(hr))return hr;
    if(!config||!callbacks||!output||callbacks->size!=sizeof(*callbacks)||callbacks->reserved||output->size!=sizeof(*output))return E_INVALIDARG;
    if(c->showing)return HRESULT_FROM_WIN32(ERROR_BUSY);
    output->status=PULSE_PICKER_UI_V2_FAILED;output->flags=0;output->win32_error=0;output->terminal_hr=E_FAIL;
    output->filter_index=0;output->show_id=0;output->generation=0;output->path.units=0;output->paths.units=0;output->path_count=0;
    if(output->path.data&&output->path.capacity)output->path.data[0]=0;
    if(output->paths.data&&output->paths.capacity)output->paths.data[0]=0;
    c->show_id=0;c->window=0;c->close_requested=false;c->close_result=S_OK;c->accepted={};c->pending_close=0;
    wire::Message request;request.kind=wire::Kind::Show;if(!wire::FromAbi(*config,request.config))return E_INVALIDARG;
    c->showing=true;
    struct Reset{Context& c;~Reset(){c.owner=nullptr;c.showing=false;c.callback=false;if(c.quit){PostQuitMessage(c.quit_code);c.quit=false;}}}reset{*c};
    PickerOwnerGuard owner(reinterpret_cast<HWND>(static_cast<uintptr_t>(request.config.owner)));
    if(owner.Error())return HRESULT_FROM_WIN32(owner.Error());
    c->owner=&owner;
    bool completed=false;struct Abort {Context& c;bool& completed;~Abort(){if(!completed){c.dead=true;c.job.Reset();}}}abort{*c,completed};
    if(c->sequence==UINT64_MAX)return E_UNEXPECTED;
    request.request_id=c->id;request.sequence=++c->sequence;hr=c->Send(request);if(FAILED(hr))return hr;
    for(;;){
        wire::Message incoming;hr=c->Receive(incoming);if(FAILED(hr))return hr;
        if(incoming.kind==wire::Kind::Reply){
            // Only the reply to a Close sent while waiting may arrive here.
            if(!c->pending_close || incoming.sequence!=c->pending_close)return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            c->pending_close=0;
            const HRESULT closed=static_cast<HRESULT>(incoming.hresult);
            if(FAILED(closed)){c->close_requested=false;c->close_result=S_OK;}
            continue;
        }
        if(incoming.kind==wire::Kind::Terminal){
            if(incoming.sequence!=request.sequence || incoming.hresult!=incoming.result.terminal_hr ||
                (c->show_id && incoming.result.show_id!=c->show_id))return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            if(c->close_requested && (incoming.result.status!=PULSE_PICKER_UI_V2_CLOSED ||
                incoming.hresult!=static_cast<uint32_t>(c->close_result)))return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            if(incoming.result.status==PULSE_PICKER_UI_V2_ACCEPTED &&
                (c->accepted.candidate_paths.empty() || incoming.result.generation!=c->accepted.generation ||
                 incoming.result.paths!=c->accepted.candidate_paths || incoming.result.filter_index!=c->accepted.filter_index))
                return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            hr=wire::CopyToAbi(incoming.result,*output);
            completed=SUCCEEDED(hr);terminal=completed;
            return FAILED(hr)?hr:static_cast<HRESULT>(incoming.hresult);
        }
        if((incoming.kind!=wire::Kind::Event && incoming.kind!=wire::Kind::Overwrite && incoming.kind!=wire::Kind::ShareViolation)||
            incoming.sequence<=c->event_sequence||!c->Window(incoming.event.window))return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        if(!incoming.event.show_id || (c->show_id && incoming.event.show_id!=c->show_id) ||
            (c->window && incoming.event.window!=c->window))return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        c->show_id=incoming.event.show_id;c->window=incoming.event.window;
        output->flags|=PULSE_PICKER_UI_V2_UI_SHOWN;
        output->show_id=c->show_id;output->generation=incoming.event.generation;
        c->event_sequence=incoming.sequence;c->callback=true;HRESULT answer=S_OK;uint32_t response=0;
        try{
            if(incoming.kind==wire::Kind::Event){wire::OwnedEvent event(incoming.event);if(callbacks->callback)answer=callbacks->callback(callbacks->context,&event.Get());}
            else{wire::OwnedOverwriteEvent event(incoming.event);auto function=incoming.kind==wire::Kind::Overwrite?callbacks->overwrite:callbacks->share_violation;answer=function?function(callbacks->context,&event.Get(),&response):E_NOTIMPL;}
        }catch(...){answer=E_UNEXPECTED;}
        c->accepted={};
        if(incoming.kind==wire::Kind::Event && incoming.event.kind==PULSE_PICKER_UI_V2_EVENT_FILE_OK && answer==S_OK && !c->close_requested){
            wire::Message query,queried;query.kind=wire::Kind::Query;query.show_id=c->show_id;query.expected_generation=incoming.event.generation;
            hr=c->Call(query,queried);
            if(FAILED(hr))answer=hr;
            else if(queried.snapshot.show_id!=c->show_id || queried.snapshot.generation!=incoming.event.generation ||
                queried.snapshot.window!=c->window || queried.snapshot.candidate_paths.empty())answer=HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            else c->accepted=queried.snapshot;
        }
        c->callback=false;wire::Message reply;reply.request_id=c->id;reply.kind=wire::Kind::EventReply;reply.sequence=incoming.sequence;reply.hresult=static_cast<uint32_t>(answer);reply.response=response;
        hr=c->Send(reply);if(FAILED(hr))return hr;
    }
});
    if(output && output->size==sizeof(*output) && !terminal){
        output->status=returned==HRESULT_FROM_WIN32(ERROR_CANCELLED)?PULSE_PICKER_UI_V2_CANCELLED:PULSE_PICKER_UI_V2_FAILED;
        output->terminal_hr=returned;output->win32_error=HRESULT_FACILITY(returned)==FACILITY_WIN32?HRESULT_CODE(returned):0;
        output->filter_index=0;output->path.units=0;output->paths.units=0;output->path_count=0;
        if(output->path.data&&output->path.capacity)output->path.data[0]=0;
        if(output->paths.data&&output->paths.capacity)output->paths.data[0]=0;
    }
    return returned;
}
HRESULT Start(const std::wstring& executable,const std::shared_ptr<Context>& c,wire::Message& reply) {
        GUID guid{};HRESULT hr=CoCreateGuid(&guid);if(FAILED(hr))return hr;memcpy(c->id.data(),&guid,sizeof(guid));
        ipc::picker::Server server;auto created=ipc::picker::Server::Create(server);if(!created)return HRESULT_FROM_WIN32(created.error);
        c->job.Reset(CreateJobObjectW(nullptr,nullptr));if(!c->job)return HRESULT_FROM_WIN32(GetLastError());
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if(!SetInformationJobObject(c->job.Get(),JobObjectExtendedLimitInformation,&limits,sizeof(limits)))return HRESULT_FROM_WIN32(GetLastError());
        std::wstring command=L"\""+executable+L"\" --picker-remote-host "+server.Name()+L" "+std::to_wstring(GetCurrentProcessId());
        wchar_t desktop[512]{};DWORD desktop_bytes=0; if(!GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()),UOI_NAME,desktop,sizeof(desktop),&desktop_bytes))return HRESULT_FROM_WIN32(GetLastError()); STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.lpDesktop=desktop;PROCESS_INFORMATION process{};
        if(!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_SUSPENDED|CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process))return HRESULT_FROM_WIN32(GetLastError());
        c->process.Reset(process.hProcess);PipeHandle thread(process.hThread);c->pid=process.dwProcessId;
        if(!AssignProcessToJobObject(c->job.Get(),c->process.Get())){DWORD error=GetLastError();TerminateProcess(c->process.Get(),error);return HRESULT_FROM_WIN32(error);}
        AllowSetForegroundWindow(c->pid);if(ResumeThread(thread.Get())==static_cast<DWORD>(-1))return HRESULT_FROM_WIN32(GetLastError());
        hr=c->Io([&](HANDLE stop){return server.Accept(c->process.Get(),c->channel,{stop,10000});});if(FAILED(hr))return hr;
        wire::Message hello;hello.kind=wire::Kind::Hello;hello.version=2;hr=c->Call(hello,reply);if(FAILED(hr))return hr;
        if(reply.version!=2)return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        return S_OK;
}
}HRESULT LoadFileDialogRemoteProvider(const std::wstring& executable,std::shared_ptr<const FileDialogInprocessProvider>& output,bool allow_unverified) noexcept {
    output.reset();return Boundary([&]()->HRESULT{
        if(!Sta())return RPC_E_WRONG_THREAD;
        if(active || !pending.expired())return HRESULT_FROM_WIN32(ERROR_BUSY);
        if(executable.size()<3||executable[1]!=L':'||executable[2]!=L'\\'||executable.find(L'"')!=std::wstring::npos)return E_INVALIDARG;
        auto c=std::make_shared<Context>();wire::Message reply;HRESULT hr=Start(executable,c,reply);if(FAILED(hr))return hr;
        auto binding=std::make_shared<Binding>();binding->executable=executable;binding->current=c;binding->capabilities=reply.capabilities;binding->features=reply.features;
        auto provider=std::make_shared<FileDialogInprocessProvider>();provider->module_lifetime=binding;provider->allow_unverified_proof=allow_unverified;
        auto& api=provider->api;api.size=sizeof(api);api.version=2;api.capabilities=reply.capabilities;api.implemented_features=reply.features;
        api.Initialize=Initialize;api.ValidateConfig=Validate;api.Show=Show;api.GetWindow=GetWindow;api.Query=Query;api.Close=Close;api.Shutdown=Shutdown;api.UpdateControl=Update;api.Navigate=Navigate;
        provider->validate_remote_window=[weak=std::weak_ptr<Binding>(binding)](PulsePickerUiV2Session session,HWND window){auto bound=weak.lock();auto current=bound?bound->current:nullptr;return current&&current==active&&!current->dead&&current->thread==GetCurrentThreadId()&&current->token==session&&current->Window(reinterpret_cast<uintptr_t>(window));};
        pending=binding;output=std::move(provider);return S_OK;
    });
}
}




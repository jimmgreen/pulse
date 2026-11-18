#include "../picker_inprocess/picker_ui_api_v2.h"
#include <windows.h>
#include <sddl.h>
#include <objbase.h>
#include <filesystem>
#include <vector>
#include <string>
#include <cstdio>
#include <thread>
struct Handle { HANDLE value=nullptr; ~Handle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);} };
DWORD Integrity(HANDLE process){
    Handle token;if(!OpenProcessToken(process,TOKEN_QUERY,&token.value))return 0;
    DWORD size=0;GetTokenInformation(token.value,TokenIntegrityLevel,nullptr,0,&size);std::vector<BYTE> bytes(size);
    if(!size||!GetTokenInformation(token.value,TokenIntegrityLevel,bytes.data(),size,&size))return 0;
    auto sid=reinterpret_cast<TOKEN_MANDATORY_LABEL*>(bytes.data())->Label.Sid;
    return IsValidSid(sid)&&*GetSidSubAuthorityCount(sid)?*GetSidSubAuthority(sid,*GetSidSubAuthorityCount(sid)-1):0;
}
struct Packet { HWND window=nullptr;DWORD pid=0,integrity=0,enable_events=0;BOOL enabled=FALSE;DWORD failure_stage=0,error=0; };
DWORD enable_events=0;
LRESULT CALLBACK OwnerProc(HWND window,UINT message,WPARAM w,LPARAM l){if(message==WM_ENABLE)++enable_events;return DefWindowProcW(window,message,w,l);}
int Child(HANDLE pipe,HANDLE stop,DWORD parent_pid){
    auto fail=[&](DWORD stage,DWORD error){Packet packet{};packet.pid=GetCurrentProcessId();packet.failure_stage=stage;packet.error=error;DWORD written=0;WriteFile(pipe,&packet,sizeof(packet),&written,nullptr);return static_cast<int>(stage);};
    Handle parent{OpenProcess(SYNCHRONIZE,FALSE,parent_pid)}, token;
    if(!parent.value||!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY|TOKEN_ADJUST_DEFAULT,&token.value))return fail(10,GetLastError());
    PSID sid=nullptr;if(!ConvertStringSidToSidW(L"S-1-16-4096",&sid))return fail(11,GetLastError());
    TOKEN_MANDATORY_LABEL label{};label.Label.Sid=sid;label.Label.Attributes=SE_GROUP_INTEGRITY;
    const BOOL lowered=SetTokenInformation(token.value,TokenIntegrityLevel,&label,sizeof(label)+GetLengthSid(sid));
    const DWORD error=GetLastError();LocalFree(sid);
    if(!lowered){std::printf("CHILD lower_integrity_error=%lu\n",error);return fail(12,error);}
    WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=OwnerProc;wc.lpszClassName=L"PulseOwnedLowIlOwner";
    if(!RegisterClassW(&wc))return fail(13,GetLastError());
    HWND window=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,wc.lpszClassName,L"Owned low integrity disabled owner",WS_OVERLAPPEDWINDOW|WS_DISABLED,20,20,360,100,nullptr,nullptr,wc.hInstance,nullptr);
    if(!window)return fail(14,GetLastError());ShowWindow(window,SW_SHOWNOACTIVATE);
    Packet packet{window,GetCurrentProcessId(),Integrity(GetCurrentProcess()),enable_events,IsWindowEnabled(window)};DWORD written=0;
    if(!WriteFile(pipe,&packet,sizeof(packet),&written,nullptr)||written!=sizeof(packet)){DestroyWindow(window);return 15;}
    HANDLE waits[]{stop,parent.value};
    for(;;){DWORD waited=MsgWaitForMultipleObjects(2,waits,FALSE,100,QS_ALLINPUT);if(waited==WAIT_OBJECT_0||waited==WAIT_OBJECT_0+1)break;MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}}
    packet.enable_events=enable_events;packet.enabled=IsWindowEnabled(window);WriteFile(pipe,&packet,sizeof(packet),&written,nullptr);
    DestroyWindow(window);UnregisterClassW(wc.lpszClassName,wc.hInstance);return 0;
}
bool ReadPacket(HANDLE pipe,HANDLE process,Packet& packet){
    const ULONGLONG until=GetTickCount64()+5000;
    do{DWORD available=0;if(!PeekNamedPipe(pipe,nullptr,0,nullptr,&available,nullptr))return false;
        if(available>=sizeof(packet)){DWORD read=0;return ReadFile(pipe,&packet,sizeof(packet),&read,nullptr)&&read==sizeof(packet);}
        if(WaitForSingleObject(process,0)==WAIT_OBJECT_0)return false;Sleep(10);
    }while(GetTickCount64()<until);return false;
}
struct State {HWND owner=nullptr;bool accept=false,visible=false,owner_unchanged=true;unsigned ready=0,file_ok=0;};
HRESULT WINAPI Event(void* context,const PulsePickerUiV2Event* event){
    auto& state=*static_cast<State*>(context);state.owner_unchanged=state.owner_unchanged&&!IsWindowEnabled(state.owner);
    if(event->kind==PULSE_PICKER_UI_V2_EVENT_READY){++state.ready;state.visible=IsWindowVisible(event->window)!=FALSE;
        if(state.accept){HWND edit=GetDlgItem(event->window,102);if(!edit)return E_FAIL;PostMessageW(edit,WM_KEYDOWN,VK_RETURN,0);}else PostMessageW(event->window,WM_CLOSE,0,0);
    }
    if(event->kind==PULSE_PICKER_UI_V2_EVENT_FILE_OK)++state.file_ok;
    return S_OK;
}
int wmain(int argc,wchar_t** argv){
    std::setvbuf(stdout,nullptr,_IONBF,0);
    if(argc==5&&std::wstring(argv[1])==L"--owner-child")return Child(reinterpret_cast<HANDLE>(_wcstoui64(argv[2],nullptr,10)),reinterpret_cast<HANDLE>(_wcstoui64(argv[3],nullptr,10)),wcstoul(argv[4],nullptr,10));
    if(argc!=2||!std::filesystem::path(argv[1]).is_absolute())return 2;
    unsigned failures=0;auto check=[&](bool ok,const char* label){std::printf("[%s] %s\n",ok?"PASS":"FAIL",label);if(!ok)++failures;};
    const DWORD caller_il=Integrity(GetCurrentProcess());std::printf("CALLER pid=%lu integrity=%lu\n",GetCurrentProcessId(),caller_il);
    if(caller_il!=SECURITY_MANDATORY_MEDIUM_RID){std::printf("REJECT expected actual medium caller\n");return 2;}
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};Handle read,write,stop{CreateEventW(&sa,TRUE,FALSE,nullptr)};
    if(!stop.value||!CreatePipe(&read.value,&write.value,&sa,0)||!SetHandleInformation(read.value,HANDLE_FLAG_INHERIT,0))return 2;
    SIZE_T bytes=0;InitializeProcThreadAttributeList(nullptr,1,0,&bytes);std::vector<BYTE> storage(bytes);
    auto attributes=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if(!InitializeProcThreadAttributeList(attributes,1,0,&bytes))return 2;
    HANDLE inherited[]{write.value,stop.value};if(!UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),nullptr,nullptr)){DeleteProcThreadAttributeList(attributes);return 2;}
    wchar_t self[32768]{};GetModuleFileNameW(nullptr,self,32768);
    std::wstring command=L"\""+std::wstring(self)+L"\" --owner-child "+std::to_wstring(reinterpret_cast<UINT_PTR>(write.value))+L" "+std::to_wstring(reinterpret_cast<UINT_PTR>(stop.value))+L" "+std::to_wstring(GetCurrentProcessId());
    STARTUPINFOEXW start{};start.StartupInfo.cb=sizeof(start);start.lpAttributeList=attributes;PROCESS_INFORMATION created{};
    const BOOL launched=CreateProcessW(self,command.data(),nullptr,nullptr,TRUE,EXTENDED_STARTUPINFO_PRESENT,nullptr,nullptr,&start.StartupInfo,&created);
    const DWORD launch_error=GetLastError();DeleteProcThreadAttributeList(attributes);std::printf("CHILD launch=%u error=%lu pid=%lu\n",launched,launched?0:launch_error,created.dwProcessId);
    if(!launched)return 2;Handle process{created.hProcess},thread{created.hThread};CloseHandle(write.value);write.value=nullptr;
    Packet before{},after{};const bool ready=ReadPacket(read.value,process.value,before);
    std::printf("CHILD status_stage=%lu error=%lu\n",before.failure_stage,before.error);
    const DWORD owner_il=Integrity(process.value);DWORD owner_pid=0;GetWindowThreadProcessId(before.window,&owner_pid);
    std::printf("OWNER pid=%lu actual_integrity=%lu reported_integrity=%lu hwnd=%p enabled=%u enable_events=%lu\n",created.dwProcessId,owner_il,before.integrity,before.window,before.enabled,before.enable_events);
    check(ready&&before.pid==created.dwProcessId&&owner_pid==created.dwProcessId&&owner_il==SECURITY_MANDATORY_LOW_RID&&before.integrity==owner_il&&IsWindow(before.window)&&!before.enabled,"actual lower-integrity child owns disabled live window");
    RECT original_rect{};GetWindowRect(before.window,&original_rect);const LONG_PTR style=GetWindowLongPtrW(before.window,GWL_STYLE);
    const auto root=std::filesystem::absolute(std::filesystem::path(L"bench_data")/(L"low-owner-ui-"+std::to_wstring(GetCurrentProcessId())));bool fixture=false;
    if(!failures&&std::filesystem::create_directory(root)){
        const auto file=root/L"owned.txt";Handle data{CreateFileW(file.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr)};fixture=data.value!=INVALID_HANDLE_VALUE;
        if(fixture){CloseHandle(data.value);data.value=nullptr;}
        const HRESULT initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        if(fixture&&SUCCEEDED(initialized)){
            HMODULE module=LoadLibraryExW(argv[1],nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
            using GetApi=HRESULT(WINAPI*)(uint32_t,uint32_t,PulsePickerUiV2Api*);auto get=module?reinterpret_cast<GetApi>(GetProcAddress(module,"PulsePickerUiGetApiV2")):nullptr;
            PulsePickerUiV2Api api{};PulsePickerUiV2Session session=0;PulsePickerUiV2Initialize init{};init.size=sizeof(init);init.version=2;
            if(get&&get(2,sizeof(api),&api)==S_OK&&api.Initialize(&init,&session)==S_OK){
                for(bool accept:{false,true}){
                    std::wstring folder=root.wstring(),name=L"owned.txt",title=accept?L"Owned low IL owner: accept":L"Owned low IL owner: cancel";
                    PulsePickerUiV2Config config{};config.size=sizeof(config);config.version=2;config.owner=before.window;config.access_policy=PULSE_PICKER_UI_V2_ACCESS_LOCAL_OPEN_V1;config.initial_folder={folder.c_str(),static_cast<uint32_t>(folder.size())};config.filename={name.c_str(),static_cast<uint32_t>(name.size())};config.title={title.c_str(),static_cast<uint32_t>(title.size())};
                    State state{before.window,accept};PulsePickerUiV2Callbacks callbacks{};callbacks.size=sizeof(callbacks);callbacks.context=&state;callbacks.callback=Event;
                    std::vector<wchar_t> path(PULSE_PICKER_UI_V2_MAX_UNITS+1);PulsePickerUiV2Result result{};result.size=sizeof(result);result.path={path.data(),static_cast<uint32_t>(path.size()),0};
                    const HRESULT shown=api.Show(session,&config,&callbacks,&result);
                    std::printf("SHOW accept=%u hr=%08lx status=%u flags=%u ready=%u FileOk=%u path=%ls\n",accept,shown,result.status,result.flags,state.ready,state.file_ok,path.data());
                    check(state.visible&&state.ready==1&&(result.flags&PULSE_PICKER_UI_V2_UI_SHOWN),"real Pulse window visible and READY reached");
                    check(accept?(shown==S_OK&&result.status==PULSE_PICKER_UI_V2_ACCEPTED&&state.file_ok==1&&path.data()==file.wstring()):(shown==HRESULT_FROM_WIN32(ERROR_CANCELLED)&&result.status==PULSE_PICKER_UI_V2_CANCELLED&&result.path.units==0),"real confirmation or cancellation completed");
                    RECT rectangle{};GetWindowRect(before.window,&rectangle);
                    check(state.owner_unchanged&&IsWindow(before.window)&&!IsWindowEnabled(before.window)&&GetWindowLongPtrW(before.window,GWL_STYLE)==style&&EqualRect(&original_rect,&rectangle),"foreign disabled owner remains live unchanged and disabled");
                }
                check(api.Shutdown(session)==S_OK,"shutdown real UI session");
            }else check(false,"load and initialize real UI API");
            if(module)FreeLibrary(module);CoUninitialize();
        }else check(false,"create owned file and caller STA");
        if(fixture)check(DeleteFileW(file.c_str())!=FALSE,"delete only owned file");check(RemoveDirectoryW(root.c_str())!=FALSE,"delete empty owned directory");
    }
    SetEvent(stop.value);const bool final=ReadPacket(read.value,process.value,after);
    check(final&&after.window==before.window&&!after.enabled&&after.enable_events==before.enable_events,"child reports zero foreign WM_ENABLE changes across both dialogs");
    check(WaitForSingleObject(process.value,5000)==WAIT_OBJECT_0,"owned lower-integrity child exits and destroys owner");
    DWORD exit=0;GetExitCodeProcess(process.value,&exit);check(exit==0,"child cleanup exit successful");
    std::printf("failures=%u\n",failures);return failures?1:0;
}

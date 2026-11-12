#define _CRT_RAND_S
#include <windows.h>
#include <shobjidl.h>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <filesystem>
#include <memory>
#include <string>
#include "../shell_bridge/runtime_auto_entry.h"
#include "../shell_bridge/file_dialog_runtime.h"
#ifndef AUTO_ENTRY_HOOK
#include "../common/dialog_auto_binding.h"
#endif
namespace cfg=pulse::shell_bridge::runtime;
namespace fs=std::filesystem;
constexpr UINT kStart=WM_APP+91,kShow=WM_APP+92,kQuery=WM_APP+93;
struct Shared {
    volatile LONG tid=0,done=0,pulse=0,native=0,worker_ops=0,selected_ok=0;
    HRESULT start=E_PENDING,show=E_PENDING;
    DWORD pid=0;
    PulseFileDialogRuntimeStatus status;
    cfg::PulseRuntimeAutoCoverageV1 coverage;
    cfg::PulseRuntimeAutoStartV1 request;
    wchar_t runtime[cfg::kPathUnits]{};
};
using Query=HRESULT(WINAPI*)(PulseFileDialogRuntimeStatus*,uint32_t) noexcept;
using Start=HRESULT(WINAPI*)(const cfg::PulseRuntimeAutoStartV1*,uint32_t) noexcept;
static std::wstring Env(const wchar_t* name){wchar_t value[512]{};GetEnvironmentVariableW(name,value,512);return value;}
static std::wstring MapName(){return L"Local\\PulseAutoEntryHost-"+Env(L"PULSE_AUTO_TEST_NONCE");}
#ifdef AUTO_ENTRY_HOOK
extern "C" __declspec(dllexport) LRESULT CALLBACK AutoEntryHook(int code,WPARAM wp,LPARAM lp){
    if(code>=0 && wp==PM_REMOVE && reinterpret_cast<MSG*>(lp)->message==kStart){
        HANDLE map=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,MapName().c_str());
        if(map){auto* state=static_cast<Shared*>(MapViewOfFile(map,FILE_MAP_ALL_ACCESS,0,0,sizeof(Shared)));
            if(state && state->pid==GetCurrentProcessId()){
                HMODULE module=LoadLibraryExW(state->runtime,nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
                const auto start=module?reinterpret_cast<Start>(GetProcAddress(module,"PulseFileDialogRuntimeAutoStart")):nullptr;
                state->start=start?start(&state->request,sizeof(state->request)):HRESULT_FROM_WIN32(GetLastError());
            }
            if(state)UnmapViewOfFile(state);CloseHandle(map);
        }
    }
    return CallNextHookEx(nullptr,code,wp,lp);
}
#else
static Shared* child_state=nullptr;
static unsigned accept_action=0;
static HWND observed_dialog=nullptr;
static volatile LONG stop_workers=0;
static DWORD WINAPI HeapWorker(void*) {
    while(!InterlockedCompareExchange(&stop_workers,0,0)) {
        void* allocation=HeapAlloc(GetProcessHeap(),0,8192);
        if(allocation) { memset(allocation,0x5a,8192);HeapFree(GetProcessHeap(),0,allocation); }
        InterlockedIncrement(&child_state->worker_ops);Sleep(1);
    }
    return 0;
}
static HANDLE report=INVALID_HANDLE_VALUE;
static void Log(const char* format,...){char line[2048]{};va_list args;va_start(args,format);vsnprintf_s(line,sizeof(line),_TRUNCATE,format,args);va_end(args);std::fputs(line,stdout);std::fflush(stdout);if(report!=INVALID_HANDLE_VALUE){DWORD written=0;WriteFile(report,line,static_cast<DWORD>(strlen(line)),&written,nullptr);FlushFileBuffers(report);}}
static BOOL CALLBACK CloseDialog(HWND window,LPARAM){
    DWORD pid=0;GetWindowThreadProcessId(window,&pid);if(pid!=GetCurrentProcessId() || !IsWindowVisible(window))return TRUE;
    wchar_t name[128]{};GetClassNameW(window,name,128);
    if(wcscmp(name,L"PulseFolderPickerWindow")==0){
        if(observed_dialog!=window){InterlockedIncrement(&child_state->pulse);observed_dialog=window;}
        if(accept_action==1){const HWND edit=GetDlgItem(window,102);if(edit){accept_action=2;PostMessageW(edit,WM_KEYDOWN,VK_RETURN,0);}}
        else if(!accept_action)PostMessageW(window,WM_CLOSE,0,0);
    }
    else if(wcscmp(name,L"#32770")==0){if(observed_dialog!=window){InterlockedIncrement(&child_state->native);observed_dialog=window;}PostMessageW(window,WM_CLOSE,0,0);}
    return TRUE;
}
static VOID CALLBACK Timer(HWND,UINT,UINT_PTR,DWORD){EnumWindows(CloseDialog,0);}
static void ReadQuery(Shared& state){
    HMODULE module=GetModuleHandleW(L"PulseFileDialogRuntimeUiProof64R8.dll");
    const auto query=module?reinterpret_cast<Query>(GetProcAddress(module,"PulseFileDialogRuntimeQuery")):nullptr;
    if(query)query(&state.status,sizeof(state.status));
    using Coverage=HRESULT(WINAPI*)(cfg::PulseRuntimeAutoCoverageV1*,uint32_t) noexcept;
    const auto coverage=module?reinterpret_cast<Coverage>(GetProcAddress(module,"PulseFileDialogRuntimeAutoQuery")):nullptr;
    if(coverage)coverage(&state.coverage,sizeof(state.coverage));
}
static int Child(){
    HANDLE map=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,MapName().c_str());if(!map)return 10;
    child_state=static_cast<Shared*>(MapViewOfFile(map,FILE_MAP_ALL_ACCESS,0,0,sizeof(Shared)));if(!child_state)return 11;
    if(FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)))return 12;
    HANDLE workers[4]{};for(auto& worker:workers){worker=CreateThread(nullptr,0,HeapWorker,nullptr,0,nullptr);if(!worker)return 13;}
    MSG msg{};PeekMessageW(&msg,nullptr,0,0,PM_NOREMOVE);child_state->pid=GetCurrentProcessId();InterlockedExchange(&child_state->tid,static_cast<LONG>(GetCurrentThreadId()));
    while(GetMessageW(&msg,nullptr,0,0)>0){
        if(msg.message==kShow){
            IFileDialog* dialog=nullptr;HRESULT hr=E_FAIL;
            if(msg.wParam==1){MULTI_QI qi{};qi.pIID=&IID_IFileDialog;hr=CoCreateInstanceEx(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,nullptr,1,&qi);if(SUCCEEDED(hr)){hr=qi.hr;dialog=static_cast<IFileDialog*>(qi.pItf);}}
            else if(msg.wParam==2){IClassFactory* factory=nullptr;hr=CoGetClassObject(CLSID_FileOpenDialog,CLSCTX_INPROC_SERVER,nullptr,IID_PPV_ARGS(&factory));if(SUCCEEDED(hr)){hr=factory->CreateInstance(nullptr,IID_PPV_ARGS(&dialog));factory->Release();}}
            else hr=CoCreateInstance((msg.wParam==3 || msg.wParam==5)?CLSID_FileSaveDialog:CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog));
            if(SUCCEEDED(hr)){
                dialog->SetOptions(msg.wParam==1?0x42u:(FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|FOS_NOCHANGEDIR|FOS_DONTADDTORECENT|((msg.wParam==3 || msg.wParam==5)?(FOS_OVERWRITEPROMPT|FOS_NOREADONLYRETURN):FOS_FILEMUSTEXIST)));
                dialog->SetTitle(L"Pulse owned automatic-entry test - cancels automatically");
                IShellItem* folder=nullptr;const auto own=fs::path(child_state->request.control).parent_path().parent_path()/L"cases";
                if(SUCCEEDED(SHCreateItemFromParsingName(own.c_str(),nullptr,IID_PPV_ARGS(&folder)))){dialog->SetFolder(folder);folder->Release();}
                accept_action=msg.wParam>=4?1u:0u;observed_dialog=nullptr;
                const wchar_t* selected=msg.wParam==5?L"save.txt":L"open.txt";
                if(accept_action)dialog->SetFileName(selected);
                const auto timer=SetTimer(nullptr,0,350,Timer);
                hr=dialog->Show(nullptr);if(timer)KillTimer(nullptr,timer);
                if(SUCCEEDED(hr) && msg.wParam>=4){IShellItem* item=nullptr;PWSTR path=nullptr;if(SUCCEEDED(dialog->GetResult(&item))){if(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&path))){if(CompareStringOrdinal(path,-1,(own/selected).c_str(),-1,TRUE)==CSTR_EQUAL)InterlockedIncrement(&child_state->selected_ok);CoTaskMemFree(path);}item->Release();}}
                dialog->Release();
            }
            child_state->show=hr;ReadQuery(*child_state);InterlockedIncrement(&child_state->done);
        }else if(msg.message==kQuery)ReadQuery(*child_state);
        else{TranslateMessage(&msg);DispatchMessageW(&msg);}
    }
    InterlockedExchange(&stop_workers,1);
    const DWORD workers_done=WaitForMultipleObjects(4,workers,TRUE,5000);
    for(HANDLE worker:workers)CloseHandle(worker);
    CoUninitialize();UnmapViewOfFile(child_state);CloseHandle(map);return workers_done==WAIT_OBJECT_0?0:14;
}
static bool Wait(volatile LONG* value,LONG count){const auto end=GetTickCount64()+30000;while(GetTickCount64()<end){if(InterlockedCompareExchange(value,0,0)>=count)return true;Sleep(20);}return false;}
int wmain(int argc,wchar_t** argv){
    if(argc==2 && std::wstring(argv[1])==L"--lease-helper") {
        HANDLE stop=OpenEventW(SYNCHRONIZE,FALSE,(MapName()+L"-stop").c_str());
        if(!stop)return 30;const DWORD waited=WaitForSingleObject(stop,120000);CloseHandle(stop);
        return waited==WAIT_OBJECT_0?0:31;
    }
    if(argc==2 && std::wstring(argv[1])==L"--child")return Child();
    if((argc!=4 && argc!=5) || std::wstring(argv[1])!=L"--run")return 2;
    const bool negative=argc==5 && std::wstring(argv[4])==L"--bad-identity";
    const bool lease=argc==5 && (std::wstring(argv[4])==L"--lease" || std::wstring(argv[4])==L"--bad-lease");
    const bool bad_lease=lease && std::wstring(argv[4])==L"--bad-lease";
    wchar_t image[32768]{};GetModuleFileNameW(nullptr,image,32768);const auto root=fs::path(image).parent_path();
    if(!root.filename().wstring().starts_with(L"PulseAutoEntryFixture-"))return 3;
    const fs::path bundle=argv[2],control=argv[3];
    fs::create_directories(root/L"cases");
    HANDLE fixture_file=CreateFileW((root/L"cases"/L"open.txt").c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(fixture_file==INVALID_HANDLE_VALUE)return 15;
    CloseHandle(fixture_file);
    SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR",(root/L"data").c_str());
    SetEnvironmentVariableW(L"LOCALAPPDATA",(root/L"data").c_str());
    report=CreateFileW((root/(bad_lease?L"bad-lease-log":lease?L"lease-log":negative?L"negative-log":L"runlog")).c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(report==INVALID_HANDLE_VALUE)return 4;
    HANDLE token=nullptr;TOKEN_ELEVATION elevation{};DWORD token_bytes=0;
    const bool elevated=OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token) && GetTokenInformation(token,TokenElevation,&elevation,sizeof(elevation),&token_bytes) && elevation.TokenIsElevated;
    if(token)CloseHandle(token);Log("TOKEN parent_elevated=%u\n",elevated?1u:0u);if(!elevated)return 16;
    GUID guid{};CoCreateGuid(&guid);wchar_t nonce[40]{};StringFromGUID2(guid,nonce,40);SetEnvironmentVariableW(L"PULSE_AUTO_TEST_NONCE",nonce);
    HANDLE mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(Shared),MapName().c_str());if(!mapping || GetLastError()==ERROR_ALREADY_EXISTS)return 5;
    auto* state=static_cast<Shared*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(Shared)));if(!state)return 6;
    new(state)Shared;wcscpy_s(state->request.nonce,nonce);wcscpy_s(state->request.host,(bundle/L"pulse.exe").c_str());wcscpy_s(state->request.control,control.c_str());wcscpy_s(state->runtime,(bundle/L"PulseFileDialogRuntimeUiProof64R8.dll").c_str());
    STARTUPINFOW si{};si.cb=sizeof(si);PROCESS_INFORMATION child{};std::wstring command=L"\""+std::wstring(image)+L"\" --child";
    if(!CreateProcessW(image,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,root.c_str(),&si,&child))return 7;
    HMODULE module=nullptr;HHOOK hook=nullptr;int result=1;
    HANDLE helper_stop=nullptr;PROCESS_INFORMATION helper{};
    do{
        if(!Wait(&state->tid,1) || state->pid!=child.dwProcessId || static_cast<DWORD>(state->tid)!=child.dwThreadId)break;
        auto session=std::make_unique<cfg::Session>();session->reserved=cfg::kAdminSession;session->target_pid=child.dwProcessId;session->machine=IMAGE_FILE_MACHINE_AMD64;
        FILETIME exit{},kernel{},user{};if(!GetProcessTimes(child.hProcess,&session->target_created,&exit,&kernel,&user))break;
        if(negative)++session->target_created.dwLowDateTime;
        wcscpy_s(session->nonce,nonce);wcscpy_s(session->host_path,state->request.host);wcscpy_s(session->target_path,image);wcscpy_s(session->control_path,control.c_str());wcscpy_s(session->launcher_path,(bundle/L"PulseDialogLauncher.exe").c_str());
        fs::create_directories(control);HANDLE file=CreateFileW((control/cfg::kSessionFile).c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);DWORD written=0;
        const bool saved=file!=INVALID_HANDLE_VALUE && WriteFile(file,session.get(),sizeof(*session),&written,nullptr) && written==sizeof(*session);if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);if(!saved)break;
        if(lease){
            helper_stop=CreateEventW(nullptr,TRUE,FALSE,(MapName()+L"-stop").c_str());
            if(!helper_stop || GetLastError()==ERROR_ALREADY_EXISTS)break;
            std::wstring helper_command=L"\""+std::wstring(image)+L"\" --lease-helper";
            if(!CreateProcessW(image,helper_command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,root.c_str(),&si,&helper))break;
            pulse::dialog_auto_binding::BindingV1 binding{};
            const DWORD error=pulse::dialog_auto_binding::CaptureProcessWithHelper(child.hProcess,helper.hProcess,1,guid,binding);
            Log("LEASE capture=%lu helper=%lu\n",error,helper.dwProcessId);if(error)break;
            if(bad_lease)++binding.helper_created.dwLowDateTime;
            HANDLE binding_file=CreateFileW((control/L"auto-binding.bin").c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
            const bool binding_saved=binding_file!=INVALID_HANDLE_VALUE && WriteFile(binding_file,&binding,sizeof(binding),&written,nullptr) && written==sizeof(binding);
            if(binding_file!=INVALID_HANDLE_VALUE)CloseHandle(binding_file);if(!binding_saved)break;
        }
        PostThreadMessageW(child.dwThreadId,kShow,0,0);if(!Wait(&state->done,1))break;
        Log("BASELINE native=%ld pulse=%ld hr=%08lx\n",state->native,state->pulse,static_cast<unsigned long>(state->show));if(!state->native || state->pulse)break;
        module=LoadLibraryW((root/L"entryhook.dll").c_str());const auto proc=module?reinterpret_cast<HOOKPROC>(GetProcAddress(module,"AutoEntryHook")):nullptr;
        hook=proc?SetWindowsHookExW(WH_GETMESSAGE,proc,module,child.dwThreadId):nullptr;if(!hook){Log("hook_error=%lu\n",GetLastError());break;}
        const LONG worker_before=state->worker_ops;
        PostThreadMessageW(child.dwThreadId,kStart,0,0);
        const auto end=GetTickCount64()+30000;while(GetTickCount64()<end && !state->status.ready){PostThreadMessageW(child.dwThreadId,kQuery,0,0);Sleep(50);if(FAILED(state->start) && state->start!=E_PENDING)break;if(FAILED(state->status.initialization) && state->status.initialization!=E_PENDING)break;}
        Log("START result=%08lx initialization=%08lx ready=%u entries=%u\n",static_cast<unsigned long>(state->start),static_cast<unsigned long>(state->status.initialization),state->status.ready,state->status.installed_entries);
        if(negative || bad_lease){
            if(state->status.ready || state->status.installed_entries || state->status.initialization==E_PENDING)break;
            PostThreadMessageW(child.dwThreadId,kShow,0,0);if(!Wait(&state->done,2))break;
            Log("NEGATIVE %s native=%ld pulse=%ld entries=%u\n",bad_lease?"bad_helper_identity":"bad_identity",state->native,state->pulse,state->status.installed_entries);
            if(state->native>=2 && state->pulse==0)result=0;
            break;
        }
        Log("WORKERS before=%ld after=%ld\n",worker_before,state->worker_ops);
        Log("COVERAGE mask=%u slots=%u skipped=%u foreign=%u protection_restored=%u\n",state->coverage.api_mask,state->coverage.patched_slots,state->coverage.skipped_modules,state->coverage.foreign_slots,state->coverage.protection_restored);
        if(!state->status.ready || state->worker_ops<=worker_before || state->coverage.api_mask!=7 || !state->coverage.patched_slots || !state->coverage.protection_restored)break;
        const LONG old_native=state->native;PostThreadMessageW(child.dwThreadId,kShow,0,0);if(!Wait(&state->done,2))break;
        Log("MODIFIED native=%ld pulse=%ld hr=%08lx admitted=%llu\n",state->native,state->pulse,static_cast<unsigned long>(state->show),static_cast<unsigned long long>(state->status.admitted));if(!state->pulse || state->native!=old_native)break;
        bool variants=true;
        for(WPARAM mode=1;mode<=3;++mode){
            const LONG prior_pulse=state->pulse;PostThreadMessageW(child.dwThreadId,kShow,mode,0);
            if(!Wait(&state->done,static_cast<LONG>(mode)+2)){variants=false;break;}
            Log("VARIANT api=%llu native=%ld pulse=%ld hr=%08lx\n",static_cast<unsigned long long>(mode),state->native,state->pulse,static_cast<unsigned long>(state->show));
            if(state->pulse<=prior_pulse || state->native!=old_native || state->show!=HRESULT_FROM_WIN32(ERROR_CANCELLED)){variants=false;break;}
        }
        if(!variants)break;
        bool accepted=true;
        for(WPARAM mode=4;mode<=5;++mode){
            const LONG prior_pulse=state->pulse;PostThreadMessageW(child.dwThreadId,kShow,mode,0);
            if(!Wait(&state->done,static_cast<LONG>(mode)+2)){accepted=false;break;}
            Log("ACCEPT mode=%llu native=%ld pulse=%ld hr=%08lx results=%ld\n",static_cast<unsigned long long>(mode),state->native,state->pulse,static_cast<unsigned long>(state->show),state->selected_ok);
            if(state->pulse<=prior_pulse || state->native!=old_native || state->show!=S_OK || state->selected_ok!=static_cast<LONG>(mode)-3){accepted=false;break;}
        }
        if(!accepted || fs::exists(root/L"cases"/L"save.txt"))break;
        if(lease){
            if(!SetEvent(helper_stop) || WaitForSingleObject(helper.hProcess,10000)!=WAIT_OBJECT_0)break;
            DWORD helper_exit=STILL_ACTIVE;GetExitCodeProcess(helper.hProcess,&helper_exit);
            Log("LEASE helper_exit=%lu disable_marker=%u\n",helper_exit,fs::exists(control/cfg::kDisableFile)?1u:0u);
            if(helper_exit || fs::exists(control/cfg::kDisableFile))break;
        }else{
            HANDLE disabled=CreateFileW((control/cfg::kDisableFile).c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(disabled==INVALID_HANDLE_VALUE)break;CloseHandle(disabled);
        }
        const LONG old_pulse=state->pulse;PostThreadMessageW(child.dwThreadId,kShow,0,0);if(!Wait(&state->done,8))break;
        Log("ROLLBACK native=%ld pulse=%ld hr=%08lx\n",state->native,state->pulse,static_cast<unsigned long>(state->show));if(state->native<=old_native || state->pulse!=old_pulse)break;
        result=0;
    }while(false);
    if(helper_stop)SetEvent(helper_stop);
    if(helper.hProcess){if(WaitForSingleObject(helper.hProcess,10000)!=WAIT_OBJECT_0){TerminateProcess(helper.hProcess,99);WaitForSingleObject(helper.hProcess,5000);result=1;}CloseHandle(helper.hThread);CloseHandle(helper.hProcess);}
    if(helper_stop)CloseHandle(helper_stop);
    if(hook)UnhookWindowsHookEx(hook);PostThreadMessageW(child.dwThreadId,WM_QUIT,0,0);
    if(WaitForSingleObject(child.hProcess,10000)!=WAIT_OBJECT_0){TerminateProcess(child.hProcess,99);WaitForSingleObject(child.hProcess,5000);result=1;}
    DWORD code=STILL_ACTIVE;GetExitCodeProcess(child.hProcess,&code);if(code)result=1;
    CloseHandle(child.hThread);CloseHandle(child.hProcess);if(module)FreeLibrary(module);UnmapViewOfFile(state);CloseHandle(mapping);
    Log("CLEANUP child_exit=%lu result=%d\n",code,result);CloseHandle(report);return result;
}
#endif

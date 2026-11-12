#include "../common/dialog_auto_registration.h"
#include "../common/runtime_log.h"
#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <cstdio>
#include <string>
#include <cstdint>
namespace pulse::diagnostics::runtime { void Event(const char*, std::initializer_list<Field>, Level) noexcept {} }
namespace {
IFileDialog* active=nullptr;
DWORD sta=0, host_pid=0; unsigned callbacks=0, closes=0;
bool wrong_thread=false, seen=false, expired=false, host64=false, native_seen=false, expect_local=false;
std::wstring title, host, accepted_path;
bool accept_mode=false, submitted=false;
unsigned file_ok_count=0;
HWND observed_window=nullptr;
ULONGLONG deadline=0;
constexpr HRESULT cancelled=HRESULT_FROM_WIN32(ERROR_CANCELLED);
struct Events final : IFileDialogEvents {
    LONG refs=1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override { if (!out) return E_POINTER; *out=nullptr; if (id==IID_IUnknown||id==IID_IFileDialogEvents) { *out=this; AddRef(); return S_OK; } return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override { return InterlockedDecrement(&refs); }
    void Hit() { ++callbacks; wrong_thread |= GetCurrentThreadId()!=sta; }
    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog*) override { Hit(); ++file_ok_count; return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog*,IShellItem*) override { Hit(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog*) override { Hit(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog*) override { Hit(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*,IShellItem*,FDE_SHAREVIOLATION_RESPONSE* out) override { Hit(); *out=FDESVR_DEFAULT; return S_OK; }
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog*) override { Hit(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog*,IShellItem*,FDE_OVERWRITE_RESPONSE* out) override { Hit(); *out=FDEOR_DEFAULT; return S_OK; }
};
BOOL CALLBACK Observe(HWND w, LPARAM) {
    wchar_t name[128]{}, text[256]{}; GetClassNameW(w,name,128); GetWindowTextW(w,text,256);
    if (!IsWindowVisible(w)||title!=text) return TRUE;
    DWORD owner=0; GetWindowThreadProcessId(w,&owner);
    if(owner==GetCurrentProcessId() && !wcscmp(name,L"#32770")) { native_seen=true; PostMessageW(w,WM_COMMAND,IDCANCEL,0); return TRUE; }
    if(wcscmp(name,L"PulseFolderPickerWindow")) return TRUE;
    DWORD pid=0; const DWORD window_thread=GetWindowThreadProcessId(w,&pid);
    if(expect_local) {
        wchar_t ui_path[32768]{};
        auto ui=GetModuleHandleW(L"PulsePickerUiProof.dll");
        const auto expected=host.substr(0,host.find_last_of(L"\\/"))+L"\\PulsePickerUiProof.dll";
        if(pid==GetCurrentProcessId() && window_thread==sta && ui && GetModuleFileNameW(ui,ui_path,32768)) {
            std::wstring actual=ui_path; if(actual.starts_with(L"\\\\?\\")) actual.erase(0,4);
            static bool reported=false; if(!reported) { reported=true; printf("local_ui_pid=%lu image=%ls expected=%ls\n",pid,actual.c_str(),expected.c_str()); }
            if(!_wcsicmp(actual.c_str(),expected.c_str())) { seen=true; host64=sizeof(void*)==8; host_pid=pid; observed_window=w; }
        }
        return TRUE;
    }
    if(pid==GetCurrentProcessId()) return TRUE;
    HANDLE p=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid); wchar_t image[32768]{}; DWORD length=32768; BOOL wow=TRUE;
    if(p && QueryFullProcessImageNameW(p,0,image,&length)) {
        static DWORD reported=0; if(reported!=pid) { reported=pid; printf("candidate_pid=%lu image=%ls expected=%ls\n",pid,image,host.c_str()); }
        std::wstring actual=image; if(actual.starts_with(L"\\\\?\\")) actual.erase(0,4);
        if(!_wcsicmp(actual.c_str(),host.c_str()) && IsWow64Process(p,&wow)) { seen=true; host64=!wow; host_pid=pid; }
    }
    if(p) CloseHandle(p); return TRUE;
}
void CALLBACK Tick(HWND,UINT,UINT_PTR,DWORD) {
    EnumWindows(Observe,0); expired=GetTickCount64()>deadline;
    if(accept_mode && seen && !submitted && callbacks>=3) {
        // Reuse picker_inprocess_v2_test's filename edit/Return acceptance driver.
        const HWND edit=GetDlgItem(observed_window,102);
        if(edit && IsWindowEnabled(edit)) {
            submitted=SendMessageW(edit,WM_SETTEXT,0,reinterpret_cast<LPARAM>(accepted_path.c_str()))!=0 &&
                PostMessageW(edit,WM_KEYDOWN,VK_RETURN,0)!=0;
            printf("accept_submit=%d fixture=%ls\n",submitted,accepted_path.c_str());
        }
    }
    if(active && !closes && ((!accept_mode && seen)||expired)) { ++closes; const auto hr=active->Close(cancelled); printf("close_hr=%08lx\n",hr); }
}
DWORD WINAPI Watchdog(void* done) { if(WaitForSingleObject(done,45000)==WAIT_TIMEOUT) { puts("FAIL watchdog"); fflush(stdout); ExitProcess(124); } return 0; }
bool NoUi() {
    bool clean=true; HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE,GetCurrentProcessId()); MODULEENTRY32W e{}; e.dwSize=sizeof(e);
    if(snap==INVALID_HANDLE_VALUE) return false;
    if(Module32FirstW(snap,&e)) do { if(wcsstr(e.szModule,L"PulsePickerUiProof")||!_wcsicmp(e.szModule,L"lumatext.dll")) clean=false; } while(Module32NextW(snap,&e));
    CloseHandle(snap); return clean;
}
}
int wmain(int argc,wchar_t** argv) {
    // mode (auto|managed|auto-yjk|managed-yjk), Auto32 DLL, x64 pulse.exe, fixture directory, report.
    if(argc!=6) return 2;
    FILE* report=nullptr; if(_wfreopen_s(&report,argv[5],L"w",stdout)) return 3; setvbuf(stdout,nullptr,_IONBF,0);
    accept_mode=!wcscmp(argv[1],L"managed-yjk-full-accept");
    accepted_path=std::wstring(argv[4])+L"\\owned.dwg";
    const bool full=accept_mode || !wcscmp(argv[1],L"auto-yjk-full") || !wcscmp(argv[1],L"managed-yjk-full");
    const bool yjk=full || !wcscmp(argv[1],L"auto-yjk") || !wcscmp(argv[1],L"managed-yjk");
    const bool managed=accept_mode || !wcscmp(argv[1],L"managed") || !wcscmp(argv[1],L"managed-yjk") || !wcscmp(argv[1],L"managed-yjk-full"); host=argv[3]; sta=GetCurrentThreadId();
    printf("caller_pid=%lu pointer_bytes=%zu mode=%s\n",GetCurrentProcessId(),sizeof(void*),managed?"managed":"auto");
    expect_local=sizeof(void*)==8 && managed;
    printf("expected_route=%s full_yjk=%d\n",expect_local?"x64-inprocess":"remote",full);
    if(FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED))) return 4;
    HKEY key=nullptr, machine_classes=nullptr; std::wstring isolated=L"Software\\PulseThinRouteTest-"+std::to_wstring(GetCurrentProcessId()); HMODULE module=nullptr;
    if(!managed) {
        DWORD error=RegCreateKeyExW(HKEY_CURRENT_USER,isolated.c_str(),0,nullptr,0,KEY_ALL_ACCESS,nullptr,&key,nullptr);
        if(!error) error=RegOverridePredefKey(HKEY_CURRENT_USER,key);
        if(!error) error=pulse::dialog_auto::Enable(argv[2]);
        if(!error) error=RegOpenKeyExW(HKEY_LOCAL_MACHINE,L"Software\\Classes",0,KEY_READ|pulse::dialog_auto::RegistryView,&machine_classes);
        if(!error) error=RegOverridePredefKey(HKEY_CLASSES_ROOT,machine_classes);
        printf("isolated_registration=%lu\n",error); if(error) return 5;
        module=LoadLibraryW(argv[2]); if(!module) return 6;
    } else {
        DWORD registry_error=RegCreateKeyExW(HKEY_CURRENT_USER,isolated.c_str(),0,nullptr,0,KEY_ALL_ACCESS,nullptr,&key,nullptr);
        if(!registry_error) registry_error=RegOverridePredefKey(HKEY_CURRENT_USER,key);
        if(!registry_error) registry_error=RegOpenKeyExW(HKEY_LOCAL_MACHINE,L"Software\\Classes",0,KEY_READ|pulse::dialog_auto::RegistryView,&machine_classes);
        if(!registry_error) registry_error=RegOverridePredefKey(HKEY_CLASSES_ROOT,machine_classes);
        printf("managed_machine_classes=%lu\n",registry_error); if(registry_error) return 8;
        struct Status { uint32_t size=sizeof(Status),version=1,pid=0,entries=0; HRESULT initialization=E_PENDING; uint32_t ready=0; uint64_t activations=0,admitted=0,rejected=0; } status;
        using Query=HRESULT(WINAPI*)(Status*,uint32_t);
        #ifdef _WIN64
        auto runtime=GetModuleHandleW(L"PulseFileDialogRuntimeUiProof64R8.dll");
#else
        auto runtime=GetModuleHandleW(L"PulseFileDialogRuntimeUiProof32R8.dll");
#endif
        auto query=runtime?reinterpret_cast<Query>(GetProcAddress(runtime,"PulseFileDialogRuntimeQuery")):nullptr;
        HRESULT hr=query?query(&status,sizeof(status)):E_NOINTERFACE;
        const auto ready_deadline=GetTickCount64()+15000;
        while(query && SUCCEEDED(hr) && !status.ready && status.initialization==E_PENDING && GetTickCount64()<ready_deadline) { Sleep(25); hr=query(&status,sizeof(status)); }
        printf("runtime_query=%08lx ready=%u initialization=%08lx\n",hr,status.ready,status.initialization);
        if(FAILED(hr)||!status.ready) return 7;
    }
    HANDLE done=CreateEventW(nullptr,TRUE,FALSE,nullptr); HANDLE watcher=CreateThread(nullptr,0,Watchdog,done,0,nullptr); bool all=true;
    for(int save=0;save!=(yjk?1:2);++save) {
        seen=expired=host64=wrong_thread=native_seen=false; callbacks=closes=host_pid=0;
        IFileDialog* dialog=nullptr; const CLSID& clsid=save?CLSID_FileSaveDialog:CLSID_FileOpenDialog;
        HRESULT hr=E_FAIL;
        if(managed) hr=CoCreateInstance(clsid,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog));
        else { using Get=HRESULT(WINAPI*)(REFCLSID,REFIID,void**); auto get=reinterpret_cast<Get>(GetProcAddress(module,"DllGetClassObject")); IClassFactory* factory=nullptr; hr=get?get(clsid,IID_PPV_ARGS(&factory)):E_NOINTERFACE; if(SUCCEEDED(hr)) { hr=factory->CreateInstance(nullptr,IID_PPV_ARGS(&dialog)); factory->Release(); } }
        if(FAILED(hr)) { printf("FAIL activation=%08lx\n",hr); all=false; break; }
        title=L"Pulse thin route "+std::to_wstring(GetCurrentProcessId())+(save?L" save":L" open");
        IShellItem* folder=nullptr; hr=SHCreateItemFromParsingName(argv[4],nullptr,IID_PPV_ARGS(&folder));
        bool configured=SUCCEEDED(hr);
        if(!full) configured &= SUCCEEDED(dialog->SetTitle(title.c_str())) && SUCCEEDED(dialog->SetFolder(folder));
        if(yjk) {
            const COMDLG_FILTERSPEC filters[]{
                {L"YJK drawing", L"*.dwy"},
                {L"DWG drawing", L"*.dwg"},
                {L"DXF drawing", L"*.dxf"},
                {L"CAD drawings", L"*.dwg; *.dxf"},
                {L"All drawings", L"*.dwy;*.dwg;*.dxf"}
            };
            const HRESULT types=dialog->SetFileTypes(ARRAYSIZE(filters),filters);
            const UINT selected=full?1:4;
            const HRESULT index=dialog->SetFileTypeIndex(selected);
            configured &= SUCCEEDED(types) && SUCCEEDED(index);
            printf("yjk_filter_count=5 selected_index=%u spaced_mask=*.dwg; *.dxf setter_hr=%08lx index_hr=%08lx\n",selected,types,index);
            if(full) {
                const HRESULT filename=dialog->SetFileName(L"");
                const HRESULT options=dialog->SetOptions(0x42u);
                FILEOPENDIALOGOPTIONS actual{}; const HRESULT queried=dialog->GetOptions(&actual);
                configured &= SUCCEEDED(filename) && SUCCEEDED(options) && SUCCEEDED(queried) && actual==0x42u;
                printf("full_order=SetFileTypes,SetFileTypeIndex,SetFileName,SetOptions empty_filename=1 filename_hr=%08lx options_hr=%08lx actual_options=%08lx\n",filename,options,actual);
            }
        }
        if(full) configured &= SUCCEEDED(dialog->SetTitle(title.c_str())) && SUCCEEDED(dialog->SetFolder(folder));
        if(folder) folder->Release();
        if(save) configured &= SUCCEEDED(dialog->SetFileName(L"thin-route-never-saved.txt"));
        Events events; DWORD cookie=0; configured &= SUCCEEDED(dialog->Advise(&events,&cookie));
        active=dialog; deadline=GetTickCount64()+15000; UINT_PTR timer=SetTimer(nullptr,0,100,Tick);
        hr=configured?dialog->Show(nullptr):E_FAIL; KillTimer(nullptr,timer); active=nullptr;
        IShellItem* result=nullptr; HRESULT result_hr=dialog->GetResult(&result); bool no_result=FAILED(result_hr)&&!result; bool path_matches=false;
        if(result && SUCCEEDED(result_hr)) { PWSTR path=nullptr; if(SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH,&path))) { path_matches=!_wcsicmp(path,accepted_path.c_str()); CoTaskMemFree(path); } }
        if(result) result->Release();
        HRESULT unadvise=dialog->Unadvise(cookie); ULONG remaining=dialog->Release(); bool clean=NoUi();
        const bool terminal=accept_mode ? hr==S_OK && path_matches && file_ok_count==1 && submitted && closes==0 : hr==cancelled && closes==1 && no_result;
        bool pass=configured&&terminal&&seen&&!native_seen&&host64&&!expired&&callbacks>0&&!wrong_thread&&SUCCEEDED(unadvise)&&events.refs==1&&remaining==0&&(expect_local?!clean:clean);
        if(accept_mode) printf("accept_result_hr=%08lx exact_fixture_path=%d file_ok_count=%u submitted=%d\n",result_hr,path_matches,file_ok_count,submitted);
        printf("%s %s show=%08lx host_pid=%lu host_x64=%d callbacks=%u sta=%d close_count=%u no_result=%d sink_refs=%ld dialog_refs=%lu no_local_ui=%d timeout=%d native_seen=%d\n",pass?"PASS":"FAIL",save?"save":"open",hr,host_pid,host64,callbacks,!wrong_thread,closes,no_result,events.refs,remaining,clean,expired,native_seen); all &= pass;
    }
    SetEvent(done); WaitForSingleObject(watcher,1000); CloseHandle(watcher); CloseHandle(done);
    if(machine_classes) { RegOverridePredefKey(HKEY_CLASSES_ROOT,nullptr); RegCloseKey(machine_classes); }
    if(key) { RegOverridePredefKey(HKEY_CURRENT_USER,nullptr); RegCloseKey(key); printf("registry_cleanup=%ld\n",RegDeleteTreeW(HKEY_CURRENT_USER,isolated.c_str())); }
    CoUninitialize(); printf("RESULT=%s\n",all?"PASS":"FAIL"); if(managed) Sleep(1500); return all?0:1;
}









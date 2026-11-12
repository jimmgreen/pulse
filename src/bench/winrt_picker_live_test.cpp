#include "../shell_bridge/winrt_picker_activation.h"
#include "../ipc/picker_client.h"
#include "../shell_bridge/file_dialog_runtime.h"
#include "../shell_bridge/runtime_auto_entry.h"
#include <winrt/Windows.Storage.Pickers.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <io.h>
#include <fcntl.h>
namespace api=pulse::shell_bridge::winrt_picker;
namespace wire=pulse::ipc::picker;
namespace pick=winrt::Windows::Storage::Pickers;
using winrt::Windows::Foundation::AsyncStatus;
struct Fixture {
    HWND owner=nullptr, other=nullptr, dialog=nullptr;
    std::wstring executable,path;
    FILETIME started{};
    bool driven=false,cancel=false,switched=false,switch_mode=false,click_switch=false,identity_failed=false;
    unsigned shown=0;ULONGLONG switch_started=0;
    bool Owned(HWND window) {
        if(!IsWindowVisible(window)||GetWindow(window,GW_OWNER)!=owner)return false;
        DWORD pid=0;GetWindowThreadProcessId(window,&pid);
        HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
        if(!process)return false;
        wchar_t image[32768]{};DWORD units=32768;FILETIME created{},exit{},kernel{},user{};
        const bool identity=QueryFullProcessImageNameW(process,0,image,&units)&&_wcsicmp(image,executable.c_str())==0&&
            GetProcessTimes(process,&created,&exit,&kernel,&user)&&CompareFileTime(&created,&started)>=0;
        CloseHandle(process);if(!identity)return false;
        HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);if(snapshot==INVALID_HANDLE_VALUE)return false;
        PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);bool child=false;
        if(Process32FirstW(snapshot,&entry))do{if(entry.th32ProcessID==pid){child=entry.th32ParentProcessID==GetCurrentProcessId();break;}}while(Process32NextW(snapshot,&entry));
        CloseHandle(snapshot);return child;
    }
    void Drive() {
        if(driven)return;
        EnumWindows([](HWND window,LPARAM context)->BOOL{auto& self=*reinterpret_cast<Fixture*>(context);if(self.Owned(window)){self.dialog=window;return FALSE;}return TRUE;},reinterpret_cast<LPARAM>(this));
        if(!dialog)return;
        ++shown;driven=true;
        printf("UI owned_child=1 visible=1 owner_enabled=%d thread=%lu\n",IsWindowEnabled(owner),GetCurrentThreadId());
        if(switch_mode){ShowWindow(other,SW_SHOWNORMAL);SetForegroundWindow(other);switch_started=GetTickCount64();printf("SWITCH requested=1 immediate=%d\n",GetForegroundWindow()==other);
            if(click_switch){
                SetWindowPos(other,HWND_TOPMOST,10,10,320,160,SWP_NOACTIVATE);POINT point{40,40};ClientToScreen(other,&point);
                DWORD pid=0;GetWindowThreadProcessId(other,&pid);const HWND hit=WindowFromPoint(point);
                printf("SWITCH hit=%p other=%p pid=%lu visible=%d\n",static_cast<void*>(hit),static_cast<void*>(other),pid,IsWindowVisible(other));if(pid!=GetCurrentProcessId()||!IsWindowVisible(other)||(hit!=other&&!IsChild(other,hit))){identity_failed=true;return;}
                if(GetAsyncKeyState(VK_LBUTTON)&0x8000){identity_failed=true;return;}SetCursorPos(point.x,point.y);
                INPUT input[2]{};input[0].type=INPUT_MOUSE;input[0].mi.dwFlags=MOUSEEVENTF_LEFTDOWN;input[1].type=INPUT_MOUSE;input[1].mi.dwFlags=MOUSEEVENTF_LEFTUP;
                printf("SWITCH owned_click_count=%u before_fg=%p\n",SendInput(2,input,sizeof(INPUT)),static_cast<void*>(GetForegroundWindow()));

            }}
        if(cancel)return;
        HWND filename=GetDlgItem(dialog,102);DWORD_PTR answer=0;
        if(!filename||GetParent(filename)!=dialog||!SendMessageTimeoutW(filename,WM_SETTEXT,0,reinterpret_cast<LPARAM>(path.c_str()),SMTO_ABORTIFHUNG|SMTO_BLOCK,2000,&answer)||!answer){identity_failed=true;return;}
        PostMessageW(dialog,WM_COMMAND,IDOK,0);
    }
};
int wmain(int argc,wchar_t** argv) {
    setvbuf(stdout,nullptr,_IONBF,0);
    if(argc<2)return 2;
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        bool runtime=false,iat=false;std::wstring auto_dll,auto_nonce,auto_control,report_path;
        Fixture f;f.executable=std::filesystem::absolute(argv[1]).make_preferred().wstring();
        for(int i=2;i<argc;++i){f.cancel|=std::wstring_view(argv[i])==L"--cancel";f.switch_mode|=std::wstring_view(argv[i])==L"--switch";f.click_switch|=std::wstring_view(argv[i])==L"--click-switch";runtime|=std::wstring_view(argv[i])==L"--runtime";iat|=std::wstring_view(argv[i])==L"--iat";if(std::wstring_view(argv[i])==L"--report"&&i+1<argc)report_path=argv[++i];if(std::wstring_view(argv[i])==L"--auto"&&i+3<argc){auto_dll=argv[++i];auto_nonce=argv[++i];auto_control=argv[++i];runtime=iat=true;}}f.switch_mode|=f.click_switch;f.cancel|=f.switch_mode;
        if(!report_path.empty()){if(!std::filesystem::path(report_path).is_absolute())return 2;HANDLE output=CreateFileW(report_path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(output==INVALID_HANDLE_VALUE)return 2;int fd=_open_osfhandle(reinterpret_cast<intptr_t>(output),_O_WRONLY|_O_TEXT);if(fd<0){CloseHandle(output);return 2;}if(_dup2(fd,_fileno(stdout))){_close(fd);return 2;}_close(fd);setvbuf(stdout,nullptr,_IONBF,0);}
        if(!auto_dll.empty()){
            HMODULE loaded=LoadLibraryExW(auto_dll.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);if(!loaded)winrt::throw_last_error();
            using Start=HRESULT(WINAPI*)(const pulse::shell_bridge::runtime::PulseRuntimeAutoStartV1*,uint32_t);
            auto start=reinterpret_cast<Start>(GetProcAddress(loaded,"PulseFileDialogRuntimeAutoStart"));if(!start)winrt::throw_hresult(E_NOINTERFACE);
            pulse::shell_bridge::runtime::PulseRuntimeAutoStartV1 request;wcscpy_s(request.nonce,auto_nonce.c_str());wcscpy_s(request.host,f.executable.c_str());wcscpy_s(request.control,auto_control.c_str());winrt::check_hresult(start(&request,sizeof(request)));
        }
        if(runtime){
            HMODULE module=GetModuleHandleW(L"PulseFileDialogRuntimeUiProof64R8.dll");if(!module)winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND));
            using Query=HRESULT(WINAPI*)(PulseFileDialogRuntimeStatus*,uint32_t);auto query=reinterpret_cast<Query>(GetProcAddress(module,"PulseFileDialogRuntimeQuery"));if(!query)winrt::throw_hresult(E_NOINTERFACE);
            PulseFileDialogRuntimeStatus status;const auto until=GetTickCount64()+10000;
            do{winrt::check_hresult(query(&status,sizeof(status)));if(status.ready||status.initialization!=E_PENDING)break;MsgWaitForMultipleObjectsEx(0,nullptr,10,QS_ALLINPUT,MWMO_INPUTAVAILABLE);MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}}while(GetTickCount64()<until);
            wchar_t path[32768]{};GetModuleFileNameW(module,path,32768);char utf8[65536]{};WideCharToMultiByte(CP_UTF8,0,path,-1,utf8,sizeof(utf8),nullptr,nullptr);printf("RUNTIME module=%s ready=%u entries=%08X initialization=%08lX\n",utf8,status.ready,status.installed_entries,static_cast<unsigned long>(status.initialization));
            if(!status.ready||!(status.installed_entries&0x1000))winrt::throw_hresult(E_FAIL);
            using AutoQuery=HRESULT(WINAPI*)(pulse::shell_bridge::runtime::PulseRuntimeAutoCoverageV1*,uint32_t);auto auto_query=reinterpret_cast<AutoQuery>(GetProcAddress(module,"PulseFileDialogRuntimeAutoQuery"));
            if(iat){pulse::shell_bridge::runtime::PulseRuntimeAutoCoverageV1 coverage;if(!auto_query)winrt::throw_hresult(E_NOINTERFACE);winrt::check_hresult(auto_query(&coverage,sizeof(coverage)));printf("IAT mask=%08X slots=%u protection=%u\n",coverage.api_mask,coverage.patched_slots,coverage.protection_restored);if(!(coverage.api_mask&0x1000)||!coverage.patched_slots||!coverage.protection_restored)winrt::throw_hresult(E_FAIL);}else if(auto_query)winrt::throw_hresult(E_INVALIDARG);
        }
        GetSystemTimeAsFileTime(&f.started);
        wchar_t temp[32768]{};GetTempPathW(32768,temp);const auto directory=std::filesystem::path(temp)/(L"PulseWinrtLive-"+std::to_wstring(GetCurrentProcessId()));
        if(!std::filesystem::create_directory(directory))return 2;f.path=(directory/L"owned.txt").wstring();std::ofstream(std::filesystem::path(f.path))<<"winrt-live-consumed";
        struct Cleanup{Fixture& f;std::filesystem::path directory;~Cleanup(){if(f.other)DestroyWindow(f.other);if(f.owner)DestroyWindow(f.owner);std::filesystem::remove(f.path);std::filesystem::remove(directory);}} cleanup{f,directory};
        WNDCLASSW window_class{};window_class.lpfnWndProc=DefWindowProcW;window_class.hInstance=GetModuleHandleW(nullptr);window_class.lpszClassName=L"PulseWinrtOwnedFixture";window_class.hCursor=LoadCursorW(nullptr,IDC_ARROW);window_class.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);if(!RegisterClassW(&window_class))winrt::throw_last_error();
        f.owner=CreateWindowExW(0,L"PulseWinrtOwnedFixture",L"Owned WinRT A",WS_OVERLAPPEDWINDOW,40,40,320,160,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        f.other=CreateWindowExW(0,L"PulseWinrtOwnedFixture",L"Owned WinRT B",WS_OVERLAPPEDWINDOW,80,80,320,160,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        if(!f.owner||!f.other)winrt::throw_last_error();ShowWindow(f.owner,SW_SHOWNORMAL);SetForegroundWindow(f.owner);
        const DWORD sta=GetCurrentThreadId();std::atomic<unsigned> calls{0};std::atomic<bool> transport_done{runtime};
        api::Services services{[&](const wire::Message& request,HWND owner,HANDLE cancel){++calls;printf("TRANSPORT caller_tid=%lu owner_tid=%lu enabled=%d\n",GetCurrentThreadId(),sta,IsWindowEnabled(owner));auto reply=wire::RunPickerClient(f.executable,request,owner,cancel);const auto& response=std::get<wire::Response>(reply.body);printf("TRANSPORT status=%u error=%u source=%u\n",static_cast<unsigned>(response.status),response.error_code,static_cast<unsigned>(response.error_source));transport_done=true;return reply;}};
        const winrt::hstring name(L"Windows.Storage.Pickers.FileOpenPicker");winrt::Windows::Foundation::IActivationFactory factory{nullptr};
        if(runtime)winrt::check_hresult(::RoGetActivationFactory(static_cast<HSTRING>(winrt::get_abi(name)),winrt::guid_of<winrt::Windows::Foundation::IActivationFactory>(),winrt::put_abi(factory)));
        else winrt::check_hresult(api::GetActivationFactory(static_cast<HSTRING>(winrt::get_abi(name)),winrt::guid_of<winrt::Windows::Foundation::IActivationFactory>(),winrt::put_abi(factory),services));
        auto picker=factory.ActivateInstance<pick::FileOpenPicker>();picker.FileTypeFilter().Append(L".txt");picker.SuggestedStartLocation(pick::PickerLocationId::Desktop);winrt::check_hresult(picker.as<IInitializeWithWindow>()->Initialize(f.owner));
        printf("START owner_enabled=%d owner_sta=%lu\n",IsWindowEnabled(f.owner),sta);
        auto operation=picker.PickSingleFileAsync();const auto deadline=GetTickCount64()+20000;bool cancellation_sent=false;
        while(operation.Status()==AsyncStatus::Started){if(GetTickCount64()>deadline){operation.Cancel();break;}MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}f.Drive();if(f.switch_mode&&f.driven&&!f.switched){f.switched=GetForegroundWindow()==f.other;}if(f.cancel&&f.driven&&!cancellation_sent&&(!f.switch_mode||f.switched||GetTickCount64()-f.switch_started>2000)){printf("SWITCH settled=%d setup_failed=%d\n",f.switched,f.switch_mode&&!f.switched);operation.Cancel();cancellation_sent=true;}MsgWaitForMultipleObjectsEx(0,nullptr,10,QS_ALLINPUT,MWMO_INPUTAVAILABLE);}
        bool consumed=false,canceled=false;HRESULT result=S_OK;
        try{auto file=operation.GetResults();if(file){auto read=winrt::Windows::Storage::FileIO::ReadTextAsync(file);while(read.Status()==AsyncStatus::Started){MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}if(GetTickCount64()>deadline)winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_TIMEOUT));MsgWaitForMultipleObjectsEx(0,nullptr,10,QS_ALLINPUT,MWMO_INPUTAVAILABLE);}consumed=file.Path()==f.path&&read.GetResults()==L"winrt-live-consumed";}else canceled=true;}catch(const winrt::hresult_error& e){result=e.code();canceled=operation.Status()==AsyncStatus::Canceled;}
        // Cancellation may complete before transport owner restoration. Pump the
        // same STA until its original enabled state returns, with a hard bound.
        const auto restore_deadline=GetTickCount64()+5000;
        while((!IsWindowEnabled(f.owner)||!transport_done.load())&&GetTickCount64()<restore_deadline){MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}MsgWaitForMultipleObjectsEx(0,nullptr,10,QS_ALLINPUT,MWMO_INPUTAVAILABLE);}
        const bool focus=!f.switch_mode||(f.switched&&GetForegroundWindow()==f.other);
        if(!transport_done.load()){printf("FAIL transport worker did not finish; no fixture teardown\\n");ExitProcess(3);}
        const bool okay=calls==(runtime?0u:1u)&&f.shown==1&&!f.identity_failed&&IsWindowEnabled(f.owner)&&focus&&(f.cancel?canceled:consumed);
        printf("RESULT hr=%08lX calls=%u pulse_ui=%u consumed=%d canceled=%d owner_enabled=%d focus=%d pass=%d\n",static_cast<unsigned long>(result),calls.load(),f.shown,consumed,canceled,IsWindowEnabled(f.owner),focus,okay);return okay?0:1;
    }catch(const winrt::hresult_error& e){printf("FAIL hr=%08lX\n",static_cast<unsigned long>(e.code().value));return 1;}catch(const std::exception& e){printf("FAIL exception=%s\n",e.what());return 1;}
}

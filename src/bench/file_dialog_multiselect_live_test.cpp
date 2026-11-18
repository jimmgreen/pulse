#include "../shell_bridge/file_dialog_wrapper.h"
#ifdef PULSE_REMOTE_PROVIDER_TEST
#include "../shell_bridge/file_dialog_remote_provider.h"
#else
#include "../shell_bridge/file_dialog_ui_provider.h"
#endif
#include <shlobj.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
using namespace pulse::shell_bridge;
using Microsoft::WRL::ComPtr;
namespace {
// Snapshot requests: the remote picker HWND lives in pulse.exe, so pass the
// path as a global atom; in-process pickers accept a same-process pointer.
HANDLE SnapshotRequest(const std::wstring& path) {
#ifdef PULSE_REMOTE_PROVIDER_TEST
    return reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(GlobalAddAtomW(path.c_str())));
#else
    return const_cast<wchar_t*>(path.c_str());
#endif
}
PulsePickerUiV2Api real_api{};
unsigned failures=0, ready=0, file_ok=0;
uint64_t accepted_generation=0;
std::thread driver;
bool typed=false;
bool deselect=false;
std::wstring marquee_mode;
unsigned expected_count=10;
std::filesystem::path snapshot_directory;
std::wstring typed_names;
std::vector<std::wstring> packed;
void Check(bool ok,const char* label) { std::printf("[%s] %s\n",ok?"PASS":"FAIL",label); failures+=!ok; }
struct Forward { const PulsePickerUiV2Callbacks* original; };
HRESULT WINAPI Event(void* context,const PulsePickerUiV2Event* event) {
    auto& forward=*static_cast<Forward*>(context);
    if(event->kind==PULSE_PICKER_UI_V2_EVENT_FILE_OK) { ++file_ok;accepted_generation=event->generation; }
    const HRESULT hr=forward.original->callback ? forward.original->callback(forward.original->context,event):S_OK;
    if(event->kind==PULSE_PICKER_UI_V2_EVENT_READY) {
        ++ready;
        DWORD pid=0; GetWindowThreadProcessId(event->window,&pid);
        HWND edit=GetDlgItem(event->window,102);
#ifdef PULSE_REMOTE_PROVIDER_TEST
        Check(pid!=0 && pid!=GetCurrentProcessId() && IsWindowVisible(event->window) && edit,"real remote dialog and filename editor ready");
        Check(GetModuleHandleW(L"PulsePickerUiProof.dll")==nullptr,"caller has no picker UI DLL loaded");
#else
        Check(pid==GetCurrentProcessId() && IsWindowVisible(event->window) && edit,"real own dialog and filename editor ready");
#endif
        // Submit the preconfigured quoted filenames through the real editor.
        if(edit) {
            const HWND window=event->window;const DWORD sta=GetCurrentThreadId();
            driver=std::thread([window,edit,sta] {
                if(!SetThreadDesktop(GetThreadDesktop(sta)))return;
                for(unsigned i=0;i<1000 && IsWindow(window) && !IsWindowEnabled(edit);++i)Sleep(10);
                if(typed){DWORD_PTR result=0;SendMessageTimeoutW(edit,WM_SETTEXT,0,reinterpret_cast<LPARAM>(typed_names.c_str()),SMTO_ABORTIFHUNG,2000,&result);PostMessageW(edit,WM_KEYDOWN,VK_RETURN,0);return;}
                DWORD_PTR ignored=0;
                const bool attached=AttachThreadInput(GetCurrentThreadId(),sta,TRUE)!=FALSE;
                if(!attached)return;
                BYTE saved[256]{},keys[256]{};GetKeyboardState(saved);
                SendMessageTimeoutW(window,WM_KEYDOWN,VK_HOME,0,SMTO_ABORTIFHUNG,2000,&ignored);
                memcpy(keys,saved,sizeof(keys));keys[VK_CONTROL]=0x80;SetKeyboardState(keys);
                if(marquee_mode!=L"--marquee-shift")SendMessageTimeoutW(window,WM_KEYDOWN,'A',0,SMTO_ABORTIFHUNG,2000,&ignored);
                if(!marquee_mode.empty()) {
                    using GetDpiContext=HANDLE(WINAPI*)(HWND);
                    using SetDpiContext=HANDLE(WINAPI*)(HANDLE);
                    const auto user=GetModuleHandleW(L"user32.dll");
                    const auto get_context=reinterpret_cast<GetDpiContext>(GetProcAddress(user,"GetWindowDpiAwarenessContext"));
                    const auto set_context=reinterpret_cast<SetDpiContext>(GetProcAddress(user,"SetThreadDpiAwarenessContext"));
                    const HANDLE previous_context=get_context && set_context ? set_context(get_context(window)):nullptr;
                    memcpy(keys,saved,sizeof(keys));
                    keys[VK_CONTROL]=(marquee_mode==L"--marquee-ctrl" || marquee_mode==L"--marquee-cancel" || marquee_mode==L"--marquee-capture")?0x80:0;
                    keys[VK_SHIFT]=marquee_mode==L"--marquee-shift"?0x80:0;
                    SetKeyboardState(keys);
                    // Isolated default-details fixture: list top 160 DIP, rows
                    // 34 DIP. Start in unused name-column space, not on a file.
                    using WindowDpi=UINT(WINAPI*)(HWND);
                    const auto window_dpi=reinterpret_cast<WindowDpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetDpiForWindow"));
                    const float scale=(window_dpi?window_dpi(window):96)/96.0f;
                    const int first=marquee_mode==L"--marquee-shift"?2:0;
                    const int last=marquee_mode==L"--marquee-scroll"?12:marquee_mode==L"--marquee-shift"?4:3;
                    auto point=[&](int x,int row){return MAKELPARAM(static_cast<int>(x*scale),static_cast<int>((178+34*row)*scale));};
                    SendMessageTimeoutW(window,WM_LBUTTONDOWN,MK_LBUTTON,point(440,first),SMTO_ABORTIFHUNG,2000,&ignored);
                    Check(GetCapture()==window,"blank list press captures marquee gesture");
                    SendMessageTimeoutW(window,WM_MOUSEMOVE,MK_LBUTTON,point(500,last),SMTO_ABORTIFHUNG,2000,&ignored);
                    if(marquee_mode==L"--marquee-scroll") {
                        bool reached=false;
                        for(unsigned attempt=0;attempt<500 && !reached;++attempt) {
                            wchar_t names[4096]{};
                            SendMessageTimeoutW(edit,WM_GETTEXT,std::size(names),reinterpret_cast<LPARAM>(names),SMTO_ABORTIFHUNG,2000,&ignored);
                            reached=std::wstring_view(names).find(L"photo 39.jpg")!=std::wstring_view::npos;
                            if(!reached)Sleep(10);
                        }
                        Check(reached,"marquee edge scroll reaches offscreen final item");
                    }
                    const auto snapshot=snapshot_directory/L"marquee-active.png";
                    SetPropW(window,L"Pulse.PickerTestSnapshot",SnapshotRequest(snapshot));
                    SendMessageTimeoutW(window,WM_PAINT,0,0,SMTO_ABORTIFHUNG,2000,&ignored);
                    RemovePropW(window,L"Pulse.PickerTestSnapshot");
                    if(marquee_mode==L"--marquee-cancel")SendMessageTimeoutW(window,WM_KEYDOWN,VK_ESCAPE,0,SMTO_ABORTIFHUNG,2000,&ignored);
                    else if(marquee_mode==L"--marquee-capture")ReleaseCapture();
                    else SendMessageTimeoutW(window,WM_LBUTTONUP,0,point(500,last),SMTO_ABORTIFHUNG,2000,&ignored);
                    Check(IsWindowVisible(window) && GetCapture()!=window,"gesture ends without closing dialog or retaining capture");
                    if(previous_context)set_context(previous_context);
                }
                if(deselect) {
                    SendMessageTimeoutW(window,WM_KEYDOWN,VK_SPACE,0,SMTO_ABORTIFHUNG,2000,&ignored);
                    SendMessageTimeoutW(window,WM_KEYDOWN,VK_DOWN,0,SMTO_ABORTIFHUNG,2000,&ignored);
                    const auto snapshot=snapshot_directory/L"focus-without-selection-change.png";
                    SetPropW(window,L"Pulse.PickerTestSnapshot",SnapshotRequest(snapshot));
                    SendMessageTimeoutW(window,WM_PAINT,0,0,SMTO_ABORTIFHUNG,2000,&ignored);
                    RemovePropW(window,L"Pulse.PickerTestSnapshot");
                    SendMessageTimeoutW(window,WM_KEYDOWN,VK_SPACE,0,SMTO_ABORTIFHUNG,2000,&ignored);
                    const auto removed=snapshot_directory/L"ctrl-deselected.png";
                    SetPropW(window,L"Pulse.PickerTestSnapshot",SnapshotRequest(removed));
                    SendMessageTimeoutW(window,WM_PAINT,0,0,SMTO_ABORTIFHUNG,2000,&ignored);
                    RemovePropW(window,L"Pulse.PickerTestSnapshot");
                    wchar_t names[4096]{};
                    SendMessageTimeoutW(edit,WM_GETTEXT,std::size(names),reinterpret_cast<LPARAM>(names),SMTO_ABORTIFHUNG,2000,&ignored);
                    const std::wstring text(names);
                    Check(text.find(L"photo 0.jpg")==std::wstring::npos && text.find(L"photo 1.jpg")==std::wstring::npos && text.find(L"photo 2.jpg")!=std::wstring::npos,"Ctrl deselection updates filename editor");
                }
                SetKeyboardState(saved);AttachThreadInput(GetCurrentThreadId(),sta,FALSE);
                PostMessageW(window,WM_KEYDOWN,VK_RETURN,0);
            });
        }
    }
    return hr;
}
HRESULT WINAPI Show(PulsePickerUiV2Session session,const PulsePickerUiV2Config* config,
    const PulsePickerUiV2Callbacks* callbacks,PulsePickerUiV2Result* result) {
    Forward forward{callbacks}; auto proxy=*callbacks;proxy.context=&forward;proxy.callback=Event;
    const HRESULT hr=real_api.Show(session,config,&proxy,result);
    std::printf("REAL_SHOW hr=%08lx status=%u count=%u units=%u generation=%llu fileok_generation=%llu\n",
        static_cast<unsigned long>(hr),result->status,result->path_count,result->paths.units,
        static_cast<unsigned long long>(result->generation),static_cast<unsigned long long>(accepted_generation));
    Check(hr==S_OK && result->path_count==expected_count && result->generation==accepted_generation,"actual provider terminal generation and count");
    const auto& buffer=result->paths;
    size_t at=0;
    for(unsigned i=0;i<result->path_count && at<buffer.units && buffer.units<buffer.capacity;++i) {
        size_t end=at;while(end<buffer.units && buffer.data[end])++end;
        if(end==buffer.units)break;
        packed.emplace_back(buffer.data+at,end-at);at=end+1;
    }
    Check(packed.size()==expected_count,"actual packed array has expected bounded strings");
    return hr;
}
std::wstring ItemPath(IShellItem* item) {
    PWSTR text=nullptr;std::wstring out;
    if(item && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&text)))out=text;
    CoTaskMemFree(text);return out;
}
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=3 && argc!=4)return 2;
    typed=argc==4 && wcscmp(argv[3],L"--typed")==0;
    deselect=argc==4 && wcscmp(argv[3],L"--deselect")==0;
    if(argc==4 && (std::wstring_view(argv[3])==L"--marquee" || std::wstring_view(argv[3])==L"--marquee-ctrl" ||
        std::wstring_view(argv[3])==L"--marquee-shift" || std::wstring_view(argv[3])==L"--marquee-cancel" ||
        std::wstring_view(argv[3])==L"--marquee-capture" || std::wstring_view(argv[3])==L"--marquee-scroll"))marquee_mode=argv[3];
    if(argc==4 && !typed && !deselect && marquee_mode.empty())return 2;
    expected_count=marquee_mode==L"--marquee-scroll"?40:deselect?8:marquee_mode==L"--marquee-ctrl"?6:
        (marquee_mode==L"--marquee" || marquee_mode==L"--marquee-shift")?4:10;
    std::setvbuf(stdout,nullptr,_IONBF,0);
    const std::filesystem::path root(argv[2]);
    snapshot_directory=root;
    std::vector<wchar_t> env(32768);
    if(!root.is_absolute() || !GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR",env.data(),static_cast<DWORD>(env.size())) || root!=std::filesystem::path(env.data()) || !std::filesystem::is_empty(root))return 2;
    if(!GetEnvironmentVariableW(L"LOCALAPPDATA",env.data(),static_cast<DWORD>(env.size())) || std::filesystem::path(env.data()).parent_path()!=root)return 2;
    std::filesystem::create_directories(std::filesystem::path(env.data()));
    HANDLE finished=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!finished)return 2;
    std::thread watchdog([&]{if(WaitForSingleObject(finished,30000)==WAIT_TIMEOUT){std::puts("[FAIL] watchdog");TerminateProcess(GetCurrentProcess(),99);}});
    HRESULT apartment=OleInitialize(nullptr);
    if(SUCCEEDED(apartment)) {
        const auto files=root/L"files";std::filesystem::create_directory(files);
        std::vector<std::wstring> expected;std::wstring names;
        const int file_count=marquee_mode==L"--marquee-scroll"?40:10;
        for(int i=0;i<file_count;++i){auto name=L"photo "+std::wstring(file_count>10 && i<10?L"0":L"")+std::to_wstring(i)+L".jpg";auto path=files/name;std::ofstream(path,std::ios::binary)<<"isolated jpg path fixture";expected.push_back(path.wstring());names+=L"\""+name+L"\" ";}
        if(deselect)expected.erase(expected.begin(),expected.begin()+2);
        if(marquee_mode==L"--marquee")expected.resize(4);
        if(marquee_mode==L"--marquee-ctrl")expected.erase(expected.begin(),expected.begin()+4);
        if(marquee_mode==L"--marquee-shift"){expected.resize(5);expected.erase(expected.begin()+1);}
        std::shared_ptr<const FileDialogInprocessProvider> loaded;
        #ifdef PULSE_REMOTE_PROVIDER_TEST
        Check(LoadFileDialogRemoteProvider(argv[1],loaded,true)==S_OK && loaded,"real remote provider loaded");
#else
        Check(LoadFileDialogUiProvider(argv[1],loaded,true)==S_OK && loaded,"real provider loaded");
#endif
        HMODULE system=LoadLibraryExW(L"comdlg32.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        using GetClass=HRESULT(WINAPI*)(REFCLSID,REFIID,void**);
        auto get=system?reinterpret_cast<GetClass>(GetProcAddress(system,"DllGetClassObject")):nullptr;
        ComPtr<IClassFactory> factory;ComPtr<IFileOpenDialog> native,dialog;
        HRESULT hr=get?get(CLSID_FileOpenDialog,IID_PPV_ARGS(&factory)):E_FAIL;
        if(SUCCEEDED(hr))hr=factory->CreateInstance(nullptr,IID_PPV_ARGS(&native));
        Check(SUCCEEDED(hr)&&native,"system comdlg32 class factory bypasses registrations");
        unsigned native_calls=0;
        if(loaded&&native) {
            auto observed=std::make_shared<FileDialogInprocessProvider>(*loaded);real_api=loaded->api;observed->api.Show=Show;
            FileDialogBridgeServices services;services.executable=argv[1];services.inprocess_provider=observed;services.access_policy=AccessPolicy::LocalOpenV1;
            services.validation_verified=[](const ContractDecision& d){return d.disposition==Disposition::Prepared && d.validation_category==ValidationCategory::LocalOpenAccessV1;};
            services.show_native=[&](IFileDialog*,HWND){++native_calls;return E_UNEXPECTED;};
            hr=WrapNativeFileDialog(DialogFactory::Open,native.Get(),services,IID_PPV_ARGS(&dialog));
            Check(hr==S_OK&&dialog,"WrapNative creates real wrapper");
            if(dialog) {
            ComPtr<IShellItem> folder; if(SUCCEEDED(hr))hr=SHCreateItemFromParsingName(files.c_str(),nullptr,IID_PPV_ARGS(&folder));
            if(SUCCEEDED(hr))hr=dialog->SetOptions(FOS_ALLOWMULTISELECT|FOS_FORCEFILESYSTEM|FOS_FILEMUSTEXIST|FOS_PATHMUSTEXIST|FOS_NOCHANGEDIR|FOS_DONTADDTORECENT);
            if(SUCCEEDED(hr))hr=dialog->SetFolder(folder.Get());
            if(SUCCEEDED(hr))hr=dialog->SetFileName(L"");
            typed_names=names;
            Check(hr==S_OK,"wrapper configures isolated ten-JPG folder and multi-select");
            HWND owner=CreateWindowW(L"STATIC",L"Isolated multi-select wrapper",WS_OVERLAPPEDWINDOW|WS_VISIBLE,80,80,600,400,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
            if(SUCCEEDED(hr))hr=dialog->Show(owner);
            if(driver.joinable())driver.join();
            std::printf("WRAPPER_SHOW hr=%08lx\n",static_cast<unsigned long>(hr));
            Check(hr==S_OK && ready==1 && file_ok==1 && !native_calls,"wrapper returns successful real UI acceptance without fallback");
            ComPtr<IShellItemArray> results;HRESULT got=dialog->GetResults(&results);DWORD count=0;if(results)results->GetCount(&count);
            std::printf("GET_RESULTS hr=%08lx count=%lu\n",static_cast<unsigned long>(got),count);
            Check(got==S_OK&&count==expected_count,"GetResults returns exactly the remaining selection after Show ends");
            for(DWORD i=0;results&&i<count;++i){ComPtr<IShellItem> item;HRESULT item_hr=results->GetItemAt(i,&item);Check(item_hr==S_OK&&i<expected.size()&&ItemPath(item.Get())==expected[i],"IShellItemArray exact ordered filesystem path");}
            ComPtr<IShellItem> first;Check(dialog->GetResult(&first)==E_UNEXPECTED&&!first,"GetResult rejects multiple results per wrapper contract");
            Check(packed==expected,"packed provider paths and wrapper results agree");
            Check(IsWindowEnabled(owner)!=FALSE,"caller owner restored");DestroyWindow(owner);
            }
        }
        dialog.Reset();native.Reset();factory.Reset();if(system)FreeLibrary(system);loaded.reset();
        OleUninitialize();
    } else Check(false,"STA initializes");
    SetEvent(finished);watchdog.join();CloseHandle(finished);
    std::printf("failures=%u\n",failures);return failures?1:0;
}

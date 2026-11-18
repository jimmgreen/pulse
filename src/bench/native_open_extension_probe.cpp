// Native oracle only. WM_COMMAND submission is programmatic, not SendInput.
#include <windows.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <filesystem>
#include <vector>
#include <string>
#include <cstdio>
#include <thread>
#include <algorithm>
using Microsoft::WRL::ComPtr;
namespace fs=std::filesystem;
namespace {
int failures=0;
void Check(bool ok,const char* text){std::printf("[%s] %s\n",ok?"PASS":"FAIL",text);if(!ok)++failures;std::fflush(stdout);}
std::wstring Guid(){GUID id{};wchar_t name[40]{};if(FAILED(CoCreateGuid(&id)) || StringFromGUID2(id,name,40)!=39)throw 1;return name;}
struct State {
    IFileOpenDialog* dialog=nullptr;DWORD thread=GetCurrentThreadId();HWND window=nullptr;
    bool ready=false,ticking=false,submitted=false,timed_out=false,cancel=false;ULONGLONG start=0,action=0;
    bool acknowledge_open_error=false;
    unsigned phase=0,file_ok=0;UINT change_index=0;std::wstring filename;
    std::vector<UINT> type_sequence;
    std::vector<std::wstring> sequence_names, observed_names;
    size_t sequence_step=0;
    fs::path fixture;
    unsigned type_events=0, type_before=0;
    UINT observed_type=0;
    HWND Window(){
        ComPtr<IOleWindow> ole;HWND found=nullptr;DWORD pid=0;
        if(FAILED(dialog->QueryInterface(IID_PPV_ARGS(&ole))) || FAILED(ole->GetWindow(&found)) ||
            !found || GetWindowThreadProcessId(found,&pid)!=thread || pid!=GetCurrentProcessId())return nullptr;
        return found;
    }
    void CancelOwnedPrompt(){
        struct Search {State* state;HWND button=nullptr;unsigned matches=0;} found{this};
        EnumThreadWindows(thread,[](HWND popup,LPARAM parameter)->BOOL{
            auto& popup_search=*reinterpret_cast<Search*>(parameter);DWORD pid=0;
            wchar_t klass[32]{};
            if(GetWindow(popup,GW_OWNER)!=popup_search.state->window || !IsWindowVisible(popup) || !IsWindowEnabled(popup) ||
                GetWindowThreadProcessId(popup,&pid)!=popup_search.state->thread || pid!=GetCurrentProcessId() ||
                !GetClassNameW(popup,klass,32) || wcscmp(klass,L"#32770"))return TRUE;
            EnumChildWindows(popup,[](HWND child,LPARAM child_parameter)->BOOL{
                auto& button_search=*reinterpret_cast<Search*>(child_parameter);wchar_t button_class[32]{},text[128]{};
                if(!IsWindowVisible(child) || !IsWindowEnabled(child) || !GetClassNameW(child,button_class,32) ||
                    _wcsicmp(button_class,L"Button"))return TRUE;
                GetWindowTextW(child,text,128);
                const int id=GetDlgCtrlID(child);
                if((button_search.state->acknowledge_open_error && id==IDOK) || id==IDCANCEL || id==IDNO || !wcscmp(text,L"Cancel") || !wcscmp(text,L"&Cancel") ||
                    !wcscmp(text,L"取消") || !wcscmp(text,L"No") || !wcscmp(text,L"&No") || !wcscmp(text,L"否")){
                    button_search.button=child;++button_search.matches;
                }
                return TRUE;
            },parameter);
            return TRUE;
        },reinterpret_cast<LPARAM>(&found));
        if(found.matches==1){
            const BOOL posted=PostMessageW(found.button,BM_CLICK,0,0);
            std::printf("OWNED_PROMPT negative_matches=1 posted=%d (not close evidence)\n",posted);
        }
    }
    bool SelectOwnedType(UINT index) {
        struct Search { HWND combo=nullptr;unsigned matches=0; } found;
        EnumChildWindows(window,[](HWND child,LPARAM parameter)->BOOL {
            auto& result=*reinterpret_cast<Search*>(parameter);
            wchar_t klass[64]{};
            if(!IsWindowVisible(child) || !IsWindowEnabled(child) || !GetClassNameW(child,klass,64) || _wcsicmp(klass,L"ComboBox"))return TRUE;
            const LRESULT count=SendMessageW(child,CB_GETCOUNT,0,0);
            std::printf("TYPE_COMBO candidate=%p id=%d count=%lld\n",child,GetDlgCtrlID(child),static_cast<long long>(count));
            if(count!=4)return TRUE;
            const wchar_t* expected[]={L"Text",L"Markdown",L"All",L"Extension mask"};
            for(int i=0;i<4;++i) {
                const LRESULT length=SendMessageW(child,CB_GETLBTEXTLEN,i,0);
                if(length<0 || length>1024)return TRUE;
                std::vector<wchar_t> text(static_cast<size_t>(length)+1);
                if(SendMessageW(child,CB_GETLBTEXT,i,reinterpret_cast<LPARAM>(text.data()))==CB_ERR ||
                    std::wstring(text.data()).find(expected[i])==std::wstring::npos)return TRUE;
            }
            result.combo=child;++result.matches;return TRUE;
        },reinterpret_cast<LPARAM>(&found));
        Check(found.matches==1,"identify unique native file-type combo by own filter labels");
        if(found.matches!=1)return false;
        type_before=type_events;
        SetFocus(found.combo);
        SendMessageW(found.combo,CB_SHOWDROPDOWN,TRUE,0);
        const auto key=[&](UINT value) {
            const LPARAM scan=static_cast<LPARAM>(MapVirtualKeyW(value,MAPVK_VK_TO_VSC))<<16;
            SendMessageW(found.combo,WM_KEYDOWN,value,1|scan);
            SendMessageW(found.combo,WM_KEYUP,value,static_cast<LPARAM>(0xC0000001u)|scan);
        };
        key(VK_HOME);
        for(UINT i=1;i<index;++i)key(VK_DOWN);
        key(VK_RETURN);
        const LRESULT selected=SendMessageW(found.combo,CB_GETCURSEL,0,0);
        // Native file dialogs commit a drop-down selection on SELENDOK, not
        // merely SELCHANGE. Keep the actual event/index confirmation below.
        const BOOL posted=selected==static_cast<LRESULT>(index-1) && PostMessageW(GetParent(found.combo),WM_COMMAND,
            MAKEWPARAM(GetDlgCtrlID(found.combo),CBN_SELENDOK),reinterpret_cast<LPARAM>(found.combo));
        std::printf("TYPE_COMBO keyboard_selected=%lld requested=%u commit_posted=%d\n",static_cast<long long>(selected),index,posted);
        return posted!=FALSE;
    }
    void Tick(){
        if(ticking)return;ticking=true;
        struct Reset{bool& value;~Reset(){value=false;}} reset{ticking};
        if(GetTickCount64()-start>20000){timed_out=true;CancelOwnedPrompt();dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));return;}
        HWND current=Window();if(!current || !IsWindowVisible(current))return;
        if(!window)window=current;
        if(current!=window){timed_out=true;dialog->Close(E_UNEXPECTED);return;}
        if(!ready)return;
        if(phase==0){
            phase=1;action=GetTickCount64();
            if(!type_sequence.empty()) {
                change_index=type_sequence[sequence_step];
                filename=sequence_names[sequence_step];
                std::printf("SEQUENCE step=%zu requested_type=%u name=%ls\n",sequence_step,change_index,filename.c_str());
            }
            if(!type_sequence.empty()) {
                if(!SelectOwnedType(change_index)) {phase=3;dialog->Close(E_FAIL);return;}
                phase=4;return;
            }
            if(change_index){const HRESULT hr=dialog->SetFileTypeIndex(change_index);std::printf("SET_TYPE index=%u hr=%08lX\n",change_index,static_cast<unsigned long>(hr));}
            const HRESULT hr=dialog->SetFileName(filename.c_str());
            std::printf("SET_NAME hr=%08lX text=%ls\n",static_cast<unsigned long>(hr),filename.c_str());action=GetTickCount64();
        }else if(phase==4) {
            UINT current_index=0;const HRESULT queried=dialog->GetFileTypeIndex(&current_index);
            if(type_events>type_before && observed_type==change_index && SUCCEEDED(queried) && current_index==change_index) {
                Check(true,"real OnTypeChange and GetFileTypeIndex confirm requested type before submit");
                const HRESULT hr=dialog->SetFileName(filename.c_str());
                std::printf("SET_NAME hr=%08lX text=%ls\n",static_cast<unsigned long>(hr),filename.c_str());
                phase=1;action=GetTickCount64();
            }else if(GetTickCount64()-action>=4000) {
                Check(false,"native combo change did not produce matching OnTypeChange");
                std::printf("TYPE_NOT_CONFIRMED events=%u baseline=%u observed=%u queried=%u hr=%08lX\n",
                    type_events,type_before,observed_type,current_index,static_cast<unsigned long>(queried));
                phase=3;dialog->Close(E_FAIL);
            }
        }else if(phase==1 && GetTickCount64()-action>=500){
            if(cancel){phase=2;action=GetTickCount64();submitted=PostMessageW(window,WM_COMMAND,MAKEWPARAM(IDCANCEL,BN_CLICKED),0)!=FALSE;return;}
            phase=2;action=GetTickCount64();submitted=PostMessageW(window,WM_COMMAND,MAKEWPARAM(IDOK,BN_CLICKED),0)!=FALSE;
            std::printf("SUBMIT posted=%d error=%lu (not acceptance evidence)\n",submitted,submitted?0:GetLastError());
        }else if(phase==2 && GetTickCount64()-action>=5000){
            // Observation that validation did not reach FileOk; do not call this success.
            phase=3;std::printf("NOT_ACCEPTED file_ok=%u requesting_cancel=1\n",file_ok);
            CancelOwnedPrompt();dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
        }else if(phase==3){
            CancelOwnedPrompt();dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
        }
    }
};
thread_local State* active=nullptr;
void CALLBACK Timer(HWND,UINT,UINT_PTR,DWORD){try{if(active)active->Tick();}catch(...){if(active){active->timed_out=true;active->dialog->Close(E_UNEXPECTED);}}}
class Sink final:public IFileDialogEvents{
    ULONG refs_=1;State& state_;
public:
    explicit Sink(State& state):state_(state){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out)override{if(!out)return E_POINTER;*out=nullptr;if(iid==IID_IUnknown || iid==IID_IFileDialogEvents){*out=static_cast<IFileDialogEvents*>(this);AddRef();return S_OK;}return E_NOINTERFACE;}
    ULONG STDMETHODCALLTYPE AddRef()override{return ++refs_;}
    ULONG STDMETHODCALLTYPE Release()override{const auto count=--refs_;if(!count)delete this;return count;}
    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog* dialog)override{
        ++state_.file_ok;PWSTR name=nullptr;const HRESULT hr=dialog->GetFileName(&name);UINT index=0;const HRESULT index_hr=dialog->GetFileTypeIndex(&index);
        std::printf("FILE_OK count=%u filename_hr=%08lX filename=%ls type_hr=%08lX index=%u\n",
            state_.file_ok,static_cast<unsigned long>(hr),name?name:L"",static_cast<unsigned long>(index_hr),index);CoTaskMemFree(name);
        if(!state_.type_sequence.empty()) {
            ComPtr<IShellItem> item;PWSTR path=nullptr;
            HRESULT selected=dialog->GetResult(&item);
            if(SUCCEEDED(selected))selected=item->GetDisplayName(SIGDN_FILESYSPATH,&path);
            const bool valid=SUCCEEDED(selected) && path && fs::path(path).parent_path()==state_.fixture && fs::exists(path);
            Check(valid,"sequence FileOk exposes an existing owned result before veto");
            Check(SUCCEEDED(index_hr) && index==state_.type_sequence[state_.sequence_step],"sequence requested file type is active at FileOk");
            std::printf("SEQUENCE_RESULT step=%zu type=%u result_hr=%08lX path=%ls\n",
                state_.sequence_step,index,static_cast<unsigned long>(selected),path?path:L"");
            state_.observed_names.push_back(path?fs::path(path).filename().wstring():L"");
            CoTaskMemFree(path);
            if(state_.sequence_step+1<state_.type_sequence.size()) {
                ++state_.sequence_step;state_.phase=0;state_.action=GetTickCount64();
                return S_FALSE; // Observe each effective extension in one live dialog.
            }
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog*,IShellItem*)override{return S_OK;}
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog*)override{state_.ready=true;return S_OK;}
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog*)override{return S_OK;}
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*,IShellItem*,FDE_SHAREVIOLATION_RESPONSE* response)override{*response=FDESVR_DEFAULT;return S_OK;}
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog* dialog)override{
        ++state_.type_events;UINT index=0;const HRESULT hr=dialog->GetFileTypeIndex(&index);
        if(SUCCEEDED(hr))state_.observed_type=index;
        std::printf("TYPE_CHANGE count=%u index=%u hr=%08lX\n",state_.type_events,index,static_cast<unsigned long>(hr));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog*,IShellItem*,FDE_OVERWRITE_RESPONSE* response)override{*response=FDEOR_DEFAULT;return S_OK;}
};
}
int wmain(int argc,wchar_t** argv){
    std::setvbuf(stdout,nullptr,_IONBF,0);
    if(argc!=2 && argc!=3 && argc!=4){std::puts("cases: exact-only extension-only both explicit multi type-change all-type wildcard-sequence");return 2;}
    const std::wstring mode=argv[1];
    const bool extension_value=mode==L"--extension-value";
    if(extension_value && argc!=4)return 2;
    const std::wstring extension_text=extension_value?argv[2]:L"txt";
    const std::wstring extension_case=extension_value?argv[3]:L"";
    if(extension_value && extension_case!=L"explicit" && extension_case!=L"multi" && extension_case!=L"bare" && extension_case!=L"suffix")return 2;
    const bool xmind=mode==L"--xmind-open";
    const std::wstring variant=argc==3?argv[2]:L"multi";
    if(xmind && variant!=L"single" && variant!=L"multi" && variant!=L"missing" && variant!=L"missing-parent")return 2;
    const bool reject=xmind && (variant==L"missing" || variant==L"missing-parent");
    const bool multi=(extension_value && extension_case==L"multi") || mode==L"multi" || (xmind && variant==L"multi");
    const bool wildcard_sequence=mode==L"wildcard-sequence";
    const bool cwd_case=mode==L"cwd-accept" || mode==L"cwd-cancel";
    const auto initial_cwd=fs::current_path();
    if(mode!=L"exact-only" && mode!=L"extension-only" && mode!=L"both" && mode!=L"explicit" &&
        !extension_value && !xmind && !multi && !cwd_case && !wildcard_sequence && mode!=L"type-change" && mode!=L"all-type")return 2;
    HANDLE done=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!done)return 2;
    std::thread watchdog([done]{if(WaitForSingleObject(done,45000)!=WAIT_OBJECT_0){std::puts("WATCHDOG fixture retained");std::fflush(stdout);TerminateProcess(GetCurrentProcess(),99);}});
    fs::path root;std::vector<fs::path> files;
    try{
        const auto bench=fs::canonical(L"bench_data");root=bench/(L"native-open-extension-"+Guid());
        if(!CreateDirectoryW(root.c_str(),nullptr))throw 2;
        std::vector<std::wstring> names;
        if(wildcard_sequence) {
            for(const auto* stem:{L"literal",L"wildcard",L"all"})
                for(const auto* extension:{L".txt",L".md",L".xlsx",L".xls"}) names.emplace_back(std::wstring(stem)+extension);
        }
        else if(extension_value) {
            names={L"one.td",L"two.td",L"stem",L"stem.td",L"stem.bt",L"stem..td",L"stem.tar.gz",L"stem.tar",L"stem.gz",L"stem.td;bt"};
            if(extension_case==L"suffix")std::erase(names,std::wstring(L"stem"));
        }
        else if(xmind)names={L"one.xmind",L"two.xmind"};
        else if(multi)names={L"one.txt",L"two.txt"};
        else if(mode==L"exact-only")names={L"stem"};
        else if(mode==L"extension-only")names={L"stem.txt"};
        else if(mode==L"type-change" || mode==L"all-type")names={L"stem.txt",L"stem.log",L"stem.md"};
        else names={L"stem",L"stem.txt",L"stem.md"};
        for(const auto& name:names){
            const auto file=root/name;HANDLE h=CreateFileW(file.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(h==INVALID_HANDLE_VALUE)throw 3;CloseHandle(h);files.push_back(file);
        }
        HDESK original=GetThreadDesktop(GetCurrentThreadId());
        HDESK desktop=CreateDesktopW((L"PulseOpenExtension-"+Guid()).c_str(),nullptr,nullptr,0,GENERIC_ALL,nullptr);
        if(!desktop || !SetThreadDesktop(desktop))throw 4;
        const HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);if(FAILED(com))throw 5;
        {
            HWND owner=CreateWindowExW(0,L"STATIC",L"owned extension oracle",WS_OVERLAPPED,0,0,100,100,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
            ComPtr<IFileOpenDialog> dialog;HRESULT hr=E_FAIL;HMODULE native_module=nullptr;
            if(xmind || extension_value){
                wchar_t system[MAX_PATH]{};GetSystemDirectoryW(system,MAX_PATH);
                native_module=LoadLibraryExW((fs::path(system)/L"comdlg32.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
                using Factory=HRESULT(WINAPI*)(REFCLSID,REFIID,void**);
                auto get=native_module?reinterpret_cast<Factory>(GetProcAddress(native_module,"DllGetClassObject")):nullptr;
                ComPtr<IClassFactory> factory;
                if(get)hr=get(CLSID_FileOpenDialog,IID_PPV_ARGS(&factory));
                if(SUCCEEDED(hr))hr=factory->CreateInstance(nullptr,IID_PPV_ARGS(&dialog));
            }else hr=CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog));
            Check(owner && SUCCEEDED(hr),"private desktop owned native Open");
            if(owner && dialog){
                ComPtr<IShellItem> folder;hr=SHCreateItemFromParsingName(root.c_str(),nullptr,IID_PPV_ARGS(&folder));
                GUID client{};const HRESULT client_hr=CoCreateGuid(&client);
                const DWORD options=extension_value?(multi?0x1240u:0x1040u):xmind?0x1240u:FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|FOS_FILEMUSTEXIST|(cwd_case?FOS_FORCESHOWHIDDEN:FOS_NOCHANGEDIR)|FOS_DONTADDTORECENT|(multi?FOS_ALLOWMULTISELECT:0);
                COMDLG_FILTERSPEC filters[]={{L"Text",cwd_case?L"*":L"*.txt"},{L"Markdown",L"*.md"},{L"All",L"*.*"},
                    {L"Extension mask",L"*.xl*;*.xlsx"}};
                if(xmind){filters[0]={L"XMind",L"*.xmind"};filters[1]={L"All",L"*.*"};}
                if(extension_value){filters[0]={L"All",L"*.*"};}
                const bool configured=SUCCEEDED(client_hr) && SUCCEEDED(hr) && SUCCEEDED(dialog->SetOptions(options)) && SUCCEEDED(dialog->SetClientGuid(client)) &&
                    SUCCEEDED(dialog->SetFolder(folder.Get())) && SUCCEEDED(dialog->SetFileTypes(extension_value?1:xmind?2:wildcard_sequence?4:3,filters)) &&
                    SUCCEEDED(dialog->SetFileTypeIndex(1));
                const HRESULT extension=xmind?S_OK:dialog->SetDefaultExtension(extension_text.c_str());
                if(xmind){FILEOPENDIALOGOPTIONS actual{};Check(SUCCEEDED(dialog->GetOptions(&actual)) && actual==0x1240u,"exact XMind options retained");}
                Check(configured && extension==S_OK,"native default extension configured");
                std::printf("CASE=%ls options=%08lX default=%ls extension_hr=%08lX\n",mode.c_str(),options,extension_text.c_str(),static_cast<unsigned long>(extension));
                State state;state.cancel=mode==L"cwd-cancel";state.dialog=dialog.Get();state.filename=multi?L"\"one\" \"two\"":mode==L"explicit"?L"stem.md":L"stem";
                state.acknowledge_open_error=extension_value && extension_case==L"suffix";
                if(extension_value)state.filename=extension_case==L"multi"?L"\"one.td\" \"two.td\"":extension_case==L"explicit"?L"one.td":L"stem";
                if(xmind)state.filename=variant==L"multi"?L"\"one.xmind\" \"two.xmind\"":variant==L"single"?L"one.xmind":variant==L"missing"?L"absent.xmind":(root/L"absent-parent"/L"absent.xmind").wstring();
                if(wildcard_sequence) {
                    state.type_sequence={2,4,3};state.sequence_names={L"literal",L"wildcard",L"all"};state.fixture=root;
                }
                state.change_index=mode==L"type-change"?2:mode==L"all-type"?3:0;state.start=GetTickCount64();active=&state;
                Sink* sink=new Sink(state);DWORD cookie=0;const HRESULT advise=dialog->Advise(sink,&cookie);sink->Release();
                UINT_PTR timer=SetTimer(nullptr,0,100,Timer);
                if(configured && SUCCEEDED(extension) && SUCCEEDED(advise) && timer){
                    Check(fs::current_path()==initial_cwd,"configuration preserves process working directory");
                    hr=dialog->Show(owner);
                    Check(fs::current_path()==initial_cwd,"native Show preserves process working directory");
                    if(extension_value && extension_case!=L"suffix")Check(hr==S_OK && state.file_ok==1,"extension value accepts existing owned input");
                    if(xmind)Check(reject?(hr==HRESULT_FROM_WIN32(ERROR_CANCELLED) && state.file_ok==0):(hr==S_OK && state.file_ok==1),"XMind expected acceptance or rejection");
                    if(wildcard_sequence)Check(hr==S_OK && state.file_ok==3 && state.observed_names.size()==3,
                        "literal to wildcard to all sequence produced three observed FileOk results");
                    if(cwd_case)Check(hr==(state.cancel?HRESULT_FROM_WIN32(ERROR_CANCELLED):S_OK),"cwd oracle reaches intended outcome");
                    std::printf("SHOW hr=%08lX file_ok=%u timeout=%d submitted=%d\n",static_cast<unsigned long>(hr),state.file_ok,state.timed_out,state.submitted);
                    Check(!state.timed_out && state.submitted,"driver submitted within bounded own dialog");
                    if(hr==S_OK){
                        ComPtr<IShellItemArray> results;DWORD count=0;HRESULT result_hr=dialog->GetResults(&results);
                        if(SUCCEEDED(result_hr))result_hr=results->GetCount(&count);
                        Check(SUCCEEDED(result_hr) && count>0,"native accepted result array read");
                        if(xmind || extension_value)Check(count==(multi?2u:1u),"expected exact result count");
                        for(DWORD i=0;i<count;++i){
                            ComPtr<IShellItem> item;PWSTR path=nullptr;HRESULT item_hr=results->GetItemAt(i,&item);
                            if(SUCCEEDED(item_hr))item_hr=item->GetDisplayName(SIGDN_FILESYSPATH,&path);
                            std::printf("RESULT index=%lu hr=%08lX path=%ls\n",i,static_cast<unsigned long>(item_hr),path?path:L"");
                            Check(SUCCEEDED(item_hr) && path && fs::path(path).parent_path()==root && fs::exists(path),"result stays existing owned fixture");
                            CoTaskMemFree(path);
                        }
                    }else Check(hr==HRESULT_FROM_WIN32(ERROR_CANCELLED),"unaccepted case cancelled without fabricated result");
                }else Check(false,"configured driver");
                if(timer)KillTimer(nullptr,timer);active=nullptr;
                if(SUCCEEDED(advise))dialog->Unadvise(cookie);
                const HRESULT clear=dialog->ClearClientData();std::printf("CLEANUP ClearClientData=%08lX\n",static_cast<unsigned long>(clear));
                Check(SUCCEEDED(clear),"unique native client cleanup");
            }
            dialog.Reset();if(native_module)FreeLibrary(native_module);
            if(owner)DestroyWindow(owner);
        }
        CoUninitialize();Check(SetThreadDesktop(original)!=FALSE,"restore own thread desktop");Check(CloseDesktop(desktop)!=FALSE,"close private desktop");
        if(!failures){for(const auto& file:files)Check(DeleteFileW(file.c_str())!=FALSE,"remove exact owned file");Check(RemoveDirectoryW(root.c_str())!=FALSE,"remove empty owned fixture");}
    }catch(...){Check(false,"setup failed; fixture retained");}
    SetEvent(done);watchdog.join();CloseHandle(done);
    std::printf("failures=%d (observations are not an extension precedence specification)\n",failures);return failures?1:0;
}

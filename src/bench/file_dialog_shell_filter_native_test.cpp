#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <shlguid.h>
#include <filesystem>
#include <fstream>
#include <wrl/client.h>
#include <atomic>
#include <cstdio>
#include <string>
using Microsoft::WRL::ComPtr;
using Factory=HRESULT(WINAPI*)(REFCLSID,REFIID,void**);
namespace {
bool failure_probe=false;
struct Filter final : IShellItemFilter {
    std::atomic<ULONG> refs{1};
    unsigned added = 0, released = 0, includes = 0, enums = 0;
    DWORD caller = GetCurrentThreadId();
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (iid != IID_IUnknown && iid != IID_IShellItemFilter) return E_NOINTERFACE;
        *value = static_cast<IShellItemFilter*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { ++added; return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { ++released; return --refs; }
    HRESULT STDMETHODCALLTYPE IncludeItem(IShellItem* item) override {
        ++includes; PWSTR name=nullptr; if(item)item->GetDisplayName(SIGDN_NORMALDISPLAY,&name);
        APTTYPE type{};APTTYPEQUALIFIER qualifier{};const auto apartment=CoGetApartmentType(&type,&qualifier);
        const bool excluded=name && std::wstring(name).starts_with(L"blocked");
        const HRESULT answer=failure_probe && name && std::wstring(name)==L"blocked.txt"?E_ACCESSDENIED:failure_probe && name && std::wstring(name)==L"visible.txt"?HRESULT(2):excluded?S_FALSE:S_OK;
        std::printf("CALL IncludeItem thread=%lu expected=%lu apt_hr=%08lX apt=%d name=%ls returned=%08lX\n",GetCurrentThreadId(),caller,static_cast<unsigned long>(apartment),static_cast<int>(type),name?name:L"<null>",static_cast<unsigned long>(answer));
        CoTaskMemFree(name);return answer;
    }
    HRESULT STDMETHODCALLTYPE GetEnumFlagsForItem(IShellItem*, SHCONTF* flags) override { ++enums; if (flags) *flags = SHCONTF_FOLDERS; std::printf("CALL GetEnumFlagsForItem thread=%lu expected=%lu\n", GetCurrentThreadId(), caller); return S_OK; }
};
void Module(const void* address) {
    HMODULE module = nullptr; wchar_t path[32768]{};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(address), &module)) GetModuleFileNameW(module,path,32768);
    std::wprintf(L"MODULE %ls\n",path);
}

struct Driver { IFileDialog* dialog=nullptr;std::filesystem::path folder;ComPtr<IShellItem> item;ULONGLONG started=0,changed=0,next=0;unsigned clicks=0,file_ok=0; };
thread_local Driver* active=nullptr;
class Sink final : public IFileDialogEvents {
    std::atomic<ULONG> refs_{1};Driver& driver_;
public:
    explicit Sink(Driver& d):driver_(d){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override{if(!out)return E_POINTER;*out=nullptr;if(iid!=IID_IUnknown&&iid!=IID_IFileDialogEvents)return E_NOINTERFACE;*out=static_cast<IFileDialogEvents*>(this);AddRef();return S_OK;}
    ULONG STDMETHODCALLTYPE AddRef() override{return ++refs_;}
    ULONG STDMETHODCALLTYPE Release() override{const auto n=--refs_;if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog* dialog) override{++driver_.file_ok;ComPtr<IShellItem> result;PWSTR path=nullptr;const auto hr=dialog->GetResult(&result);if(result)result->GetDisplayName(SIGDN_FILESYSPATH,&path);std::printf("EVENT FileOk count=%u result=%08lX path=%ls thread=%lu\n",driver_.file_ok,static_cast<unsigned long>(hr),path?path:L"<null>",GetCurrentThreadId());CoTaskMemFree(path);return S_OK;}
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog*,IShellItem*) override{std::puts("EVENT FolderChanging");return S_OK;}
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog*) override{driver_.changed=GetTickCount64();std::puts("EVENT FolderChange");return S_OK;}
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog*) override{std::puts("EVENT SelectionChange");return S_OK;}
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog*) override{std::puts("EVENT TypeChange");return S_OK;}
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*,IShellItem*,FDE_SHAREVIOLATION_RESPONSE* value) override{if(!value)return E_POINTER;*value=FDESVR_DEFAULT;return S_OK;}
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog*,IShellItem*,FDE_OVERWRITE_RESPONSE* value) override{if(!value)return E_POINTER;*value=FDEOR_ACCEPT;return S_OK;}
};
void CALLBACK Tick(HWND,UINT,UINT_PTR,DWORD){
    if(!active)return;auto& d=*active;const auto now=GetTickCount64();
    if(now-d.started>12000){d.dialog->Close(HRESULT_FROM_WIN32(ERROR_TIMEOUT));return;}
    if(!d.changed||now-d.changed<1500||now<d.next)return;
    ComPtr<IShellItem> folder;PWSTR path=nullptr;d.dialog->GetFolder(&folder);if(folder)folder->GetDisplayName(SIGDN_FILESYSPATH,&path);
    const bool exact=path&&_wcsicmp(path,d.folder.c_str())==0;CoTaskMemFree(path);
    if(!exact){std::puts("DRIVER reanchor");d.dialog->SetFolder(d.item.Get());d.dialog->SetFileName(L"blocked.txt");d.next=now+1500;return;}
    ComPtr<IOleWindow> ole;HWND window=nullptr;if(FAILED(d.dialog->QueryInterface(IID_PPV_ARGS(&ole)))||FAILED(ole->GetWindow(&window))||!IsWindowVisible(window))return;
    const auto button=GetDlgItem(window,d.clicks?IDCANCEL:IDOK);if(!IsWindowEnabled(button))return;
    if(!d.clicks){
        ComPtr<IServiceProvider> service;ComPtr<IFolderView2> view;
        HRESULT view_hr=d.dialog->QueryInterface(IID_PPV_ARGS(&service));
        if(service)view_hr=service->QueryService(SID_SFolderView,IID_PPV_ARGS(&view));
        if(!view && service){ComPtr<IShellBrowser> browser;view_hr=service->QueryService(SID_STopLevelBrowser,IID_PPV_ARGS(&browser));if(browser){ComPtr<IShellView> shell;view_hr=browser->QueryActiveShellView(&shell);if(shell)view_hr=shell.As(&view);}}
        std::printf("VIEW hr=%08lX available=%u\n",static_cast<unsigned long>(view_hr),bool(view));
        if(view){ComPtr<IShellItemArray> items;view_hr=view->Items(SVGIO_ALLVIEW,IID_PPV_ARGS(&items));DWORD count=0;if(items)items->GetCount(&count);std::printf("VIEW items_hr=%08lX count=%lu\n",static_cast<unsigned long>(view_hr),count);for(DWORD i=0;i<count;++i){ComPtr<IShellItem> item;PWSTR name=nullptr;items->GetItemAt(i,&item);if(item)item->GetDisplayName(SIGDN_NORMALDISPLAY,&name);std::printf("VIEW item=%ls\n",name?name:L"<null>");CoTaskMemFree(name);}}
    }
    std::printf("DRIVER click=%s\n",d.clicks?"cancel-after-filtered-submit":"accept-typed-blocked");++d.clicks;d.next=now+2000;SendMessageW(button,BM_CLICK,0,0);
}
int Live(Factory factory,const std::filesystem::path& root){
    if(std::filesystem::exists(root))return 3;
    std::filesystem::create_directories(root/L"allowedDir");std::filesystem::create_directory(root/L"blockedDir");
    for(const auto* leaf:{L"visible.txt",L"blocked.txt"}){std::ofstream output(root/leaf);output<<"owned-filter-fixture";}
    for(unsigned kind=0;kind<(failure_probe?1u:2u);++kind){
        ComPtr<IClassFactory> cf;ComPtr<IFileDialog> dialog;Filter filter;Driver driver;driver.folder=root;
        HRESULT hr=factory?factory(kind?CLSID_FileSaveDialog:CLSID_FileOpenDialog,IID_PPV_ARGS(&cf)):E_NOINTERFACE;
        if(SUCCEEDED(hr))hr=cf->CreateInstance(nullptr,IID_PPV_ARGS(&dialog));if(FAILED(hr))return 4;
        driver.dialog=dialog.Get();SHCreateItemFromParsingName(root.c_str(),nullptr,IID_PPV_ARGS(&driver.item));
        GUID guid{};CoCreateGuid(&guid);dialog->SetClientGuid(guid);dialog->SetOptions(FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|FOS_NOCHANGEDIR|FOS_DONTADDTORECENT|(kind?FOS_NOREADONLYRETURN:FOS_FILEMUSTEXIST));
        dialog->SetFolder(driver.item.Get());dialog->SetDefaultFolder(driver.item.Get());dialog->SetFileName(L"blocked.txt");
        const auto title=L"Pulse.Filter.Native."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(kind);dialog->SetTitle(title.c_str());
        ComPtr<IFileDialogEvents> sink;sink.Attach(new Sink(driver));DWORD cookie=0;dialog->Advise(sink.Get(),&cookie);
        hr=dialog->SetFilter(&filter);std::printf("LIVE %s SetFilter=%08lX refs=%lu pid=%lu\n",kind?"Save":"Open",static_cast<unsigned long>(hr),filter.refs.load(),GetCurrentProcessId());
        active=&driver;driver.started=GetTickCount64();const auto timer=SetTimer(nullptr,0,100,Tick);hr=dialog->Show(nullptr);KillTimer(nullptr,timer);active=nullptr;
        ComPtr<IShellItem> result;PWSTR path=nullptr;const auto result_hr=dialog->GetResult(&result);if(result)result->GetDisplayName(SIGDN_FILESYSPATH,&path);
        std::printf("LIVE_TERMINAL %s hr=%08lX result_hr=%08lX file_ok=%u include=%u enum=%u path=%ls\n",kind?"Save":"Open",static_cast<unsigned long>(hr),static_cast<unsigned long>(result_hr),driver.file_ok,filter.includes,filter.enums,path?path:L"<null>");CoTaskMemFree(path);
        dialog->Unadvise(cookie);dialog.Reset();std::printf("LIVE_RELEASE refs=%lu added=%u released=%u\n",filter.refs.load(),filter.added,filter.released);
    }
    return 0;
}
}
int wmain(int argc,wchar_t** argv) {
    const HRESULT init=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED); if(FAILED(init)) return 2;
    wchar_t system[32768]{};GetSystemDirectoryW(system,32768);
    const auto module=LoadLibraryExW((std::wstring(system)+L"\\comdlg32.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto factory=module?reinterpret_cast<Factory>(GetProcAddress(module,"DllGetClassObject")):nullptr;
    Module(reinterpret_cast<const void*>(factory));
    failure_probe=argc==3 && std::wstring(argv[1])==L"--failure-live";
    if(argc==3 && (std::wstring(argv[1])==L"--live" || failure_probe)){const int result=Live(factory,std::filesystem::absolute(argv[2]));CoUninitialize();return result;}
    unsigned failures=0;
    for(unsigned kind=0;kind<(failure_probe?1u:2u);++kind){
        Filter first,second;
        {
            ComPtr<IClassFactory> cf;ComPtr<IFileDialog> dialog;
            HRESULT hr=factory?factory(kind?CLSID_FileSaveDialog:CLSID_FileOpenDialog,IID_PPV_ARGS(&cf)):E_NOINTERFACE;
            if(SUCCEEDED(hr))hr=cf->CreateInstance(nullptr,IID_PPV_ARGS(&dialog));
            std::printf("NATIVE %s create=%08lX caller_bits=%u thread=%lu\n",kind?"Save":"Open",static_cast<unsigned long>(hr),static_cast<unsigned>(sizeof(void*)*8),GetCurrentThreadId());
            if(FAILED(hr)||!dialog){++failures;continue;}
            Module((*reinterpret_cast<void***>(dialog.Get()))[26]);
            hr=dialog->SetFilter(nullptr);std::printf("SET null-before hr=%08lX\n",static_cast<unsigned long>(hr));
            hr=dialog->SetFilter(&first);std::printf("SET first hr=%08lX refs=%lu added=%u released=%u\n",static_cast<unsigned long>(hr),first.refs.load(),first.added,first.released);
            hr=dialog->SetFilter(&second);std::printf("SET second hr=%08lX first_refs=%lu second_refs=%lu\n",static_cast<unsigned long>(hr),first.refs.load(),second.refs.load());
            hr=dialog->SetFilter(nullptr);std::printf("SET null-after hr=%08lX first_refs=%lu second_refs=%lu\n",static_cast<unsigned long>(hr),first.refs.load(),second.refs.load());
        }
        std::printf("RELEASE first refs=%lu added=%u released=%u include=%u enum=%u second_refs=%lu\n",first.refs.load(),first.added,first.released,first.includes,first.enums,second.refs.load());
        if(first.refs!=1||second.refs!=1)++failures;
    }
    CoUninitialize();std::printf("NATIVE_METADATA failures=%u no_show=1\n",failures);return failures?1:0;
}

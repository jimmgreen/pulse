#include "../shell_bridge/legacy_com_provider.h"
#include <windows.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <filesystem>
#include <cstdio>
#include <string>
#include <uiautomation.h>
void DumpOwnedMessage(HWND window) {
    const HRESULT init=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(FAILED(init))return;
    {
        Microsoft::WRL::ComPtr<IUIAutomation> automation;
        Microsoft::WRL::ComPtr<IUIAutomationElement> root;
        Microsoft::WRL::ComPtr<IUIAutomationCondition> condition;
        Microsoft::WRL::ComPtr<IUIAutomationElementArray> children;
        if(SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&automation)))&&
            SUCCEEDED(automation->ElementFromHandle(window,&root))&&SUCCEEDED(automation->CreateTrueCondition(&condition))&&
            SUCCEEDED(root->FindAll(TreeScope_Subtree,condition.Get(),&children))){
            int count=0;children->get_Length(&count);
            for(int i=0;i<count;++i){Microsoft::WRL::ComPtr<IUIAutomationElement> item;BSTR name=nullptr;
                if(SUCCEEDED(children->GetElement(i,&item))&&SUCCEEDED(item->get_CurrentName(&name))&&name){char utf8[4096]{};WideCharToMultiByte(CP_UTF8,0,name,-1,utf8,4096,nullptr,nullptr);if(*utf8)std::printf("UIA_MESSAGE %s\n",utf8);SysFreeString(name);}
            }
        }
    }
    CoUninitialize();
}

using Microsoft::WRL::ComPtr;
void Results(IFileDialog* dialog,const char* phase) {
    ComPtr<IShellItem> item;HRESULT hr=dialog->GetResult(&item);
    std::printf("%s GetResult=%08lx",phase,hr);
    if(SUCCEEDED(hr)){PWSTR text=nullptr;hr=item->GetDisplayName(SIGDN_FILESYSPATH,&text);std::printf(" display=%08lx path=%ls",hr,text?text:L"");CoTaskMemFree(text);}std::printf("\n");
    ComPtr<IFileOpenDialog> open;ComPtr<IShellItemArray> items;
    hr=dialog->QueryInterface(IID_PPV_ARGS(&open));if(SUCCEEDED(hr))hr=open->GetResults(&items);
    DWORD count=0;if(SUCCEEDED(hr))items->GetCount(&count);std::printf("%s GetResults=%08lx count=%lu\n",phase,hr,count);
    for(DWORD i=0;i<count;++i){ComPtr<IShellItem> entry;items->GetItemAt(i,&entry);PWSTR text=nullptr;hr=entry->GetDisplayName(SIGDN_FILESYSPATH,&text);std::printf("item=%lu display=%08lx path=%ls attrs=%lu\n",i,hr,text?text:L"",text?GetFileAttributesW(text):0);CoTaskMemFree(text);}
}
struct Events final:IFileDialogEvents {
    ULONG refs=1;unsigned count=0;bool veto=false;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out)override{if(!out)return E_POINTER;*out=nullptr;if(iid!=IID_IUnknown&&iid!=IID_IFileDialogEvents)return E_NOINTERFACE;*out=static_cast<IFileDialogEvents*>(this);AddRef();return S_OK;}
    ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;} ULONG STDMETHODCALLTYPE Release()override{return --refs;}
    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog* d)override{++count;std::printf("FILE_OK count=%u veto=%u\n",count,veto);Results(d,"inside");return veto?S_FALSE:S_OK;}
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog*,IShellItem*)override{return S_OK;}
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog*)override{return S_OK;}
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog*)override{return S_OK;}
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*,IShellItem*,FDE_SHAREVIOLATION_RESPONSE* r)override{*r=FDESVR_DEFAULT;return S_OK;}
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog*)override{return S_OK;}
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog*,IShellItem*,FDE_OVERWRITE_RESPONSE* r)override{*r=FDEOR_DEFAULT;return S_OK;}
};
struct Drive {IFileDialog* dialog=nullptr;Events* events=nullptr;ULONGLONG start=0;bool posted=false,timed=false;};
Drive* active=nullptr;
void CALLBACK Tick(HWND,UINT,UINT_PTR,DWORD){
    if(!active)return;ComPtr<IOleWindow> ole;HWND window=nullptr;if(FAILED(active->dialog->QueryInterface(IID_PPV_ARGS(&ole)))||FAILED(ole->GetWindow(&window)))return;
    EnumThreadWindows(GetCurrentThreadId(),[](HWND popup,LPARAM parent)->BOOL{if(GetWindow(popup,GW_OWNER)==reinterpret_cast<HWND>(parent)){wchar_t title[256]{};GetWindowTextW(popup,title,256);char utf8[1024]{};WideCharToMultiByte(CP_UTF8,0,title,-1,utf8,1024,nullptr,nullptr);if(*utf8)std::printf("POPUP title=%s\n",utf8);static HWND last=nullptr;if(IsWindowVisible(popup)&&popup!=last&&*utf8&&std::wstring(title)!=L"Default IME"){last=popup;DumpOwnedMessage(popup);}const bool yes=std::wstring(title)==L"\u786e\u8ba4\u53e6\u5b58\u4e3a"||std::wstring(title)==L"Confirm Save As";if(yes)PostMessageW(popup,WM_USER+102,IDYES,0);}return TRUE;},reinterpret_cast<LPARAM>(window));
    const auto elapsed=GetTickCount64()-active->start;
    if(elapsed>500&&!active->posted&&IsWindowVisible(window)){active->posted=true;PostMessageW(window,WM_COMMAND,IDOK,0);}
    if(elapsed>5000||(elapsed>1500&&active->events->veto&&active->events->count)){
        active->timed=elapsed>5000;EnumThreadWindows(GetCurrentThreadId(),[](HWND popup,LPARAM parent)->BOOL{if(GetWindow(popup,GW_OWNER)==reinterpret_cast<HWND>(parent)){PostMessageW(popup,WM_USER+102,IDOK,0);PostMessageW(popup,WM_CLOSE,0,0);}return TRUE;},reinterpret_cast<LPARAM>(window));
        PostMessageW(window,WM_COMMAND,IDCANCEL,0);
    }
}
int main(int argc,char** argv){
    std::setvbuf(stdout,nullptr,_IONBF,0);if(FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)))return 2;
    const auto root=std::filesystem::absolute(std::filesystem::path(L"bench_data")/(L"native-save-notest-"+std::to_wstring(GetCurrentProcessId())));
    if(!std::filesystem::create_directory(root))return 2;const auto file=root/L"exists.txt";
    HANDLE owned=CreateFileW(file.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(owned==INVALID_HANDLE_VALUE)return 2;CloseHandle(owned);
    if(argc<2)SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_READONLY);
    HANDLE locked = argc > 1 && std::string(argv[1]) == "--locked" ? CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) : INVALID_HANDLE_VALUE;
    const wchar_t* names[]={L"missing.txt",L"exists.txt",L"absent\\missing.txt",L"missing.txt",L"missing.txt"};
    for(unsigned i=argc>1?1u:0u;i<3;++i){
        std::printf("PHASE create case=%u\n",i);ComPtr<IUnknown> native;HRESULT hr=pulse::shell_bridge::CreateSystemLegacyDialog(true,&native);ComPtr<IFileSaveDialog> dialog;if(SUCCEEDED(hr))hr=native.As(&dialog);if(FAILED(hr))return 2;
        std::printf("PHASE options\n");hr=dialog->SetOptions(static_cast<FILEOPENDIALOGOPTIONS>(0x1000a));std::printf("PHASE item\n");ComPtr<IShellItem> folder;SHCreateItemFromParsingName(root.c_str(),nullptr,IID_PPV_ARGS(&folder));std::printf("PHASE folder\n");dialog->SetFolder(folder.Get());std::printf("PHASE filename\n");dialog->SetFileName(names[i]);
        FILEOPENDIALOGOPTIONS options{};dialog->GetOptions(&options);Events events;events.veto=false;DWORD cookie=0;std::printf("PHASE advise\n");dialog->Advise(&events,&cookie);
        std::printf("BEGIN case=%u name=%ls set=%08lx options=%lx\n",i,names[i],hr,options);const auto cwd=std::filesystem::current_path();
        Drive state{dialog.Get(),&events,GetTickCount64()};active=&state;UINT_PTR timer=SetTimer(nullptr,0,50,Tick);hr=dialog->Show(nullptr);KillTimer(nullptr,timer);active=nullptr;
        std::printf("END case=%u show=%08lx FileOk=%u timed=%u cwd_same=%u\n",i,hr,events.count,state.timed,std::filesystem::current_path()==cwd);Results(dialog.Get(),"after");dialog->Unadvise(cookie);
    }
    if(locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
    SetFileAttributesW(file.c_str(),FILE_ATTRIBUTE_NORMAL);
    const bool clean=DeleteFileW(file.c_str())&&RemoveDirectoryW(root.c_str());std::printf("cleanup=%u\n",clean);CoUninitialize();return clean?0:1;
}

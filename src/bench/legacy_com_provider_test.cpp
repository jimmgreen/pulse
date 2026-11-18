#include "../shell_bridge/legacy_com_provider.h"
#include "../shell_bridge/legacy_dialog_error.h"
#include <windows.h>
#include <commdlg.h>
#include <filesystem>
#include <thread>
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>
#include <uiautomation.h>
#include <wrl/client.h>


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
int ReadOnlyOracle(bool readonly = true) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    const auto root=std::filesystem::absolute(std::filesystem::path(L"bench_data")/(L"legacy-readonly-"+std::to_wstring(GetCurrentProcessId())));
    if(!std::filesystem::create_directory(root)) return 2;
    const auto target=root/L"export.html";
    HANDLE owned=CreateFileW(target.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(owned==INVALID_HANDLE_VALUE)return 2;
    CloseHandle(owned);if(readonly&&!SetFileAttributesW(target.c_str(),FILE_ATTRIBUTE_READONLY))return 2;
    for(bool ansi:{false,true})for(bool forbid:{false,true}){
        std::printf("BEGIN readonly A=%u NOREADONLY=%u\n",ansi,forbid);
        std::atomic<bool> done=false;
        std::thread driver([&]{for(unsigned i=0;i<80&&!done;++i){Sleep(100);if(i<8)continue;
            EnumWindows([](HWND w,LPARAM tick)->BOOL{
                bool owned=false;for(HWND owner=w;owner;owner=GetWindow(owner,GW_OWNER)){DWORD pid=0;GetWindowThreadProcessId(owner,&pid);if(pid==GetCurrentProcessId()){owned=true;break;}}
                if(!owned)return TRUE;
                static thread_local std::vector<HWND> seen;
                if(std::find(seen.begin(),seen.end(),w)==seen.end()){
                    if(!GetDlgItem(w,IDOK))DumpOwnedMessage(w);
                    seen.push_back(w);wchar_t title[512]{},kind[128]{};GetWindowTextW(w,title,512);GetClassNameW(w,kind,128);char utf8[2048]{};WideCharToMultiByte(CP_UTF8,0,title,-1,utf8,2048,nullptr,nullptr);std::printf("OWNED_WINDOW enabled=%u title=%s\n",IsWindowEnabled(w),utf8);
                    EnumChildWindows(w,[](HWND child,LPARAM)->BOOL{wchar_t text[1024]{};GetWindowTextW(child,text,1024);if(*text){char utf8[4096]{};WideCharToMultiByte(CP_UTF8,0,text,-1,utf8,4096,nullptr,nullptr);std::printf("CONTROL id=%d text=%s\n",GetDlgCtrlID(child),utf8);}return TRUE;},0);
                }
                wchar_t cls[32]{};GetClassNameW(w,cls,32);
                if(std::wstring(cls)!=L"#32770"||!IsWindowVisible(w)||!IsWindowEnabled(w))return TRUE;
                EnumChildWindows(w,[](HWND child,LPARAM)->BOOL{wchar_t text[1024]{};GetWindowTextW(child,text,1024);const std::wstring value(text);static thread_local bool noted=false;if(!noted&&(value.find(L"\u53ea\u8bfb")!=value.npos||value.find(L"read-only")!=value.npos)){char utf8[4096]{};WideCharToMultiByte(CP_UTF8,0,text,-1,utf8,4096,nullptr,nullptr);std::printf("READONLY_WARNING %s\n",utf8);noted=true;}return TRUE;},0);
                if(tick>25){wchar_t caption[256]{};GetWindowTextW(w,caption,256);const std::wstring title(caption);const bool confirm=title==L"\u786e\u8ba4\u53e6\u5b58\u4e3a"||title==L"Confirm Save As";PostMessageW(w,WM_COMMAND,IDCANCEL,0);PostMessageW(w,WM_USER+102,confirm?IDNO:IDOK,0);}
                else {
                    HWND yes=GetDlgItem(w,IDYES);wchar_t yes_class[32]{};if(yes)GetClassNameW(yes,yes_class,32);
                    wchar_t caption[256]{};GetWindowTextW(w,caption,256);const std::wstring title(caption);
                    const bool confirm=(yes&&IsWindowVisible(yes)&&std::wstring(yes_class)==L"Button")||title==L"\u786e\u8ba4\u53e6\u5b58\u4e3a"||title==L"Confirm Save As";
                    PostMessageW(w,WM_COMMAND,confirm?IDYES:IDOK,0);
                    if(confirm)PostMessageW(w,WM_USER+102,IDYES,0); // TDM_CLICK_BUTTON for DirectUI task dialog.
                }
                return TRUE;
            },i);
        }});
        DWORD flags=OFN_EXPLORER|OFN_HIDEREADONLY|OFN_NOCHANGEDIR|OFN_DONTADDTORECENT|OFN_PATHMUSTEXIST|OFN_OVERWRITEPROMPT|(forbid?OFN_NOREADONLYRETURN:0);
        BOOL accepted=FALSE;
        if(ansi){char name[1024]="export.html";auto directory=root.string();OPENFILENAMEA o{};o.lStructSize=sizeof(o);o.lpstrFile=name;o.nMaxFile=1024;o.lpstrInitialDir=directory.c_str();o.lpstrFilter="All\0*.*\0\0";o.lpstrDefExt="html";o.Flags=flags;accepted=GetSaveFileNameA(&o);}
        else{wchar_t name[1024]=L"export.html";OPENFILENAMEW o{};o.lStructSize=sizeof(o);o.lpstrFile=name;o.nMaxFile=1024;o.lpstrInitialDir=root.c_str();o.lpstrFilter=L"All\0*.*\0\0";o.lpstrDefExt=L"html";o.Flags=flags;accepted=GetSaveFileNameW(&o);}
        const DWORD error=CommDlgExtendedError();done=true;driver.join();
        std::printf("END readonly A=%u NOREADONLY=%u accepted=%u error=%lu\n",ansi,forbid,accepted,error);
    }
    bool clean=SetFileAttributesW(target.c_str(),FILE_ATTRIBUTE_NORMAL)&&DeleteFileW(target.c_str())&&RemoveDirectoryW(root.c_str());
    std::printf("readonly cleanup=%u\n",clean);return clean?0:1;
}
int DiagnosticTest() {
    using namespace pulse::shell_bridge;
    unsigned failures=0;
    const auto check=[&](bool ok,const char* label){std::printf("[%s] %s\n",ok?"PASS":"FAIL",label);if(!ok)++failures;};
    wchar_t file[8]{}; OPENFILENAMEW input{};input.lStructSize=sizeof(input);input.lpstrFile=file;input.nMaxFile=8;
    const DWORD baseline=OFN_EXPLORER|OFN_HIDEREADONLY|OFN_NOCHANGEDIR;
    for(unsigned variant=0;variant<5;++variant){
        input.Flags=baseline;input.FlagsEx=0;input.lpfnHook=nullptr;input.lCustData=0;
        const char* expected=nullptr;
        if(variant==0){input.Flags&=~OFN_EXPLORER;expected="legacy-decline-missing-explorer";}
        if(variant==1){input.Flags|=OFN_ENABLEHOOK;expected="legacy-decline-unsupported-flags";}
        if(variant==2){input.FlagsEx=1;expected="legacy-decline-flags-ex";}
        if(variant==3){input.lCustData=1;expected="legacy-decline-custom-data";}
        if(variant==4)expected="legacy-decline-factory";
        unsigned factory_calls=0,diagnostics=0;LegacyComDiagnostic observed;
        LegacyDialogFactory factory=[&](bool,IFileDialog** out){++factory_calls;*out=nullptr;return E_ACCESSDENIED;};
        BOOL result=TRUE;
        const bool consumed=RunLegacyComDialog(&input,false,factory,result,[&](const LegacyComDiagnostic& event){observed=event;++diagnostics;SetLastError(ERROR_BUSY);});
        check(!consumed&&diagnostics==1&&std::string(observed.reason)==expected,"specific decline reason emitted exactly once");
        check(observed.flags==input.Flags&&observed.flags_ex==input.FlagsEx&&observed.structure_size==sizeof(input)&&observed.max_file==8&&
            observed.presence==(variant==3?2049u:1u),"numeric input fields contain only expected sizes flags and presence");
        check(factory_calls==(variant==4?1u:0u),"preflight bypass never activates COM factory");
        if(variant==4)check(observed.error==E_ACCESSDENIED,"failed factory HRESULT preserved");
    }
    input.Flags=baseline&~OFN_EXPLORER;BOOL result=FALSE;SetLastError(ERROR_FILE_NOT_FOUND);
    check(!RunLegacyComDialog(&input,false,{},result,[](const LegacyComDiagnostic&){SetLastError(ERROR_BUSY);throw 1;})&&GetLastError()==ERROR_FILE_NOT_FOUND,"diagnostic throw and LastError mutation cannot alter bypass");
    input.Flags=baseline&~OFN_NOCHANGEDIR; bool save_gate=false;
    check(!RunLegacyComDialog(&input,true,{},result,[&](const LegacyComDiagnostic& event){save_gate=std::string(event.reason)=="legacy-decline-missing-nochangedir";})&&save_gate,
        "Save without NOCHANGEDIR retains its unverified native gate");
    std::printf("failures=%u\n",failures);return failures?1:0;
}
int main(int argc, char** argv) {
    if(argc==2&&std::string(argv[1])=="--diagnostic")return DiagnosticTest();
    if(argc==2&&std::string(argv[1])=="--readonly")return ReadOnlyOracle();
    if(argc==2&&std::string(argv[1])=="--writable")return ReadOnlyOracle(false);
    const bool adapter = argc == 2 && std::string(argv[1]) == "--adapter";
    const pulse::shell_bridge::LegacyDialogFactory factory = [](bool save, IFileDialog** output) {
        IUnknown* native = nullptr;
        HRESULT hr = pulse::shell_bridge::CreateSystemLegacyDialog(save, &native);
        if (SUCCEEDED(hr)) { hr = native->QueryInterface(IID_PPV_ARGS(output)); native->Release(); }
        return hr;
    };
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto root=std::filesystem::absolute(std::filesystem::path(L"bench_data")/(L"legacy-ext-"+std::to_wstring(GetCurrentProcessId())));
    if(!std::filesystem::create_directory(root))return 2;
    unsigned failures=0;
    for(bool ansi:{false,true})for(bool filter:{false,true})for(bool explicit_name:{false,true}){
        const DWORD thread=GetCurrentThreadId();std::atomic<bool> done=false;
        std::thread driver([&]{for(unsigned i=0;i<120&&!done;++i){Sleep(100);if(i<5)continue;EnumThreadWindows(thread,[](HWND w,LPARAM cancel)->BOOL{wchar_t cls[32]{};GetClassNameW(w,cls,32);if(std::wstring(cls)==L"#32770"&&IsWindowVisible(w))PostMessageW(w,WM_COMMAND,cancel?IDCANCEL:IDOK,0);return TRUE;},i>100?1:0);}});
        BOOL ok=FALSE;DWORD flags=OFN_EXPLORER|OFN_HIDEREADONLY|OFN_NOCHANGEDIR|OFN_DONTADDTORECENT|OFN_PATHMUSTEXIST|OFN_OVERWRITEPROMPT;
        if(ansi){char name[1024]{};strcpy_s(name,explicit_name?"export.html":"export");auto dir=root.string();OPENFILENAMEA o{};o.lStructSize=sizeof(o);o.lpstrFile=name;o.nMaxFile=1024;o.lpstrInitialDir=dir.c_str();o.lpstrDefExt="html";o.lpstrFilter=filter?"HTML\0*.html\0\0":"All\0*.*\0\0";o.Flags=flags;if (adapter) { if (!pulse::shell_bridge::RunLegacyComDialog(&o,true,factory,ok)) ++failures; } else ok=GetSaveFileNameA(&o);
            if (std::filesystem::path(name) != root/L"export.html" || (o.Flags & OFN_EXTENSIONDIFFERENT)) ++failures;std::printf("A filter=%u explicit=%u ok=%u error=%lu flags=%lx path=%s\n",filter,explicit_name,ok,CommDlgExtendedError(),o.Flags,name);}
        else{wchar_t name[1024]{};wcscpy_s(name,explicit_name?L"export.html":L"export");OPENFILENAMEW o{};o.lStructSize=sizeof(o);o.lpstrFile=name;o.nMaxFile=1024;o.lpstrInitialDir=root.c_str();o.lpstrDefExt=L"html";o.lpstrFilter=filter?L"HTML\0*.html\0\0":L"All\0*.*\0\0";o.Flags=flags;if (adapter) { if (!pulse::shell_bridge::RunLegacyComDialog(&o,true,factory,ok)) ++failures; } else ok=GetSaveFileNameW(&o);
            if (std::filesystem::path(name) != root/L"export.html" || (o.Flags & OFN_EXTENSIONDIFFERENT)) ++failures;std::printf("W filter=%u explicit=%u ok=%u error=%lu flags=%lx path=%ls\n",filter,explicit_name,ok,CommDlgExtendedError(),o.Flags,name);}
        done=true;driver.join();if(!ok)++failures;
    }
    if(!RemoveDirectoryW(root.c_str()))++failures;
    std::printf("adapter=%u failures=%u\n",adapter,failures);
    return failures?1:0;
}

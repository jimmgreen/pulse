#include "../shell_bridge/legacy_com_provider.h"
#include "../shell_bridge/legacy_dialog_error.h"
#include "../shell_bridge/legacy_hook_host.h"
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
#include <array>
#include <cstring>
#include <type_traits>


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
    for(unsigned variant=0;variant<10;++variant){
        input.lStructSize=sizeof(input);input.Flags=baseline;input.FlagsEx=0;input.lpfnHook=nullptr;input.lCustData=0;
        const char* expected=nullptr;
        if(variant==0){input.Flags&=~OFN_EXPLORER;expected="legacy-decline-factory";}
        if(variant==1){input.Flags|=OFN_ENABLEHOOK;expected="legacy-decline-hook";}
        if(variant==2){input.FlagsEx=2;expected="legacy-decline-flags-ex";}
        if(variant==3){input.lCustData=1;expected="legacy-decline-factory";}
        if(variant==4)expected="legacy-decline-factory";
        if(variant==5){input.Flags|=OFN_LONGNAMES;expected="legacy-decline-factory";}
        if(variant==6){input.Flags|=OFN_NOLONGNAMES;expected="legacy-decline-factory";}
        if(variant==7){input.Flags|=OFN_NOTESTFILECREATE;expected="legacy-decline-factory";}
        if(variant==8){input.Flags=(baseline&~OFN_EXPLORER)|OFN_ALLOWMULTISELECT;expected="legacy-decline-old-style-contract";}
        if(variant==9){input.lStructSize=OPENFILENAME_SIZE_VERSION_400W;input.Flags|=OFN_ENABLEHOOK;expected="legacy-decline-v4-customization";}
        unsigned factory_calls=0,diagnostics=0;LegacyComDiagnostic observed;
        LegacyDialogFactory factory=[&](bool,IFileDialog** out){++factory_calls;*out=nullptr;return E_ACCESSDENIED;};
        BOOL result=TRUE;
        const bool consumed=RunLegacyComDialog(&input,false,factory,result,[&](const LegacyComDiagnostic& event){observed=event;++diagnostics;SetLastError(ERROR_BUSY);});
        check(!consumed&&diagnostics==1&&std::string(observed.reason)==expected,"specific decline reason emitted exactly once");
        check(observed.flags==input.Flags&&observed.flags_ex==input.FlagsEx&&observed.structure_size==input.lStructSize&&observed.max_file==8&&
            observed.presence==(variant==3?2049u:1u),"numeric input fields contain only expected sizes flags and presence");
        const bool reaches_factory = variant == 0 || (variant >= 3 && variant <= 7);
        check(factory_calls==(reaches_factory?1u:0u),"supported flags reach factory and unsupported preflight never activates it");
        if(reaches_factory)check(observed.error==E_ACCESSDENIED,"failed factory HRESULT preserved");
    }
    input.lStructSize=sizeof(input);input.Flags=(baseline&~OFN_EXPLORER)|OFN_ALLOWMULTISELECT;BOOL result=FALSE;SetLastError(ERROR_FILE_NOT_FOUND);
    check(!RunLegacyComDialog(&input,false,{},result,[](const LegacyComDiagnostic&){SetLastError(ERROR_BUSY);throw 1;})&&GetLastError()==ERROR_FILE_NOT_FOUND,"diagnostic throw and LastError mutation cannot alter bypass");
    input.Flags=baseline&~OFN_NOCHANGEDIR; bool save_factory=false;
    check(!RunLegacyComDialog(&input,true,[&](bool save,IFileDialog** out){save_factory=save;*out=nullptr;return E_ACCESSDENIED;},result)&&save_factory,
        "standard Save without NOCHANGEDIR reaches the directory-preserving adapter");
    input.Flags|=OFN_ENABLEHOOK;
    input.lpfnHook=[](HWND,UINT,WPARAM,LPARAM)->UINT_PTR{return 0;};
    bool hooked_save_factory=false;
    check(!RunLegacyComDialog(&input,true,[&](bool save,IFileDialog** out){hooked_save_factory=save;*out=nullptr;return E_ACCESSDENIED;},result,{},true)&&hooked_save_factory,
        "hooked Save without NOCHANGEDIR reaches the directory-preserving adapter");
    input.Flags=baseline&~OFN_HIDEREADONLY; input.lpfnHook=nullptr;
    unsigned open_ro_factory=0;
    check(!RunLegacyComDialog(&input,false,[&](bool,IFileDialog** out){++open_ro_factory;*out=nullptr;return E_ACCESSDENIED;},result)&&open_ro_factory==1,
        "uncustomized Open without HIDEREADONLY reaches factory for read-only checkbox control");
    input.Flags=(baseline&~OFN_HIDEREADONLY)|OFN_READONLY;
    open_ro_factory=0;
    check(!RunLegacyComDialog(&input,false,[&](bool,IFileDialog** out){++open_ro_factory;*out=nullptr;return E_ACCESSDENIED;},result)&&open_ro_factory==1,
        "uncustomized Open with OFN_READONLY initial state reaches factory");
    input.Flags=(baseline&~OFN_HIDEREADONLY)|OFN_ENABLEHOOK;
    input.lpfnHook=[](HWND,UINT,WPARAM,LPARAM)->UINT_PTR{return 0;};
    open_ro_factory=0;
    check(!RunLegacyComDialog(&input,false,[&](bool,IFileDialog** out){++open_ro_factory;*out=nullptr;return E_ACCESSDENIED;},result,{},true)&&open_ro_factory==1,
        "hook-only Open without HIDEREADONLY reaches factory for read-only checkbox control");
    input.Flags=(baseline&~OFN_HIDEREADONLY)|OFN_ENABLETEMPLATE;
    bool visible_open_gate=false;
    check(!RunLegacyComDialog(&input,false,{},result,[&](const LegacyComDiagnostic& event){visible_open_gate=std::string(event.reason)=="legacy-decline-missing-hidereadonly";},true)&&visible_open_gate,
        "template-customized Open without HIDEREADONLY stays native");
    input.lpfnHook=nullptr;
    wchar_t custom[64]=L"My filter\0*.mine\0";
    input.Flags=baseline; input.lpstrCustomFilter=custom; input.nMaxCustFilter=ARRAYSIZE(custom);
    bool filter_gate=false;
    check(!RunLegacyComDialog(&input,true,{},result,[&](const LegacyComDiagnostic& event){filter_gate=std::string(event.reason)=="legacy-decline-custom-filter";})&&filter_gate&&
        std::wstring(custom+10)==L"*.mine", "active custom-filter output buffer is left to the native dialog unchanged");
    // Batch 2 expanded legacy compatibility flags.
    for (DWORD flag : {DWORD{OFN_NOVALIDATE}, DWORD{OFN_CREATEPROMPT}, DWORD{OFN_NONETWORKBUTTON},
                       DWORD{OFN_SHAREAWARE}, DWORD{OFN_SHOWHELP}, DWORD{OFN_ENABLEINCLUDENOTIFY}}) {
        input.Flags = baseline | flag; input.FlagsEx = 0; input.lpstrCustomFilter = nullptr; input.nMaxCustFilter = 0;
        unsigned reached = 0;
        RunLegacyComDialog(&input, false, [&](bool, IFileDialog** out) { ++reached; *out = nullptr; return E_ACCESSDENIED; }, result);
        check(reached == 1, "expanded safe OFN flag reaches COM dialog factory");
    }
    // Remote hook-only support (32-bit thin companion & 64-bit user_auto without in-process UI DLL).
    {
        input.Flags = baseline | OFN_ENABLEHOOK;
        input.lpfnHook = [](HWND, UINT, WPARAM, LPARAM) -> UINT_PTR { return 0; };
        unsigned reached = 0;
        RunLegacyComDialog(&input, false, [&](bool, IFileDialog** out) { ++reached; *out = nullptr; return E_ACCESSDENIED; }, result, {}, false, nullptr, true);
        check(reached == 1, "hook-only OFN_ENABLEHOOK reaches factory when remote hook host verified");
        std::string tpl_reason;
        input.Flags = baseline | OFN_ENABLEHOOK | OFN_ENABLETEMPLATE;
        RunLegacyComDialog(&input, false, {}, result, [&](const LegacyComDiagnostic& e) { tpl_reason = e.reason; }, false, nullptr, true);
        check(tpl_reason == "legacy-decline-hook", "OFN_ENABLETEMPLATE remains declined on remote-only hook host");
        // Verify LegacyHookHost::Create bridges a cross-thread/remote parent HWND via a caller-thread proxy.
        std::atomic<HWND> foreign_parent{nullptr};
        HANDLE ready_evt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE quit_evt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::thread foreign_thread([&] {
            HWND w = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 10, 20, 320, 240, nullptr, nullptr, nullptr, nullptr);
            foreign_parent.store(w);
            SetEvent(ready_evt);
            WaitForSingleObject(quit_evt, 5000);
            if (w) DestroyWindow(w);
        });
        WaitForSingleObject(ready_evt, 5000);
        static bool saw_init = false;
        static bool saw_cdm = false;
        saw_init = false; saw_cdm = false;
        wchar_t hook_file[64]{};
        OPENFILENAMEW hook_ofn{};
        hook_ofn.lStructSize = sizeof(hook_ofn);
        hook_ofn.lpstrFile = hook_file;
        hook_ofn.nMaxFile = ARRAYSIZE(hook_file);
        hook_ofn.Flags = OFN_EXPLORER | OFN_ENABLEHOOK;
        hook_ofn.lpfnHook = [](HWND hdlg, UINT msg, WPARAM, LPARAM) -> UINT_PTR {
            if (msg == WM_INITDIALOG) saw_init = true;
            if (msg == WM_NOTIFY) {
                wchar_t buf[32]{};
                if (SendMessageW(GetParent(hdlg), CDM_GETSPEC, ARRAYSIZE(buf), reinterpret_cast<LPARAM>(buf)) == 9 &&
                    std::wstring(buf) == L"test.png") {
                    saw_cdm = true;
                }
            }
            return 0;
        };
        bool saw_close = false;
        {
            LegacyHookHost host(&hook_ofn);
            const HRESULT created = host.Create(foreign_parent.load(), [&](UINT msg, WPARAM cap, LPARAM buf) -> LRESULT {
                if (msg == CDM_GETSPEC) {
                    const wchar_t* spec = L"test.png";
                    if (buf && cap >= 9) wcscpy_s(reinterpret_cast<wchar_t*>(buf), cap, spec);
                    return 9;
                }
                if (msg == WM_CLOSE) { saw_close = true; return 0; }
                return -1;
            });
            check(created == S_OK && host.Window() != nullptr && saw_init,
                  "LegacyHookHost creates hidden caller-thread proxy for remote hook-only parent");
            if (created == S_OK) {
                host.Notify(CDN_INITDONE);
                SendMessageW(GetParent(host.Window()), WM_CLOSE, 0, 0);
                check(saw_cdm && saw_close, "remote proxy parent delivers CDM_* and WM_CLOSE to IFileDialog handler");
            }
        }
        SetEvent(quit_evt);
        foreign_thread.join();
        CloseHandle(ready_evt);
        CloseHandle(quit_evt);
        input.lpfnHook = nullptr;
    }
    {
        input.Flags = baseline; input.FlagsEx = OFN_EX_NOPLACESBAR;
        unsigned reached = 0;
        RunLegacyComDialog(&input, false, [&](bool, IFileDialog** out) { ++reached; *out = nullptr; return E_ACCESSDENIED; }, result);
        check(reached == 1, "OFN_EX_NOPLACESBAR reaches COM dialog factory");
        input.FlagsEx = 0;
    }
    {
        input.Flags = baseline | OFN_ENABLEHOOK | OFN_ALLOWMULTISELECT;
        input.lpfnHook = [](HWND, UINT, WPARAM, LPARAM) -> UINT_PTR { return 0; };
        unsigned reached = 0;
        RunLegacyComDialog(&input, false, [&](bool, IFileDialog** out) { ++reached; *out = nullptr; return E_ACCESSDENIED; }, result, {}, true);
        check(reached == 1, "OFN_ENABLEHOOK + OFN_ALLOWMULTISELECT on Open reaches factory when local hook host verified");
        std::string tpl_reason;
        input.Flags |= OFN_ENABLETEMPLATE;
        RunLegacyComDialog(&input, false, {}, result, [&](const LegacyComDiagnostic& e) { tpl_reason = e.reason; }, true);
        check(tpl_reason == "legacy-decline-customization-multiselect", "OFN_ENABLETEMPLATE + OFN_ALLOWMULTISELECT remains declined");
        input.lpfnHook = nullptr;
    }
    // LiZheng-style orphan trailing filter label and stale initial directory no longer abort before factory.
    {
        input.Flags = baseline; input.FlagsEx = 0; input.lpstrCustomFilter = nullptr; input.nMaxCustFilter = 0;
        input.lpstrFilter = L"Data (*.dat)\0*.dat\0OrphanLabel\0\0";
        input.nFilterIndex = 99;
        input.lpstrInitialDir = L"Z:\\NonExistentStaleFolder_PulseTest_999";
        unsigned reached = 0;
        RunLegacyComDialog(&input, false, [&](bool, IFileDialog** out) { ++reached; *out = nullptr; return E_ACCESSDENIED; }, result);
        check(reached == 1, "trailing orphan filter label reaches factory cleanly");
        input.lpstrFilter = L"OnlyOrphanLabel\0\0";
        reached = 0;
        RunLegacyComDialog(&input, true, [&](bool, IFileDialog** out) { ++reached; *out = nullptr; return E_ACCESSDENIED; }, result);
        check(reached == 1, "single orphan filter label reaches factory as unfiltered dialog");
        input.lpstrFilter = nullptr; input.nFilterIndex = 0; input.lpstrInitialDir = nullptr;
    }
    // LiZheng-style wildcard lpstrFile ("*.lsg") with orphan filter promotes the
    // mask into SetFileTypes and omits SetFileName so the exact contract succeeds.
    {
        struct CaptureDialog final : IFileDialog {
            ULONG refs = 1;
            std::vector<std::pair<std::wstring, std::wstring>> filters;
            std::wstring filename;
            bool filename_called = false;
            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
                if (!out) return E_POINTER;
                if (iid == IID_IUnknown || iid == IID_IModalWindow || iid == IID_IFileDialog) { *out = static_cast<IFileDialog*>(this); ++refs; return S_OK; }
                *out = nullptr; return E_NOINTERFACE;
            }
            ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
            ULONG STDMETHODCALLTYPE Release() override { if (--refs == 0) { delete this; return 0; } return refs; }
            HRESULT STDMETHODCALLTYPE Show(HWND) override { return HRESULT_FROM_WIN32(ERROR_CANCELLED); }
            HRESULT STDMETHODCALLTYPE SetFileTypes(UINT count, const COMDLG_FILTERSPEC* specs) override {
                for (UINT i = 0; i < count; ++i) filters.emplace_back(specs[i].pszName ? specs[i].pszName : L"", specs[i].pszSpec ? specs[i].pszSpec : L"");
                return S_OK;
            }
            HRESULT STDMETHODCALLTYPE SetFileTypeIndex(UINT) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE GetFileTypeIndex(UINT* out) override { if (out) *out = 1; return S_OK; }
            HRESULT STDMETHODCALLTYPE Advise(IFileDialogEvents*, DWORD* cookie) override { if (cookie) *cookie = 1; return S_OK; }
            HRESULT STDMETHODCALLTYPE Unadvise(DWORD) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE SetOptions(FILEOPENDIALOGOPTIONS) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE GetOptions(FILEOPENDIALOGOPTIONS* out) override { if (out) *out = 0; return S_OK; }
            HRESULT STDMETHODCALLTYPE SetDefaultFolder(IShellItem*) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE SetFolder(IShellItem*) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE GetFolder(IShellItem**) override { return E_NOTIMPL; }
            HRESULT STDMETHODCALLTYPE GetCurrentSelection(IShellItem**) override { return E_NOTIMPL; }
            HRESULT STDMETHODCALLTYPE SetFileName(LPCWSTR name) override { filename_called = true; filename = name ? name : L""; return S_OK; }
            HRESULT STDMETHODCALLTYPE GetFileName(LPWSTR*) override { return E_NOTIMPL; }
            HRESULT STDMETHODCALLTYPE SetTitle(LPCWSTR) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE SetOkButtonLabel(LPCWSTR) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE SetFileNameLabel(LPCWSTR) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE GetResult(IShellItem**) override { return E_NOTIMPL; }
            HRESULT STDMETHODCALLTYPE AddPlace(IShellItem*, FDAP) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE SetDefaultExtension(LPCWSTR) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE Close(HRESULT) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE SetClientGuid(REFGUID) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE ClearClientData() override { return S_OK; }
            HRESULT STDMETHODCALLTYPE SetFilter(IShellItemFilter*) override { return S_OK; }
        };
        for (bool save_case : {false, true}) {
            CaptureDialog* cap = nullptr;
            char name_a[256] = "*.lsg";
            char title_a[256]{};
            OPENFILENAMEA ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.lpstrFile = name_a;
            ofn.nMaxFile = sizeof(name_a);
            ofn.lpstrFileTitle = title_a;
            ofn.nMaxFileTitle = sizeof(title_a);
            ofn.lpstrFilter = "BeamFile(*.lsg)\0\0";
            ofn.Flags = save_case ? OFN_OVERWRITEPROMPT : OFN_HIDEREADONLY;
            BOOL ok = TRUE;
            const bool consumed = RunLegacyComDialog(&ofn, save_case, [&](bool, IFileDialog** out) {
                cap = new CaptureDialog();
                cap->AddRef();
                *out = cap;
                return S_OK;
            }, ok);
            check(consumed && !ok && cap && !cap->filename_called && cap->filters.size() == 1 &&
                cap->filters[0].first == L"BeamFile(*.lsg)" && cap->filters[0].second == L"*.lsg",
                save_case ? "LiZheng ANSI Save wildcard lpstrFile promotes filter and omits SetFileName"
                          : "LiZheng ANSI Open wildcard lpstrFile promotes filter and omits SetFileName");
            if (cap) cap->Release();
        }
        // Invalid non-wildcard filename ("bad:name*") must NOT be silently stripped.
        {
            CaptureDialog* cap = nullptr;
            wchar_t bad[64] = L"bad:name*";
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.lpstrFile = bad;
            ofn.nMaxFile = 64;
            ofn.Flags = baseline;
            BOOL ok = TRUE;
            const bool consumed = RunLegacyComDialog(&ofn, false, [&](bool, IFileDialog** out) {
                cap = new CaptureDialog();
                cap->AddRef();
                *out = cap;
                return S_OK;
            }, ok);
            check(consumed && cap && cap->filename_called && cap->filename == L"bad:name*",
                "invalid non-wildcard lpstrFile preserves SetFileName for contract enforcement");
            if (cap) cap->Release();
        }
    }
    std::printf("failures=%u\n",failures);return failures?1:0;
}
template<class O> void IgnoredComFieldsCases(bool save, unsigned& checks, unsigned& failures) {
    using namespace pulse::shell_bridge;
    using C = std::remove_pointer_t<decltype(O{}.lpstrFile)>;
    const auto check = [&](bool okay, const char* text) { ++checks; if (!okay) ++failures; std::printf("[%s] %s\n", okay ? "PASS" : "FAIL", text); };
    const auto set_fields = [](O& input, unsigned mask) {
        if (mask & 1) input.lpfnHook = reinterpret_cast<LPOFNHOOKPROC>(static_cast<UINT_PTR>(1));
        if (mask & 2) input.lpTemplateName = reinterpret_cast<decltype(input.lpTemplateName)>(static_cast<UINT_PTR>(1));
        if (mask & 4) input.hInstance = reinterpret_cast<HINSTANCE>(static_cast<UINT_PTR>(1));
        if (mask & 8) input.lCustData = static_cast<LPARAM>(0x1234567);
    };
    for (unsigned variant = 0; variant < 12; ++variant) {
        const bool active = variant >= 5;
        std::array<C, 16> file{}; std::array<C, 8> title{}; file[7] = static_cast<C>('!'); title[5] = static_cast<C>('?');
        O input{}; input.lStructSize = sizeof(input); input.lpstrFile = file.data(); input.nMaxFile = static_cast<DWORD>(file.size());
        input.lpstrFileTitle = title.data(); input.nMaxFileTitle = static_cast<DWORD>(title.size());
        input.Flags = OFN_EXPLORER | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
        constexpr unsigned masks[]{1,2,4,8,15}; const unsigned mask = active ? 15 : masks[variant]; set_fields(input, mask);
        if (active) {
            const unsigned kind = variant - 5;
            if (kind < 2) { input.Flags |= OFN_ENABLEHOOK; if (!kind) input.lpfnHook = nullptr; }
            else if (kind < 4) { input.Flags |= OFN_ENABLETEMPLATE; if (kind == 3) { input.hInstance = nullptr; input.lpTemplateName = nullptr; } }
            else if (kind < 6) { input.Flags |= OFN_ENABLETEMPLATEHANDLE; if (kind == 5) input.Flags |= OFN_ENABLETEMPLATE; }
            else input.Flags |= OFN_ENABLEHOOK | OFN_ENABLETEMPLATE;
        }
        const auto file_before = file; const auto title_before = title;
        std::array<unsigned char, sizeof(input)> before{}; std::memcpy(before.data(), &input, before.size());
        unsigned factories = 0, diagnostics = 0; DWORD diagnostic_input_error = 0; LegacyComDiagnostic observed;
        const LegacyDialogFactory factory = [&](bool actual_save, IFileDialog** output) { ++factories; *output = nullptr;
            check(actual_save == save, "Open/Save factory mode preserved"); SetLastError(ERROR_ACCESS_DENIED); return E_ACCESSDENIED; };
        BOOL result = TRUE; SetLastError(ERROR_FILE_NOT_FOUND);
        const bool consumed = RunLegacyComDialog(&input, save, factory, result, [&](const LegacyComDiagnostic& value) { diagnostic_input_error = GetLastError(); ++diagnostics; observed = value; SetLastError(ERROR_BUSY); });
        if (active) check(!consumed && factories == 0 && diagnostics == 1 && result == TRUE && GetLastError() == ERROR_FILE_NOT_FOUND,
            "active hook/template including null hook stays preflight native without factory or caller-state change");
        // CoUninitialize runs after decline and may change Win32 LastError.
        // The factory HRESULT and diagnostic-entry error are the seam outputs;
        // native CommDlgExtendedError preservation is tested by both adapters.
        else check(!consumed && factories == 1 && diagnostics == 1 && result == FALSE && observed.error == E_ACCESSDENIED && diagnostic_input_error == ERROR_ACCESS_DENIED,
            "ignored pointers reach injected factory without dereference and preserve factory failure");
        check(file == file_before && title == title_before && !std::memcmp(before.data(), &input, before.size()),
            "COM preflight preserves every caller field and buffer for A/W Open/Save");
    }
}
int IgnoredComFieldsTest() {
    unsigned checks = 0, failures = 0;
    for (const bool save : {false, true}) { IgnoredComFieldsCases<OPENFILENAMEW>(save, checks, failures); IgnoredComFieldsCases<OPENFILENAMEA>(save, checks, failures); }
    std::printf("IGNORED_FIELDS_COM checks=%u failures=%u native_ui=0 sentinel_dereferences=0\n", checks, failures);
    return failures ? 1 : 0;
}
int main(int argc, char** argv) {
    if(argc==2&&std::string(argv[1])=="--ignored-fields")return IgnoredComFieldsTest();
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

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
    // Photoshop Save for Web (flags=0x008888A4, OFN_ENABLEHOOK|OFN_ENABLETEMPLATEHANDLE,
    // lpTemplateName=nullptr): off-screen visible host populates 0x0BB9/0x0BBA combos,
    // mirrors them via IFileDialogCustomize, routes OnItemSelected -> CBN_SELCHANGE,
    // syncs cmb1 on OnTypeChange, and runs CDN_FILEOK on Save.
    {
        struct PsMockDialog final : IFileDialog, IFileDialogCustomize {
            ULONG refs = 1;
            struct MirroredItem { DWORD id; std::wstring text; };
            struct MirroredCtrl {
                DWORD id = 0;
                bool is_combo = false;
                std::wstring text;
                std::vector<MirroredItem> items;
                DWORD selected = UINT32_MAX;
                CDCONTROLSTATEF state = CDCS_ENABLEDVISIBLE;
            };
            std::vector<MirroredCtrl> controls;
            IFileDialogEvents* events = nullptr;
            IFileDialogControlEvents* ctrl_events = nullptr;
            UINT filter_index = 2;
            std::wstring saved_path;
            bool mirrored_before_show = false;
            bool slice_enabled_after_change = false;
            bool file_ok_accepted = false;

            MirroredCtrl* Find(DWORD id) {
                for (auto& c : controls) if (c.id == id) return &c;
                return nullptr;
            }
            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
                if (!out) return E_POINTER;
                if (iid == IID_IUnknown || iid == IID_IModalWindow || iid == IID_IFileDialog) {
                    *out = static_cast<IFileDialog*>(this); ++refs; return S_OK;
                }
                if (iid == IID_IFileDialogCustomize) {
                    *out = static_cast<IFileDialogCustomize*>(this); ++refs; return S_OK;
                }
                *out = nullptr; return E_NOINTERFACE;
            }
            ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
            ULONG STDMETHODCALLTYPE Release() override { if (--refs == 0) { delete this; return 0; } return refs; }
            HRESULT STDMETHODCALLTYPE Show(HWND) override {
                // Verify both "设置" (5 items, enabled) and "切片" (1 item, disabled) were mirrored before Show.
                const auto* settings_label = Find(0x50530100u);
                const auto* settings_combo = Find(0x50530200u);
                const auto* slices_label = Find(0x50530101u);
                const auto* slices_combo = Find(0x50530201u);
                mirrored_before_show = settings_label && settings_label->text == L"\u8BBE\u7F6E" &&
                    settings_combo && settings_combo->items.size() == 5 && settings_combo->selected == 0 &&
                    (settings_combo->state & CDCS_ENABLED) &&
                    slices_label && slices_label->text == L"\u5207\u7247" &&
                    slices_combo && slices_combo->items.size() == 1 && !(slices_combo->state & CDCS_ENABLED);
                if (ctrl_events) {
                    ctrl_events->OnItemSelected(static_cast<IFileDialogCustomize*>(this), 0x50530200u, 2u);
                    if (const auto* s = Find(0x50530201u)) slice_enabled_after_change = (s->state & CDCS_ENABLED) != 0;
                }
                filter_index = 1;
                if (events) {
                    events->OnTypeChange(static_cast<IFileDialog*>(this));
                    file_ok_accepted = (events->OnFileOk(static_cast<IFileDialog*>(this)) == S_OK);
                }
                return file_ok_accepted ? S_OK : HRESULT_FROM_WIN32(ERROR_CANCELLED);
            }
            HRESULT STDMETHODCALLTYPE SetFileTypes(UINT, const COMDLG_FILTERSPEC*) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE SetFileTypeIndex(UINT idx) override { filter_index = idx; return S_OK; }
            HRESULT STDMETHODCALLTYPE GetFileTypeIndex(UINT* out) override { if (out) *out = filter_index; return S_OK; }
            HRESULT STDMETHODCALLTYPE Advise(IFileDialogEvents* ev, DWORD* cookie) override {
                events = ev;
                if (events) {
                    events->AddRef();
                    (void)events->QueryInterface(IID_PPV_ARGS(&ctrl_events));
                }
                if (cookie) *cookie = 1;
                return S_OK;
            }
            HRESULT STDMETHODCALLTYPE Unadvise(DWORD) override {
                if (ctrl_events) { ctrl_events->Release(); ctrl_events = nullptr; }
                if (events) { events->Release(); events = nullptr; }
                return S_OK;
            }
            HRESULT STDMETHODCALLTYPE SetOptions(FILEOPENDIALOGOPTIONS) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE GetOptions(FILEOPENDIALOGOPTIONS* out) override { if (out) *out = 0; return S_OK; }
            HRESULT STDMETHODCALLTYPE SetDefaultFolder(IShellItem*) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE SetFolder(IShellItem*) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE GetFolder(IShellItem**) override { return E_NOTIMPL; }
            HRESULT STDMETHODCALLTYPE GetCurrentSelection(IShellItem**) override { return E_NOTIMPL; }
            HRESULT STDMETHODCALLTYPE SetFileName(LPCWSTR) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE GetFileName(LPWSTR*) override { return E_NOTIMPL; }
            HRESULT STDMETHODCALLTYPE SetTitle(LPCWSTR) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE SetOkButtonLabel(LPCWSTR) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE SetFileNameLabel(LPCWSTR) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE GetResult(IShellItem** out) override {
                return SHCreateItemFromParsingName(saved_path.c_str(), nullptr, IID_PPV_ARGS(out));
            }
            HRESULT STDMETHODCALLTYPE AddPlace(IShellItem*, FDAP) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE SetDefaultExtension(LPCWSTR) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE Close(HRESULT) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE SetClientGuid(REFGUID) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE ClearClientData() override { return S_OK; }
            HRESULT STDMETHODCALLTYPE SetFilter(IShellItemFilter*) override { return S_OK; }
            // IFileDialogCustomize
            HRESULT STDMETHODCALLTYPE EnableOpenDropDown(DWORD) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE AddMenu(DWORD, LPCWSTR) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE AddPushButton(DWORD, LPCWSTR) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE AddComboBox(DWORD id) override { controls.push_back({id, true, {}, {}, UINT32_MAX, CDCS_ENABLEDVISIBLE}); return S_OK; }
            HRESULT STDMETHODCALLTYPE AddRadioButtonList(DWORD) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE AddCheckButton(DWORD, LPCWSTR, BOOL) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE AddEditBox(DWORD, LPCWSTR) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE AddSeparator(DWORD) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE AddText(DWORD id, LPCWSTR text) override { controls.push_back({id, false, text ? text : L"", {}, UINT32_MAX, CDCS_ENABLEDVISIBLE}); return S_OK; }
            HRESULT STDMETHODCALLTYPE SetControlLabel(DWORD, LPCWSTR) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE GetControlState(DWORD id, CDCONTROLSTATEF* st) override {
                if (auto* c = Find(id)) { if (st) *st = c->state; return S_OK; }
                return E_INVALIDARG;
            }
            HRESULT STDMETHODCALLTYPE SetControlState(DWORD id, CDCONTROLSTATEF st) override {
                if (auto* c = Find(id)) { c->state = st; return S_OK; }
                return E_INVALIDARG;
            }
            HRESULT STDMETHODCALLTYPE GetEditBoxText(DWORD, WCHAR**) override { return E_NOTIMPL; }
            HRESULT STDMETHODCALLTYPE SetEditBoxText(DWORD, LPCWSTR) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE GetCheckButtonState(DWORD, BOOL*) override { return E_NOTIMPL; }
            HRESULT STDMETHODCALLTYPE SetCheckButtonState(DWORD, BOOL) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE AddControlItem(DWORD id, DWORD item_id, LPCWSTR text) override {
                if (auto* c = Find(id)) { c->items.push_back({item_id, text ? text : L""}); return S_OK; }
                return E_INVALIDARG;
            }
            HRESULT STDMETHODCALLTYPE RemoveControlItem(DWORD, DWORD) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE RemoveAllControlItems(DWORD) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE GetControlItemState(DWORD, DWORD, CDCONTROLSTATEF*) override { return E_NOTIMPL; }
            HRESULT STDMETHODCALLTYPE SetControlItemState(DWORD, DWORD, CDCONTROLSTATEF) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE GetSelectedControlItem(DWORD id, DWORD* out) override {
                if (auto* c = Find(id)) { if (out) *out = c->selected; return S_OK; }
                return E_INVALIDARG;
            }
            HRESULT STDMETHODCALLTYPE SetSelectedControlItem(DWORD id, DWORD item_id) override {
                if (auto* c = Find(id)) { c->selected = item_id; return S_OK; }
                return E_INVALIDARG;
            }
            HRESULT STDMETHODCALLTYPE StartVisualGroup(DWORD, LPCWSTR) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE EndVisualGroup() override { return S_OK; }
            HRESULT STDMETHODCALLTYPE MakeProminent(DWORD) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE SetControlItemText(DWORD, DWORD, LPCWSTR) override { return S_OK; }
        };

        static bool ps_saw_visible_init = false;
        static int ps_selected_preset = -1;
        static int ps_cmb1_at_fileok = -1;
        ps_saw_visible_init = false;
        ps_selected_preset = -1;
        ps_cmb1_at_fileok = -1;

        wchar_t temp_dir[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp_dir);
        const std::filesystem::path target_file = std::filesystem::path(temp_dir) / L"ps_save_for_web_test.jpg";
        {
            HANDLE f = CreateFileW(target_file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        }

        wchar_t ps_file[4096] = L"banner.jpg";
        wchar_t ps_title[1024]{};
        OPENFILENAMEW ps_ofn{};
        ps_ofn.lStructSize = sizeof(ps_ofn);
        ps_ofn.lpstrFile = ps_file;
        ps_ofn.nMaxFile = ARRAYSIZE(ps_file);
        ps_ofn.lpstrFileTitle = ps_title;
        ps_ofn.nMaxFileTitle = ARRAYSIZE(ps_title);
        ps_ofn.lpstrFilter = L"HTML\0*.html\0Images Only\0*.jpg\0HTML Only\0*.html\0\0";
        ps_ofn.nFilterIndex = 2;
        ps_ofn.lpstrDefExt = L"jpg";
        ps_ofn.Flags = 0x008888A4u;  // Exact Photoshop Save for Web flags (OFN_ENABLEHOOK|OFN_ENABLETEMPLATEHANDLE)
        ps_ofn.lpfnHook = [](HWND hdlg, UINT msg, WPARAM wp, LPARAM lp) -> UINT_PTR {
            if (msg == WM_INITDIALOG) {
                CreateWindowExW(0, L"Static", L"\u8BBE\u7F6E\uFF1A", WS_CHILD | WS_VISIBLE, 8, 8, 60, 20, hdlg, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(0x0BBB)), nullptr, nullptr);
                CreateWindowExW(0, L"ComboBox", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 72, 8, 160, 120, hdlg, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(0x0BB9)), nullptr, nullptr);
                CreateWindowExW(0, L"Static", L"\u5207\u7247\uFF1A", WS_CHILD | WS_VISIBLE, 240, 8, 60, 20, hdlg, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(0x0BBC)), nullptr, nullptr);
                HWND slices = CreateWindowExW(0, L"ComboBox", L"", WS_CHILD | WS_VISIBLE | WS_DISABLED | CBS_DROPDOWNLIST, 304, 8, 160, 120, hdlg, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(0x0BBA)), nullptr, nullptr);
                (void)slices;
                return 0;  // PS hook returns 0; host must still keep dialog alive!
            }
            if (msg == WM_NOTIFY) {
                const auto* hdr = reinterpret_cast<const NMHDR*>(lp);
                if (hdr && static_cast<int>(hdr->code) == static_cast<int>(CDN_INITDONE)) {
                    ps_saw_visible_init = IsWindowVisible(hdlg) != FALSE;
                    if (ps_saw_visible_init) {
                        HWND settings = GetDlgItem(hdlg, 0x0BB9);
                        const wchar_t* presets[] = {L"\u9ED8\u8BA4\u8BBE\u7F6E", L"JPEG \u9AD8", L"JPEG \u4E2D", L"PNG-24", L"GIF"};
                        for (const wchar_t* p : presets) SendMessageW(settings, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(p));
                        SendMessageW(settings, CB_SETCURSEL, 0, 0);
                        HWND slices = GetDlgItem(hdlg, 0x0BBA);
                        SendMessageW(slices, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"\u6240\u6709\u5207\u7247"));
                        SendMessageW(slices, CB_SETCURSEL, 0, 0);
                    }
                } else if (hdr && static_cast<int>(hdr->code) == static_cast<int>(CDN_FILEOK)) {
                    HWND cmb = GetDlgItem(GetParent(hdlg), 0x0470);  // cmb1
                    ps_cmb1_at_fileok = cmb ? static_cast<int>(SendMessageW(cmb, CB_GETCURSEL, 0, 0)) : -1;
                }
            }
            if (msg == WM_COMMAND && LOWORD(wp) == 0x0BB9 && HIWORD(wp) == CBN_SELCHANGE) {
                ps_selected_preset = static_cast<int>(SendMessageW(reinterpret_cast<HWND>(lp), CB_GETCURSEL, 0, 0));
                if (ps_selected_preset == 2) EnableWindow(GetDlgItem(hdlg, 0x0BBA), TRUE);
            }
            return 0;
        };

        SetMirroredLegacyHookTestOverride(true);
        PsMockDialog* mock = nullptr;
        BOOL ps_ok = FALSE;
        const bool ps_consumed = RunLegacyComDialog(&ps_ofn, true, [&](bool, IFileDialog** out) {
            mock = new PsMockDialog();
            mock->saved_path = target_file.wstring();
            mock->AddRef();
            *out = static_cast<IFileDialog*>(mock);
            return S_OK;
        }, ps_ok, {}, false, nullptr, true);  // remote-only hook host!
        SetMirroredLegacyHookTestOverride(false);

        check(ps_consumed && ps_ok == TRUE && mock != nullptr, "Photoshop Save-for-Web 0x008888A4 taken over on remote hook host");
        check(ps_saw_visible_init, "Photoshop hook saw visible off-screen sub-dialog on CDN_INITDONE");
        check(mock && mock->mirrored_before_show, "Photoshop Settings/Slices combos mirrored into IFileDialogCustomize before Show");
        check(ps_selected_preset == 2 && mock && mock->slice_enabled_after_change, "mirrored OnItemSelected drives CBN_SELCHANGE and syncs sibling state");
        check(ps_cmb1_at_fileok == 0 && ps_ofn.nFilterIndex == 1, "OnTypeChange syncs parent cmb1 and output nFilterIndex");
        check(std::wstring(ps_file) == target_file.wstring(), "Photoshop Save-for-Web writes final path to lpstrFile");
        if (mock) mock->Release();
        DeleteFileW(target_file.c_str());
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

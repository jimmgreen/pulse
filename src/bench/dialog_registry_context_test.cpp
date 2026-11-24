#include "../common/json_value.h"
#include "../setup/setup_desktop_launch.h"
#include "../common/direct_reveal_bootstrap_diagnostic.h"
#include <windows.h>
#include <objbase.h>
#include <appmodel.h>
#include <sddl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
namespace {
namespace json=pulse::json;
json::Value Read(HKEY root,const std::wstring& key,REGSAM view) {
    auto result=json::Value::Object();HKEY handle=nullptr;
    const LSTATUS opened=RegOpenKeyExW(root,key.c_str(),0,KEY_READ|view,&handle);
    result.set("open_error",static_cast<int>(opened));
    if(opened)return result;
    for(const auto* name:{L"DllPath",L""}) {
        DWORD type=0,size=0;LSTATUS error=RegQueryValueExW(handle,name,nullptr,&type,nullptr,&size);
        if(!error && type==REG_SZ && size>=2 && size<=65536) {
            std::vector<wchar_t> value(size/2+1);error=RegQueryValueExW(handle,name,nullptr,&type,reinterpret_cast<BYTE*>(value.data()),&size);
            if(!error)result.set(*name?"dll_path":"default",json::Utf8(value.data()));
        }
    }
    DWORD enabled=0,size=sizeof(enabled);const auto error=RegGetValueW(handle,nullptr,L"Enabled",RRF_RT_REG_DWORD,nullptr,&enabled,&size);
    result.set("enabled_error",static_cast<int>(error));if(!error)result.set("enabled",static_cast<int>(enabled));
    RegCloseKey(handle);return result;
}
std::wstring Self(){std::wstring p(32768,0);const auto n=GetModuleFileNameW(nullptr,p.data(),static_cast<DWORD>(p.size()));p.resize(n);return p;}
int ProbeActivation(const std::filesystem::path& output) {
    const HRESULT initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(FAILED(initialized))return 3;
    auto report=json::Value::Object();
    report.set("pid",static_cast<int>(GetCurrentProcessId()));
    HANDLE process_token=nullptr;
    if(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&process_token)) {
        alignas(TOKEN_MANDATORY_LABEL) BYTE label[1024]{};DWORD returned=0;
        if(GetTokenInformation(process_token,TokenIntegrityLevel,label,sizeof(label),&returned)) {
            const auto sid=reinterpret_cast<TOKEN_MANDATORY_LABEL*>(label)->Label.Sid;
            const auto count=*GetSidSubAuthorityCount(sid);
            if(count)report.set("integrity",static_cast<uint64_t>(*GetSidSubAuthority(sid,count-1)));
        }
        CloseHandle(process_token);
    }
    PWSTR local=nullptr;
    const HRESULT folder=SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&local);
    report.set("local_appdata_hr",static_cast<uint64_t>(static_cast<uint32_t>(folder)));
    if(local){report.set("local_appdata",json::Utf8(local));CoTaskMemFree(local);}
    for(bool save:{false,true}) {
        IUnknown* object=nullptr;
        const HRESULT hr=CoCreateInstance(save?CLSID_FileSaveDialog:CLSID_FileOpenDialog,
            nullptr,CLSCTX_INPROC_SERVER,save?IID_IFileSaveDialog:IID_IFileOpenDialog,reinterpret_cast<void**>(&object));
        auto row=json::Value::Object();row.set("hr",static_cast<uint64_t>(static_cast<uint32_t>(hr)));
        if(object) {
            const auto address=(*reinterpret_cast<void***>(object))[0];
            HMODULE image=nullptr;
            if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(address),&image)) {
                std::wstring path(32768,0);path.resize(GetModuleFileNameW(image,path.data(),static_cast<DWORD>(path.size())));
                row.set("implementation",json::Utf8(path));
            }
            row.set("pulse_module_loaded",GetModuleHandleW(L"PulseDialogAuto64.dll")!=nullptr);
            if(const HMODULE bridge=GetModuleHandleW(L"PulseDialogAuto64.dll")) {
                std::wstring path(32768,0);path.resize(GetModuleFileNameW(bridge,path.data(),static_cast<DWORD>(path.size())));
                row.set("pulse_module_path",json::Utf8(path));
                using Diagnose=DWORD(WINAPI*)(void*);
                if(const auto diagnose=reinterpret_cast<Diagnose>(GetProcAddress(bridge,"PulseInitializeDirectReveal"))) {
                    pulse::DirectRevealBootstrapDiagnostic evidence;
                    row.set("bootstrap_error",static_cast<uint64_t>(diagnose(&evidence)));
                    row.set("bootstrap_stage",static_cast<uint64_t>(evidence.stage));
                    row.set("token_reason",static_cast<uint64_t>(evidence.token_reason));
                    row.set("token_error",static_cast<uint64_t>(evidence.token_error));
                    row.set("log_error",static_cast<uint64_t>(evidence.log_error));
                    row.set("folder_hr",static_cast<uint64_t>(static_cast<uint32_t>(evidence.folder_status)));
                }
                using Entry=HRESULT(WINAPI*)(REFCLSID,REFIID,void**);
                const auto entry=reinterpret_cast<Entry>(GetProcAddress(bridge,"DllGetClassObject"));
                IClassFactory* factory=nullptr;
                const HRESULT direct=entry?entry(save?CLSID_FileSaveDialog:CLSID_FileOpenDialog,
                    IID_IClassFactory,reinterpret_cast<void**>(&factory)):E_NOINTERFACE;
                row.set("direct_factory_hr",static_cast<uint64_t>(static_cast<uint32_t>(direct)));
                if(factory) {
                    IUnknown* instance=nullptr;
                    const HRESULT created=factory->CreateInstance(nullptr,save?IID_IFileSaveDialog:IID_IFileOpenDialog,
                        reinterpret_cast<void**>(&instance));
                    row.set("direct_instance_hr",static_cast<uint64_t>(static_cast<uint32_t>(created)));
                    if(instance) {
                        HMODULE direct_image=nullptr;
                        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>((*reinterpret_cast<void***>(instance))[0]),&direct_image);
                        std::wstring direct_path(32768,0);
                        direct_path.resize(GetModuleFileNameW(direct_image,direct_path.data(),static_cast<DWORD>(direct_path.size())));
                        row.set("direct_implementation",json::Utf8(direct_path));instance->Release();
                    }
                    factory->Release();
                }
            }
            object->Release();
        }
        report.set(save?"save":"open",std::move(row));
    }
    // Let the loaded bridge's asynchronous diagnostics reach disk before exit.
    Sleep(1000);
    CoUninitialize();
    std::ofstream file(output,std::ios::binary);file<<json::Serialize(report)<<'\n';return file?0:4;
}
int Probe(const std::filesystem::path& output) {
    auto report=json::Value::Object();report.set("pid",static_cast<int>(GetCurrentProcessId()));report.set("exe",json::Utf8(Self()));
    UINT32 count=0;LONG error=GetCurrentPackageFullName(&count,nullptr);report.set("package_error",static_cast<int>(error));
    if(error==ERROR_INSUFFICIENT_BUFFER){std::wstring name(count,0);error=GetCurrentPackageFullName(&count,name.data());if(!error)report.set("package",json::Utf8(name.c_str()));}
    HANDLE token=nullptr;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return 3;
    DWORD size=0;GetTokenInformation(token,TokenUser,nullptr,0,&size);std::vector<BYTE> bytes(size);
    if(!GetTokenInformation(token,TokenUser,bytes.data(),size,&size)){CloseHandle(token);return 3;}
    LPWSTR raw=nullptr;if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid,&raw)){CloseHandle(token);return 3;}
    const std::wstring sid(raw);LocalFree(raw);report.set("sid",json::Utf8(sid));
    for(const auto pair:{std::pair{TokenVirtualizationAllowed,"virtualization_allowed"},std::pair{TokenVirtualizationEnabled,"virtualization_enabled"},std::pair{TokenIsAppContainer,"app_container"}}){DWORD value=0;size=sizeof(value);if(GetTokenInformation(token,pair.first,&value,size,&size))report.set(pair.second,static_cast<int>(value));}
    CloseHandle(token);
    auto views=json::Value::Object();
    for(bool x86:{false,true}) {
        const auto view=x86?KEY_WOW64_32KEY:KEY_WOW64_64KEY;
        const std::wstring config=x86?L"Software\\Pulse\\DialogAuto32":L"Software\\Pulse\\DialogAuto";
        const std::wstring clsid=L"Software\\Classes\\CLSID\\{DC1C5A9C-E88A-4DDE-A5A1-60F82A20AEF7}\\InprocServer32";
        auto row=json::Value::Object();row.set("hkcu_config",Read(HKEY_CURRENT_USER,config,view));row.set("hku_config",Read(HKEY_USERS,sid+L"\\"+config,view));
        row.set("hkcu_open",Read(HKEY_CURRENT_USER,clsid,view));row.set("hku_open",Read(HKEY_USERS,sid+L"\\"+clsid,view));views.set(x86?"x86":"x64",std::move(row));
    }
    report.set("views",std::move(views));std::ofstream f(output,std::ios::binary|std::ios::trunc);f<<json::Serialize(report)<<'\n';return f?0:4;
}
}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,LPWSTR,int) {
    int argc=0;wchar_t** argv=CommandLineToArgvW(GetCommandLineW(),&argc);if(!argv)return 2;
    int result=2;
    if(argc==3 && std::wstring_view(argv[1])==L"--output")result=Probe(argv[2]);
    if(argc==3 && std::wstring_view(argv[1])==L"--activation-output")result=ProbeActivation(argv[2]);
    if(argc==3 && std::wstring_view(argv[1])==L"--desktop") {
        const HRESULT apartment=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        const HRESULT launch=SUCCEEDED(apartment)?pulse::setup::LaunchFromDesktop(Self(),L"--output \""+std::wstring(argv[2])+L"\"",std::filesystem::path(Self()).parent_path().wstring()):apartment;
        auto status=json::Value::Object();
        status.set("apartment_hr",static_cast<uint64_t>(static_cast<uint32_t>(apartment)));
        status.set("launch_hr",static_cast<uint64_t>(static_cast<uint32_t>(launch)));
        std::ofstream report(std::filesystem::path(std::wstring(argv[2])+L".launch.json"),std::ios::binary);
        report<<json::Serialize(status)<<'\n';
        result=FAILED(launch)?5:0;if(SUCCEEDED(apartment))CoUninitialize();
    }
    if(argc==5 && std::wstring_view(argv[1])==L"--desktop-python") {
        const std::filesystem::path exe(argv[2]),script(argv[3]),plan(argv[4]);
        const auto directory=std::filesystem::path(Self()).parent_path();
        if(exe.is_absolute() && exe.filename()==L"pythonw.exe" && script.is_absolute() && plan.is_absolute() &&
            script.parent_path()==directory && plan.parent_path()==directory) {
            const auto quote=[](const std::wstring& value){return L"\""+value+L"\"";};
            const HRESULT launched=pulse::setup::LaunchFromDesktop(exe.wstring(),quote(script.wstring())+L" --plan "+quote(plan.wstring()),directory.wstring());
            result=SUCCEEDED(launched)?0:6;
        }
    }
    LocalFree(argv);return result;
}


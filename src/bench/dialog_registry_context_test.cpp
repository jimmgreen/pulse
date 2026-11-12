#include "../common/json_value.h"
#include "../setup/setup_desktop_launch.h"
#include <windows.h>
#include <objbase.h>
#include <appmodel.h>
#include <sddl.h>
#include <shellapi.h>
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
    if(argc==3 && std::wstring_view(argv[1])==L"--desktop") {
        const HRESULT apartment=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        const HRESULT launch=SUCCEEDED(apartment)?pulse::setup::LaunchFromDesktop(Self(),L"--output \""+std::wstring(argv[2])+L"\"",std::filesystem::path(Self()).parent_path().wstring()):apartment;
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


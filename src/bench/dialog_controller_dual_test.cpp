#include "../app/dialog_management_controller.h"
#include "../common/dialog_auto_registration.h"
#include "../common/json_value.h"
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
namespace {std::wstring install_root,state_root;}
namespace pulse::app {std::wstring GetPulseDataDir(){return state_root;}}
namespace pulse::app_runtime_paths {
std::wstring InstallRoot(){return install_root;}
std::wstring RuntimeDirectory(){return install_root;}
std::wstring StableEntryPath(){return install_root+L"\\pulse.exe";}
}
int wmain(int argc,wchar_t** argv) {
    using namespace pulse::app;
    if(argc!=4)return 2;
    const std::wstring operation(argv[1]);
    if(operation!=L"--refresh" && operation!=L"--enable" && operation!=L"--disable")return 2;
    install_root=argv[2];state_root=argv[3];
    if(!std::filesystem::path(install_root).is_absolute() || !std::filesystem::path(state_root).is_absolute())return 2;
    DialogManagementController controller(nullptr,0,state_root);
    const auto generation=operation==L"--enable"?controller.EnableAuto():operation==L"--disable"?controller.DisableAuto():controller.Refresh();
    std::optional<DialogManagementResult> result;
    const auto deadline=GetTickCount64()+60000;
    while(GetTickCount64()<deadline&&!result) {
        for(auto& entry:controller.Drain())if(entry.generation==generation)result=std::move(entry);
        if(!result)Sleep(10);
    }
    if(!result){std::cout<<"CONTROLLER timeout\n";return 3;}
    auto json=pulse::json::Value::Object();
    json.set("operation",pulse::json::Utf8(operation));json.set("error",static_cast<int>(result->error));
    json.set("rollback_error",static_cast<int>(result->auto_rollback_error));
    json.set("bundle_verified",result->bundle_verified);json.set("bundle_error",static_cast<int>(result->bundle_error));
    json.set("enabled",result->auto_enabled);json.set("conflict",result->auto_conflict);json.set("path_mismatch",result->auto_path_mismatch);
    auto arches=pulse::json::Value::Array();
    bool both=true;
    for(const auto& arch:result->auto_arch) {
        auto item=pulse::json::Value::Object();item.set("architecture",arch.x86?"x86":"x64");
        item.set("component",static_cast<int>(arch.component));item.set("registration",static_cast<int>(arch.registration));
        item.set("registry_error",static_cast<int>(arch.registry_error));item.set("dll_path",pulse::json::Utf8(arch.dll_path));arches.push(std::move(item));
        if(operation==L"--enable")both=both && arch.registration==pulse::dialog_auto::ArchRegistrationState::Registered && arch.component==pulse::dialog_auto::ArchComponentState::Present;
        if(operation==L"--disable")both=both && (arch.registration==pulse::dialog_auto::ArchRegistrationState::Unregistered || arch.registration==pulse::dialog_auto::ArchRegistrationState::DisabledMatching);
    }
    json.set("architectures",std::move(arches));json.set("requested_state",both);
    std::cout<<pulse::json::Serialize(json)<<'\n';
    return result->error||result->auto_rollback_error||!both?1:0;
}

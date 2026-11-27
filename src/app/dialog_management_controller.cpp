#include "../common/app_runtime_paths.h"
#include "../common/dialog_admin_trust.h"
#include <shellapi.h>
#include "dialog_management_controller.h"
#include <algorithm>
#include "../common/dialog_auto_ownership.h"
#include "../common/dialog_auto_registration.h"
#include "../common/dialog_auto_transaction.h"
#include "../common/dialog_bundle.h"
#include "../common/dialog_bridge_paths.h"
#include "../common/dialog_session.h"
#include "../common/dialog_process_diagnostics.h"
#include "../common/dialog_log_excerpt.h"
#include "../common/diagnostics_report.h"
#include "../common/dialog_admin_auto_task.h"
#include "../common/runtime_log.h"
#include "../common/current_user_security.h"
#include "../common/json_value.h"
#include <shlobj.h>
#include "../dialog_launcher/managed_launch_profile.h"
#include "../dialog_launcher/target_preflight.h"
#include <objbase.h>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string_view>
#include <thread>
#include <array>
namespace pulse::app {
std::wstring GetPulseDataDir();
bool DialogAutoPathMismatch(bool enabled, const std::wstring& registered, const std::wstring& bundle_root) {
    if (!enabled || bundle_root.empty()) return false;
    const auto base = std::filesystem::path(bundle_root);
    const auto auto_dll = (base / dialog_auto::DllName).lexically_normal();
    const auto stub_dll = (base / dialog_auto::StubDllName).lexically_normal();
    const auto expected = (GetFileAttributesW(auto_dll.c_str()) == INVALID_FILE_ATTRIBUTES &&
                           GetFileAttributesW(stub_dll.c_str()) != INVALID_FILE_ATTRIBUTES)
        ? stub_dll.wstring() : auto_dll.wstring();
    const auto actual = std::filesystem::path(registered).lexically_normal().wstring();
    return CompareStringOrdinal(expected.c_str(), -1, actual.c_str(), -1, TRUE) != CSTR_EQUAL;
}
namespace {
namespace fs = std::filesystem;
using Profile = dialog_launcher::ManagedLaunchProfile;
struct Handle { HANDLE h=nullptr; ~Handle(){if(h && h!=INVALID_HANDLE_VALUE) CloseHandle(h);} };
std::wstring Plain(std::wstring path) {
    if(path.size()>6 && path.starts_with(LR"(\\?\)") && path[5]==L':' && path[6]==L'\\') path.erase(0,4);
    return path;
}
std::wstring Unique() {
    GUID id{}; wchar_t text[40]{};
    return SUCCEEDED(CoCreateGuid(&id)) && StringFromGUID2(id,text,40)?text:L"";
}
bool IsGuid(const fs::path& path) {
    const auto name=path.filename().wstring(); GUID id{};
    return name.size()==38 && SUCCEEDED(CLSIDFromString(name.c_str(),&id));
}
DWORD NoReparse(const fs::path& path) {
    if(!path.is_absolute()) return ERROR_BAD_PATHNAME;
    for(auto p=path; !p.empty(); p=p.parent_path()) {
        const DWORD attributes=GetFileAttributesW(p.c_str());
        if(attributes==INVALID_FILE_ATTRIBUTES) return GetLastError();
        if(attributes&FILE_ATTRIBUTE_REPARSE_POINT) return ERROR_REPARSE_TAG_INVALID;
        if(p==p.parent_path()) break;
    }
    return ERROR_SUCCESS;
}
std::wstring Quote(const std::wstring& value) {
    std::wstring result=L"\""; size_t slashes=0;
    for(wchar_t c:value) {
        if(c==L'\\'){++slashes;continue;}
        result.append(c==L'"'?slashes*2+1:slashes,L'\\'); result+=c;slashes=0;
    }
    result.append(slashes*2,L'\\');result+=L'"';return result;
}
DWORD OrdinaryLaunchContext() {
    HANDLE raw=nullptr;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&raw)) return GetLastError();
    Handle token{raw};TOKEN_ELEVATION elevation{};DWORD returned=0;
    if(!GetTokenInformation(token.h,TokenElevation,&elevation,sizeof(elevation),&returned)) return GetLastError();
    return elevation.TokenIsElevated ? ERROR_ACCESS_DENIED : ERROR_SUCCESS;
}
DWORD ValidateApplication(std::wstring& path,WORD* detected=nullptr) {
    auto canonical=dialog_session::Canonical(path,false);WORD machine=0;
    if(canonical.empty() || !dialog_session::Machine(canonical,machine)) return ERROR_BAD_EXE_FORMAT;
    if(machine!=IMAGE_FILE_MACHINE_AMD64 && machine!=IMAGE_FILE_MACHINE_I386) return ERROR_EXE_MACHINE_TYPE_MISMATCH;
    if(detected) *detected=machine;
    path=Plain(std::move(canonical));return ERROR_SUCCESS;
}
DWORD ReadConfiguration(const fs::path& file,DialogManagementConfiguration& config) {
    const DWORD attributes=GetFileAttributesW(file.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES) return GetLastError()==ERROR_FILE_NOT_FOUND?ERROR_SUCCESS:GetLastError();
    if(const DWORD e=NoReparse(file)) return e;
    std::ifstream stream(file,std::ios::binary|std::ios::ate);
    if(!stream) return ERROR_READ_FAULT;
    if(stream.tellg()>static_cast<std::streamoff>(256*1024)) return ERROR_INVALID_DATA;
    stream.seekg(0);uint32_t magic=0;stream.read(reinterpret_cast<char*>(&magic),sizeof(magic));
    if(magic!=0x314D4450) return ERROR_INVALID_DATA;
    std::array<std::wstring,3> fields;
    for(auto& field:fields) {
        uint32_t count=0;if(!stream.read(reinterpret_cast<char*>(&count),sizeof(count)) || count>32768) return ERROR_INVALID_DATA;
        field.resize(count);
        if(count && !stream.read(reinterpret_cast<char*>(field.data()),static_cast<std::streamsize>(count*sizeof(wchar_t)))) return ERROR_INVALID_DATA;
        if(field.find(wchar_t{})!=std::wstring::npos) return ERROR_INVALID_DATA;
    }
    if(stream.peek()!=EOF || fields[2].size()>128 || fields[2].find_first_not_of(L"0123456789abcdefABCDEF")!=std::wstring::npos) return ERROR_INVALID_DATA;
    config.bundle_root=std::move(fields[0]);config.application=std::move(fields[1]);
    config.manifest_digest.clear();for(wchar_t c:fields[2]) config.manifest_digest.push_back(static_cast<char>(c));return ERROR_SUCCESS;
}
DWORD SaveConfiguration(const fs::path& root,const DialogManagementConfiguration& config) {
    const auto nonce=Unique();if(nonce.empty()) return ERROR_GEN_FAILURE;
    const auto temporary=root/(nonce+L".tmp"),destination=root/L"configuration.bin";
    if(GetFileAttributesW(destination.c_str())!=INVALID_FILE_ATTRIBUTES) if(const DWORD e=NoReparse(destination)) return e;
    std::vector<unsigned char> bytes;
    auto append=[&](const void* p,size_t n){const auto* b=static_cast<const unsigned char*>(p);bytes.insert(bytes.end(),b,b+n);};
    const uint32_t magic=0x314D4450;append(&magic,sizeof(magic));
    for(const auto& field:std::array<std::wstring,3>{config.bundle_root,config.application,std::wstring(config.manifest_digest.begin(),config.manifest_digest.end())}) {
        if(field.size()>32768) return ERROR_INVALID_DATA;
        const auto count=static_cast<uint32_t>(field.size());append(&count,sizeof(count));append(field.data(),field.size()*sizeof(wchar_t));
    }
    DWORD error=0;
    {
        Handle file{CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr)};
        if(file.h==INVALID_HANDLE_VALUE) return GetLastError();
        DWORD written=0;
        if(!WriteFile(file.h,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr) || written!=bytes.size() || !FlushFileBuffers(file.h)) error=ERROR_WRITE_FAULT;
    }
    if(!error && !MoveFileExW(temporary.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) error=GetLastError();
    if(error) DeleteFileW(temporary.c_str());
    return error;
}
struct CommonBridgeInfo {
    std::wstring root;
    std::wstring stub64;
    std::wstring stub32;
    std::wstring core_dir_x64;
    std::wstring core_dir_x86;
    std::string digest_x64;
};
std::wstring NormalizeDirKey(std::wstring path) {
    path = Plain(std::move(path));
    std::replace(path.begin(), path.end(), L'/', L'\\');
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();
    return path;
}
bool FileAbsent(const fs::path& path) {
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
    const DWORD err = GetLastError();
    return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND;
}
bool ResolveCommonBridgeForInstall(const fs::path& install_root, CommonBridgeInfo& out) {
    out = {};
    if (install_root.empty() || !FileAbsent(install_root / L"dialog-current.json")) return false;
    const auto common_root = dialog_bridge_paths::CommonDialogBridgeRoot();
    if (common_root.empty()) return false;
    dialog_bridge_paths::ActiveBridgePointer ptr;
    if (dialog_bridge_paths::ReadActiveBridgePointer(common_root, ptr) != ERROR_SUCCESS) return false;
    if (ptr.core_dir_x64.empty() || ptr.install_root.empty()) return false;
    const auto lhs = NormalizeDirKey(ptr.install_root);
    const auto rhs = NormalizeDirKey(install_root.wstring());
    if (CompareStringOrdinal(lhs.c_str(), -1, rhs.c_str(), -1, TRUE) != CSTR_EQUAL) return false;
    const auto stub64 = common_root + L"\\" + dialog_bridge_paths::kStubDllName64;
    const auto core64 = ptr.core_dir_x64 + L"\\" + dialog_bridge_paths::kCoreDllName64;
    if (NoReparse(stub64) != ERROR_SUCCESS || NoReparse(core64) != ERROR_SUCCESS) return false;
    out.root = common_root;
    out.stub64 = stub64;
    out.core_dir_x64 = ptr.core_dir_x64;
    out.digest_x64 = ptr.core_digest_x64;
    if (!ptr.core_dir_x86.empty()) {
        const auto stub32 = common_root + L"\\" + dialog_bridge_paths::kStubDllName32;
        const auto core32 = ptr.core_dir_x86 + L"\\" + dialog_bridge_paths::kCoreDllName32;
        if (NoReparse(stub32) == ERROR_SUCCESS && NoReparse(core32) == ERROR_SUCCESS) {
            out.stub32 = stub32;
            out.core_dir_x86 = ptr.core_dir_x86;
        }
    }
    return true;
}
void QueryDialogBrokerRunning(bool& running, DWORD& error) noexcept {
    running = false;
    error = ERROR_SUCCESS;
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) { error = GetLastError(); return; }
    SC_HANDLE svc = OpenServiceW(scm, L"PulseDialogBroker", SERVICE_QUERY_STATUS);
    if (!svc) {
        error = GetLastError();
        CloseServiceHandle(scm);
        return;
    }
    SERVICE_STATUS_PROCESS status{};
    DWORD needed = 0;
    if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status), sizeof(status), &needed)) {
        error = GetLastError();
    } else {
        running = status.dwCurrentState == SERVICE_RUNNING;
        if (!running) error = ERROR_NOT_READY;
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
}
}
struct DialogManagementController::State {
    struct Command {DialogManagementOperation operation;uint64_t generation;std::wstring path;std::string digest;dialog_diagnostics::ReproductionWindow reproduction;};
    std::mutex mutex;std::condition_variable cv;
    HWND host=nullptr;UINT wake=0;bool stopping=false;uint64_t generation=0;
    std::deque<Command> commands;std::vector<DialogManagementResult> results;
    std::wstring requested_root;fs::path root;bool initialized=false;
    fs::path install_root;
    bool installed_default=true;
    std::array<std::wstring,2> component_directories;
    DialogManagementConfiguration configuration;
    dialog_history::History history;
    bool history_loaded=false;
    std::unordered_map<uint64_t,DialogRecordApp> record_apps; // found programs, kept for the session
    std::unordered_map<uint64_t,ULONGLONG> record_app_misses; // not found: retried after 5 minutes or on a manual check
    DialogManagementController::RecordAppResolver resolve_record_apps; // set before the worker starts
    DWORD history_load_error=0;
    dialog_restart::Inventory restart_inventory;
    ULONGLONG restart_checked_at=0;
    std::vector<dialog_adaptive::PausedTarget> paused;
    bool Current(uint64_t value){std::lock_guard lock(mutex);return !stopping && generation==value;}
    DWORD Initialize() {
        if(initialized) return ERROR_SUCCESS;
        const auto requested=requested_root.empty()?GetPulseDataDir()+L"\\DialogManagement":requested_root;
        fs::path path(requested);if(!path.is_absolute()) return ERROR_BAD_PATHNAME;
        auto ancestor=path;while(!fs::exists(ancestor) && ancestor!=ancestor.parent_path()) ancestor=ancestor.parent_path();
        if(const DWORD error=NoReparse(ancestor)) return error;
        std::error_code ec;fs::create_directories(path,ec);if(ec) return static_cast<DWORD>(ec.value());
        if(const DWORD error=NoReparse(path)) return error;
        root=Plain(dialog_session::Canonical(path,true));if(root.empty()) return ERROR_BAD_PATHNAME;
        CurrentUserSecurityAttributes security;if(!security) return ERROR_ACCESS_DENIED;
        if(!CreateDirectoryW((root/L"operations").c_str(),security.get()) && GetLastError()!=ERROR_ALREADY_EXISTS) return GetLastError();
        if(const DWORD error=NoReparse(root/L"operations")) return error;
        {const DWORD error=ReadConfiguration(root/L"configuration.bin",configuration);if(error) return error;}
        // Production remembers only the selected application, never an old
        // experimental package or digest that could pin an installed update.
        if(requested_root.empty()) {configuration.bundle_root.clear();configuration.manifest_digest.clear();}
        install_root=app_runtime_paths::InstallRoot();
        if(install_root.empty()) return ERROR_BAD_PATHNAME;
        installed_default=configuration.bundle_root.empty() ||
            _wcsicmp(configuration.bundle_root.c_str(),install_root.c_str())==0 ||
            _wcsicmp(configuration.bundle_root.c_str(),(install_root/L"DialogBridge").c_str())==0;
        if(installed_default) {
            configuration.bundle_root.clear();
            configuration.manifest_digest.clear();
        }
        initialized=true;return ERROR_SUCCESS;
    }
    void Verify(DialogManagementResult& result,dialog_bundle::BundleLease& bundle) {
        CommonBridgeInfo common_bridge;
        const bool use_common_bridge = installed_default && ResolveCommonBridgeForInstall(install_root, common_bridge);
        if (use_common_bridge) {
            result.bundle_error = ERROR_SUCCESS;
            result.bundle_verified = true;
            result.configuration = configuration;
            result.configuration.bundle_root = common_bridge.root;
            result.configuration.manifest_digest = common_bridge.digest_x64;
            result.auto_path_mismatch = result.auto_enabled &&
                _wcsicmp(common_bridge.stub64.c_str(), result.auto_dll.c_str()) != 0;
            component_directories = {common_bridge.core_dir_x64, common_bridge.core_dir_x86};
            if (!common_bridge.stub32.empty()) {
                const auto status = dialog_auto::Inspect(dialog_auto::Architecture::X86);
                result.auto_enabled = result.auto_enabled && status.enabled;
                result.auto_conflict = result.auto_conflict || status.conflict;
                if (!result.auto_error) result.auto_error = status.error;
                result.auto_path_mismatch = result.auto_path_mismatch ||
                    (status.enabled && _wcsicmp(common_bridge.stub32.c_str(), status.dll_path.c_str()) != 0);
            }
            auto application = configuration.application;
            WORD machine = 0;
            result.application_valid = !application.empty() && ValidateApplication(application, &machine) == ERROR_SUCCESS;
            result.application_machine = result.application_valid ? machine : 0;
            return;
        }
        result.bundle_error=installed_default ? dialog_bundle::ResolveInstalledBundle(install_root.wstring(),bundle) :
            dialog_bundle::VerifyBundle(configuration.bundle_root,configuration.manifest_digest,bundle);
        result.bundle_verified=result.bundle_error==ERROR_SUCCESS;
        result.configuration=configuration;
        if(installed_default) {
            result.configuration.bundle_root=bundle.Directory();
            result.configuration.manifest_digest=bundle.ManifestDigest();
        }
        result.auto_path_mismatch=(installed_default && result.auto_enabled && !result.bundle_verified) ||
            DialogAutoPathMismatch(result.auto_enabled,result.auto_dll,result.configuration.bundle_root);
        component_directories={};
        if(bundle) component_directories[bundle.Machine()==IMAGE_FILE_MACHINE_I386?1:0]=bundle.Directory();
        if(installed_default && bundle && bundle.Machine()==IMAGE_FILE_MACHINE_AMD64) {
            const auto pointer=install_root/L"dialog-x86-current.json";
            const DWORD attributes=GetFileAttributesW(pointer.c_str());
            const DWORD pointer_error=attributes==INVALID_FILE_ATTRIBUTES?GetLastError():ERROR_SUCCESS;
            if(attributes!=INVALID_FILE_ATTRIBUTES || (pointer_error!=ERROR_FILE_NOT_FOUND && pointer_error!=ERROR_PATH_NOT_FOUND)) {
                dialog_bundle::BundleLease companion;
                const DWORD error=dialog_bundle::ResolveInstalledBundleForMachine(install_root.wstring(),IMAGE_FILE_MACHINE_I386,companion);
                if(error) {result.bundle_verified=false;result.bundle_error=error;}
                else {
                    component_directories[1]=companion.Directory();
                    const auto status=dialog_auto::Inspect(dialog_auto::Architecture::X86);
                    result.auto_enabled=result.auto_enabled && status.enabled;
                    result.auto_conflict=result.auto_conflict || status.conflict;
                    if(!result.auto_error) result.auto_error=status.error;
                    const auto expected=companion.Directory()+L"\\"+dialog_auto::DllNameFor(dialog_auto::Architecture::X86);
                    result.auto_path_mismatch=result.auto_path_mismatch || (status.enabled && _wcsicmp(expected.c_str(),status.dll_path.c_str())!=0);
                }
            }
        }
        auto application=configuration.application;
        WORD machine=0;
        result.application_valid=!application.empty() && ValidateApplication(application,&machine)==ERROR_SUCCESS;
        result.application_machine=result.application_valid ? machine : 0;
    }
    void VerifyLaunch(DialogManagementResult& result,dialog_bundle::BundleLease& launch) {
        if(configuration.application.empty()) {result.launch_bundle_error=ERROR_SUCCESS;result.launch_bundle_verified=false;return;}
        if(!result.application_valid) {result.launch_bundle_error=ERROR_BAD_EXE_FORMAT;return;}
        result.launch_bundle_error=installed_default ?
            dialog_bundle::ResolveInstalledBundleForMachine(install_root.wstring(),static_cast<WORD>(result.application_machine),launch) :
            dialog_bundle::VerifyBundle(configuration.bundle_root,configuration.manifest_digest,launch);
        if(!result.launch_bundle_error && launch.Machine()!=result.application_machine)
            result.launch_bundle_error=ERROR_EXE_MACHINE_TYPE_MISMATCH;
        result.launch_bundle_verified=result.launch_bundle_error==ERROR_SUCCESS;
    }
    void VerifyAdmin(DialogManagementResult& result,const dialog_bundle::BundleLease& launch) {
        if(!installed_default) result.admin_launch_error=ERROR_NOT_SUPPORTED;
        else if(configuration.application.empty()) result.admin_launch_error=ERROR_SUCCESS;
        else if(!result.application_valid || !result.launch_bundle_verified)
            result.admin_launch_error=result.launch_bundle_error ? result.launch_bundle_error : ERROR_BAD_EXE_FORMAT;
        else if(!dialog_session::AllowedProcess(GetCurrentProcess(),dialog_admin::IsElevated())) result.admin_launch_error=ERROR_ACCESS_DENIED;
        else result.admin_launch_error=dialog_admin::VerifyAdminLaunchLocation(Plain(install_root.wstring()),
            Plain(launch.Directory()),Plain(launch.Host()),static_cast<WORD>(launch.Machine()));
        result.admin_launch_ready=!configuration.application.empty() && result.application_valid &&
            result.launch_bundle_verified && result.admin_launch_error==ERROR_SUCCESS;
    }
    DWORD StartAdmin(const Command& command,dialog_bundle::BundleLease& bundle,bool& started) {
        if(!Current(command.generation)) return ERROR_OPERATION_ABORTED;
        auto application=configuration.application;WORD machine=0;
        if(const DWORD error=ValidateApplication(application,&machine)) return error;
        if(machine!=bundle.Machine()) return ERROR_EXE_MACHINE_TYPE_MISMATCH;
        if(const DWORD error=dialog_bundle::RecheckDirectory(bundle)) return error;
        if(const DWORD error=dialog_admin::VerifyAdminLaunchLocation(Plain(install_root.wstring()),
            Plain(bundle.Directory()),Plain(bundle.Host()),machine)) return error;
        const auto parameters=L"--admin-installed "+Quote(Plain(install_root.wstring()))+L" -- "+Quote(application);
        if(parameters.size()>=32767) return ERROR_BUFFER_OVERFLOW;
        const HRESULT com_result=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        if(FAILED(com_result)) return HRESULT_FACILITY(com_result)==FACILITY_WIN32 ? HRESULT_CODE(com_result) : ERROR_GEN_FAILURE;
        struct ComScope {~ComScope(){CoUninitialize();}} com;
        if(!Current(command.generation)) return ERROR_OPERATION_ABORTED;
        SHELLEXECUTEINFOW execute{};execute.cbSize=sizeof(execute);
        execute.fMask=SEE_MASK_NOCLOSEPROCESS|SEE_MASK_NOASYNC|SEE_MASK_FLAG_NO_UI;
        execute.lpVerb=L"runas";execute.lpFile=bundle.Launcher().c_str();
        execute.lpParameters=parameters.c_str();execute.lpDirectory=bundle.Directory().c_str();execute.nShow=SW_HIDE;
        // Windows owns the consent UI. Never synthesize consent or terminate
        // the launcher/application when the UI cancels a generation or closes.
        if(!ShellExecuteExW(&execute)) {const DWORD error=GetLastError();return error ? error : ERROR_GEN_FAILURE;}
        started=true;
        Handle process{execute.hProcess};
        if(!process.h) return ERROR_INVALID_HANDLE;
        const auto deadline=GetTickCount64()+15000;
        for(;;) {
            const DWORD wait=WaitForSingleObject(process.h,100);
            if(wait==WAIT_OBJECT_0) break;
            if(wait!=WAIT_TIMEOUT) return GetLastError();
            if(!Current(command.generation)) return ERROR_OPERATION_ABORTED;
            if(GetTickCount64()>=deadline) return ERROR_TIMEOUT;
        }
        DWORD code=0;if(!GetExitCodeProcess(process.h,&code)) return GetLastError();
        return code==0 ? ERROR_SUCCESS : code==3 ? ERROR_TIMEOUT : ERROR_GEN_FAILURE;
    }
    DWORD ChangeAuto(bool enable,dialog_bundle::BundleLease& primary,DialogManagementResult& result) {
        using dialog_auto::Architecture;
        std::vector<dialog_auto::RegistrationTarget> targets;
        dialog_bundle::BundleLease companion;
        CommonBridgeInfo common_bridge;
        const bool use_common_bridge = installed_default && ResolveCommonBridgeForInstall(install_root, common_bridge);
        const auto base=installed_default?install_root.wstring():configuration.bundle_root;
        if(enable) {
            std::vector<std::pair<Architecture,std::wstring>> desired;
            if (use_common_bridge) {
                desired.emplace_back(Architecture::X64, common_bridge.stub64);
                if (!common_bridge.stub32.empty()) {
                    desired.emplace_back(Architecture::X86, common_bridge.stub32);
                }
            } else {
                DWORD error=result.bundle_verified?dialog_bundle::RecheckDirectory(primary):result.bundle_error;
                if(error) return error;
                if(_wcsicmp(primary.Directory().c_str(),install_root.c_str())==0) return ERROR_NOT_SUPPORTED;
                const auto arch=primary.Machine()==IMAGE_FILE_MACHINE_I386?Architecture::X86:Architecture::X64;
                desired.emplace_back(arch,primary.Directory()+L"\\"+dialog_auto::DllNameFor(arch));
                if(installed_default && !component_directories[1].empty() && arch==Architecture::X64) {
                    error=dialog_bundle::ResolveInstalledBundleForMachine(install_root.wstring(),IMAGE_FILE_MACHINE_I386,companion);
                    if(error) return error;
                    if((error=dialog_bundle::RecheckDirectory(companion))) return error;
                    desired.emplace_back(Architecture::X86,companion.Directory()+L"\\"+dialog_auto::DllNameFor(Architecture::X86));
                }
            }
            if (installed_default) {
                std::vector<Architecture> architectures;
                // Reject a live, unrelated installation before repairing another
                // architecture's journal. Conflicting snapshots are checked by
                // the stricter missing-journal witness below, never bypassed.
                for (const auto& [architecture, target_dll] : desired) {
                    const auto status = dialog_auto::Inspect(architecture);
                    if (status.error) return status.error;
                    if (!status.conflict && !status.dll_path.empty() &&
                        _wcsicmp(status.dll_path.c_str(), target_dll.c_str()) != 0 &&
                        !dialog_auto::IsOwnedDialogAutoDll(base, installed_default, architecture, status.dll_path) &&
                        !dialog_auto::IsOrphanedMissingPulseDialogDll(architecture, status) &&
                        !dialog_auto::IsRetiredDisabledPulseDialogDll(architecture, status)) return ERROR_ALREADY_EXISTS;
                    architectures.push_back(architecture);
                }
                const auto repaired = dialog_auto::RepairMissingRegistrationJournals(architectures);
                if (repaired.error) {
                    result.auto_rollback_error = repaired.rollback_error;
                    return repaired.error;
                }
            }
            std::vector<dialog_auto::RegistrationTarget> stale_enabled;
            std::vector<std::pair<Architecture,std::wstring>> reclaimed_orphans;
            std::vector<std::pair<Architecture,std::wstring>> retired_disabled;
            auto is_reclaimed=[&](Architecture target_arch,const std::wstring& path) {
                return std::any_of(reclaimed_orphans.begin(),reclaimed_orphans.end(),[&](const auto& item){
                    return item.first==target_arch && _wcsicmp(item.second.c_str(),path.c_str())==0;
                });
            };
            for(const auto& [desired_arch,target_dll]:desired) {
                const auto status=dialog_auto::Inspect(desired_arch);
                if(status.error) return status.error;
                if(status.conflict) return ERROR_ALREADY_EXISTS;
                if(!status.dll_path.empty() && _wcsicmp(status.dll_path.c_str(),target_dll.c_str())!=0) {
                    const bool owned=dialog_auto::IsOwnedDialogAutoDll(base,installed_default,desired_arch,status.dll_path);
                    const bool orphaned=!owned && installed_default && dialog_auto::IsOrphanedMissingPulseDialogDll(desired_arch,status);
                    const bool retired=!owned && !orphaned && installed_default &&
                        dialog_auto::IsRetiredDisabledPulseDialogDll(desired_arch,status);
                    if(!owned && !orphaned && !retired) return ERROR_ALREADY_EXISTS;
                    if(orphaned) reclaimed_orphans.emplace_back(desired_arch,status.dll_path);
                    if(retired) retired_disabled.emplace_back(desired_arch,status.dll_path);
                    if(status.enabled) stale_enabled.push_back({desired_arch,status.dll_path});
                }
            }
            if(!stale_enabled.empty()) {
                const auto disabled=dialog_auto::ExecuteRegistrationChange(stale_enabled,false);
                if(disabled.error) {
                    result.auto_rollback_error=disabled.rollback_error;
                    return disabled.error;
                }
            }
            for(const auto& [desired_arch,target_dll]:desired) {
                const auto current=dialog_auto::Inspect(desired_arch);
                if(current.error) return current.error;
                if(current.conflict) return ERROR_ALREADY_EXISTS;
                dialog_auto::RegistrationTarget target{desired_arch,target_dll};
                const auto retired=std::find_if(retired_disabled.begin(),retired_disabled.end(),[&](const auto& item){
                    return item.first==desired_arch;
                });
                if(retired!=retired_disabled.end()) {
                    if(_wcsicmp(retired->second.c_str(),current.dll_path.c_str())!=0 ||
                        !dialog_auto::IsRetiredDisabledPulseDialogDll(desired_arch,current)) return ERROR_REVISION_MISMATCH;
                    target.require_retired_disabled=true;
                }
                if(!current.dll_path.empty() && _wcsicmp(current.dll_path.c_str(),target_dll.c_str())!=0) {
                    if(!dialog_auto::IsOwnedDialogAutoDll(base,installed_default,desired_arch,current.dll_path) &&
                        !is_reclaimed(desired_arch,current.dll_path) && !target.require_retired_disabled) return ERROR_ALREADY_EXISTS;
                    target.expected_old_dll_path=current.dll_path;
                }
                targets.push_back(std::move(target));
            }
            auto changed=dialog_auto::ExecuteRegistrationChange(targets,true);
            if((changed.error==ERROR_FILE_NOT_FOUND || changed.error==ERROR_ALREADY_EXISTS) && !changed.rollback_error &&
                // Retired cross-install journals require typed zero flags. Never manufacture
                // those preconditions by normalizing a rejected migration through DisableFor.
                std::none_of(targets.begin(),targets.end(),[](const auto& t){return t.require_retired_disabled;}) &&
                std::any_of(targets.begin(),targets.end(),[](const auto& t){return !t.expected_old_dll_path.empty();})) {
                std::vector<dialog_auto::RegistrationTarget> normalize_targets;
                for(auto& t:targets) {
                    const auto st=dialog_auto::Inspect(t.architecture);
                    if(st.error || st.conflict) {normalize_targets.clear();break;}
                    if(!st.dll_path.empty() && _wcsicmp(st.dll_path.c_str(),t.dll_path.c_str())!=0 &&
                        (dialog_auto::IsOwnedDialogAutoDll(base,installed_default,t.architecture,st.dll_path) ||
                         is_reclaimed(t.architecture,st.dll_path))) {
                        normalize_targets.push_back({t.architecture,st.dll_path});
                        t.expected_old_dll_path=st.dll_path;
                    } else {
                        t.expected_old_dll_path.clear();
                    }
                }
                if(!normalize_targets.empty()) {
                    const auto normalized=dialog_auto::ExecuteRegistrationChange(normalize_targets,false);
                    if(!normalized.error) {
                        changed=dialog_auto::ExecuteRegistrationChange(targets,true);
                    }
                }
            }
            result.auto_rollback_error=changed.rollback_error;
            return changed.error;
        }
        for(const auto arch:{Architecture::X64,Architecture::X86}) {
            const auto selected_arch=primary?(primary.Machine()==IMAGE_FILE_MACHINE_I386?Architecture::X86:Architecture::X64):dialog_auto::CurrentArchitecture;
            if(!installed_default && arch!=selected_arch) continue;
            const auto status=dialog_auto::Inspect(arch);
            if(status.error==ERROR_NOT_SUPPORTED && arch!=dialog_auto::CurrentArchitecture) continue;
            if(status.error) return status.error;
            if(status.conflict) return ERROR_ALREADY_EXISTS;
            if(status.dll_path.empty()) continue;
            if(!dialog_auto::IsOwnedDialogAutoDll(base,installed_default,arch,status.dll_path)) {
                if(installed_default && dialog_auto::IsOrphanedMissingPulseDialogDll(arch,status)) {
                    targets.push_back({arch,status.dll_path});
                    continue;
                }
                if(status.enabled && (installed_default || (primary && primary.Machine()==static_cast<unsigned>(arch==Architecture::X86?IMAGE_FILE_MACHINE_I386:IMAGE_FILE_MACHINE_AMD64))))
                    return ERROR_ALREADY_EXISTS;
                continue;
            }
            targets.push_back({arch,status.dll_path});
        }
        if(targets.empty()) return ERROR_SUCCESS;
        const auto changed=dialog_auto::ExecuteRegistrationChange(targets,false);
        result.auto_rollback_error=changed.rollback_error;
        return changed.error;
    }
    void Sessions(DialogManagementResult& result,const std::wstring& disable={},bool* export_limited=nullptr) {
        bool matched=disable.empty();
        unsigned examined=0;
        const auto stop=[&]{
            if(!export_limited || (Current(result.generation) && ++examined<=128))return false;
            *export_limited=true;return true;
        };
        for(const auto& operation:fs::directory_iterator(root/L"operations")) {
            if(stop())return;
            if(!operation.is_directory() || !IsGuid(operation.path()) || NoReparse(operation.path())) continue;
            Profile profile;if(dialog_launcher::LoadProfile((operation.path()/L"launch.profile").wstring(),profile)) continue;
            const auto control=operation.path()/L"control";
            if(_wcsicmp(Plain(dialog_session::Canonical(control,true)).c_str(),profile.control_root.c_str())!=0) continue;
            // Saved per-operation profile, independent of current selected package.
            dialog_bundle::BundleLease bundle;
            DWORD bundle_error=dialog_bundle::VerifyBundle(profile.bundle_root,profile.manifest_digest,bundle);
            if(!bundle_error) bundle_error=dialog_launcher::ValidateProfile(profile,bundle.Launcher());
            if(!fs::exists(control) || NoReparse(control)) continue;
            for(const auto& entry:fs::directory_iterator(control)) {
                if(stop())return;
                if(!entry.is_directory() || !IsGuid(entry.path()) || NoReparse(entry.path())) continue;
                DialogManagedSession item;item.directory=entry.path().wstring();
                const bool selected=!disable.empty() && _wcsicmp(disable.c_str(),item.directory.c_str())==0;
                if(selected) matched=true;
                dialog_session::Status status;
                if(bundle_error) item.status_error=bundle_error;
                else if(selected) {
#ifdef _WIN64
                    const WORD native_machine=IMAGE_FILE_MACHINE_AMD64;
#else
                    const WORD native_machine=IMAGE_FILE_MACHINE_I386;
#endif
                    item.status_error=profile.machine!=native_machine ? ERROR_NOT_SUPPORTED :
                        dialog_session::Inspect(item.directory,bundle.Launcher(),true,status);
                } else item.status_error=dialog_session::InspectForMachine(item.directory,bundle.Launcher(),static_cast<WORD>(profile.machine),status);
                item.pid=status.pid;item.application=std::move(status.application);item.process_alive=status.process_alive;
                item.historical_handshake=status.historical_handshake;item.local_disabled=status.local_disabled;
                item.initialization=status.initialization;item.entry_coverage=status.entry_coverage;
                if(selected && item.status_error) result.error=item.status_error;
                result.sessions.push_back(std::move(item));
            }
        }
        if(!matched) result.error=ERROR_NOT_FOUND;
    }
    DWORD Start(const Command& command,dialog_bundle::BundleLease& bundle,bool& started) {
        if(!Current(command.generation)) return ERROR_CANCELLED;
        auto target=configuration.application;WORD machine=0;
        if(const DWORD error=ValidateApplication(target,&machine)) return error;
        if(machine!=bundle.Machine()) return ERROR_EXE_MACHINE_TYPE_MISMATCH;
        const auto nonce=Unique();if(nonce.empty()) return ERROR_GEN_FAILURE;
        const auto operation=root/L"operations"/nonce,control=operation/L"control";
        CurrentUserSecurityAttributes security;if(!security) return ERROR_ACCESS_DENIED;
        if(!CreateDirectoryW(operation.c_str(),security.get()) || !CreateDirectoryW(control.c_str(),security.get())) return GetLastError();
        Profile profile;profile.format_version=2;profile.bundle_root=bundle.Directory();profile.manifest_digest=bundle.ManifestDigest();
        profile.host=bundle.Host();profile.runtime=bundle.Runtime();profile.target=target;
        profile.cwd=fs::path(profile.target).parent_path().wstring();profile.control_root=control.wstring();
        DWORD error=dialog_launcher::CaptureProfile(profile,bundle.Launcher());
        const auto profile_file=operation/L"launch.profile";
        if(!error) error=dialog_launcher::SaveProfile(profile_file.wstring(),profile);
        if(!error) error=dialog_bundle::RecheckDirectory(bundle);
        WORD current_machine=0;auto current_target=configuration.application;
        if(!error) error=ValidateApplication(current_target,&current_machine);
        if(!error && (current_machine!=bundle.Machine() || _wcsicmp(current_target.c_str(),profile.target.c_str()))) error=ERROR_EXE_MACHINE_TYPE_MISMATCH;
        if(error || !Current(command.generation)) return error?error:ERROR_CANCELLED;
        auto text=Quote(bundle.Launcher())+L" --profile "+Quote(profile_file.wstring());
        STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION info{};
        // No inherited handles, stdout pipe, shell interpretation or kill-on-close job.
        if(!CreateProcessW(bundle.Launcher().c_str(),text.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,
            bundle.Directory().c_str(),&startup,&info)) return GetLastError();
        Handle process{info.hProcess},thread{info.hThread};started=true;
        const auto deadline=GetTickCount64()+15000;
        for(;;) {
            const DWORD wait=WaitForSingleObject(process.h,100);
            if(wait==WAIT_OBJECT_0) break;
            if(wait!=WAIT_TIMEOUT) return GetLastError();
            if(!Current(command.generation)) return ERROR_CANCELLED;
            if(GetTickCount64()>=deadline) return ERROR_TIMEOUT;
        }
        DWORD code=0;if(!GetExitCodeProcess(process.h,&code)) return GetLastError();
        return code==0?ERROR_SUCCESS:(code==3?ERROR_TIMEOUT:ERROR_GEN_FAILURE);
    }
    void ObserveRestart(const Command& command,DialogManagementResult& result) {
        // Live process evidence is deliberately separate from persisted history.
        // Keep verified file leases alive through the scan, so an update cannot
        // replace the compared components underneath this observation.
        if(command.path==L"refresh-running" || !restart_checked_at || GetTickCount64()-restart_checked_at>=60000) {
            CommonBridgeInfo common_bridge;
            dialog_restart::CurrentComponents current;
            dialog_bundle::BundleLease primary,companion;
            if (ResolveCommonBridgeForInstall(install_root, common_bridge)) {
                current.install_root = install_root.wstring();
                current.x64_directory = common_bridge.core_dir_x64;
                current.x64_error = ERROR_SUCCESS;
                current.x86_directory = common_bridge.core_dir_x86;
                current.x86_error = common_bridge.core_dir_x86.empty() ? ERROR_NOT_FOUND : ERROR_SUCCESS;
            } else {
                const auto primary_error=dialog_bundle::ResolveInstalledBundle(install_root.wstring(),primary);
                const auto companion_error=dialog_bundle::ResolveInstalledBundleForMachine(install_root.wstring(),IMAGE_FILE_MACHINE_I386,companion);
                current=dialog_restart::CaptureCurrent(install_root.wstring(),
                    primary && primary.Machine()==IMAGE_FILE_MACHINE_AMD64 ? &primary : nullptr,
                    companion ? &companion : primary && primary.Machine()==IMAGE_FILE_MACHINE_I386 ? &primary : nullptr);
                if(primary_error) current.x64_error=primary_error;
                if(companion_error) current.x86_error=companion_error;
            }
            auto next=dialog_restart::Scan(current,[&]{return !Current(command.generation);},restart_inventory.next_pid);
            if(!next.cancelled && Current(command.generation)) {
                restart_inventory=std::move(next);restart_checked_at=GetTickCount64();
            }
        }
        result.restart_inventory=restart_inventory;
        result.restart_checked=restart_checked_at!=0;
    }
    // The automatic helper's circuit breakers, next to the data root it uses.
    DWORD ObservePaused(const Command& command,DialogManagementResult& result) {
        const auto store_path=root.parent_path()/L"dialog_adaptive_targets.json";
        dialog_adaptive::AdaptiveTargetStore store;
        const bool loaded=store.Load(store_path);
        DWORD error=0;
        if(command.operation==DialogManagementOperation::ResumePaused && loaded && store.ResumeBasename(json::Utf8(command.path)) && !store.Save(store_path))
            error=ERROR_WRITE_FAULT;
        FILETIME now{};GetSystemTimeAsFileTime(&now);
        paused=store.PausedTargets((static_cast<uint64_t>(now.dwHighDateTime)<<32)|now.dwLowDateTime);
        result.paused=paused;
        return error;
    }
    void ObserveHistory(const Command& command,DialogManagementResult& result) {
        const DWORD paused_error=ObservePaused(command,result);
        ObserveRestart(command,result);
        if(!history_loaded) {
            history_load_error=dialog_history::LoadHistory(root.wstring(),history);
            if(history_load_error==ERROR_FILE_NOT_FOUND || history_load_error==ERROR_PATH_NOT_FOUND) history_load_error=0;
            history_loaded=true;
        }
        if(command.operation==DialogManagementOperation::ClearHistory) {
            // "all", or "<image id>:<reason>" of one record.
            const bool all=command.path==L"all";
            wchar_t* end=nullptr;
            const uint64_t id=all ? 0 : wcstoull(command.path.c_str(),&end,10);
            const uint32_t reason=!all && end && *end==L':' ? static_cast<uint32_t>(wcstoul(end+1,nullptr,10)) : 0;
            if(all || id) dialog_history::ForgetRecords(history,all,id,static_cast<dialog_history::Reason>(reason));
        }
        // No PID-only historical joins. The collector requires producer identity.
        // New managed events also use the bounded global runtime log.
        auto next=history;
        const auto data=GetPulseDataDir();
        const auto observed=dialog_history::CollectTakeoverHistory(data.empty()?std::wstring{}:(fs::path(data)/L"Diagnostics"/L"Runtime").wstring(),{},next,
            [&]{return !Current(command.generation);});
        result.history_error=history_load_error ? history_load_error : observed.error ? observed.error : paused_error;
        if(!observed.cancelled && Current(command.generation)) {
            history=std::move(next);
            // Persist cursors as well as visible records: repeated polls and
            // application restarts must not count the same event again.
            const auto save_error=dialog_history::SaveHistory(root.wstring(),history);
            if(!result.history_error) result.history_error=save_error;
        }
        result.history=history;
        // Name and icon for each listed program, resolved once it can be found.
        std::vector<DialogRecordAppQuery> missing;
        // Misses (closed program, no App Paths entry) wait 5 minutes between tries, so
        // the minute poll does not enumerate processes again; "检查最新记录" retries now.
        const ULONGLONG now=GetTickCount64();
        const bool manual=command.path==L"refresh-running";
        const auto want=[&](const std::wstring& basename,uint64_t id) {
            if(const auto miss=record_app_misses.find(id);!manual && miss!=record_app_misses.end() && now-miss->second<300000) return;
            if(!id || record_apps.contains(id) ||
               std::any_of(missing.begin(),missing.end(),[&](const auto& query){return query.image_id==id;})) return;
            missing.push_back({basename,id});
        };
        for(const auto& record:history.records) want(record.app_basename,record.app_image_id);
        for(const auto& record:result.restart_inventory.records) want(record.app_basename,record.app_image_id);
        if(!missing.empty() && resolve_record_apps && Current(command.generation))
            for(auto& [id,app]:resolve_record_apps(missing)) {
                if(app.path.empty()) {record_app_misses[id]=now;continue;}
                record_app_misses.erase(id);
                record_apps[id]=std::move(app);
            }
        result.record_apps=record_apps;
    }
    DialogManagementResult Execute(const Command& command) {
        DialogManagementResult result;result.operation=command.operation;result.generation=command.generation;
        result.restart_inventory=restart_inventory;result.restart_checked=restart_checked_at!=0;
        result.error=Initialize();if(result.error) {result.history_error=result.error;return result;}
        if(command.operation==DialogManagementOperation::PollHistory || command.operation==DialogManagementOperation::ClearHistory ||
           command.operation==DialogManagementOperation::ResumePaused) {ObserveHistory(command,result);return result;}
        const auto inspected=dialog_auto::Inspect();
        result.auto_enabled=inspected.enabled;result.auto_conflict=inspected.conflict;
        result.auto_error=inspected.error;result.auto_dll=inspected.dll_path;
        if(!Current(command.generation)){result.error=ERROR_CANCELLED;return result;}
        if(command.operation==DialogManagementOperation::SelectBundle || command.operation==DialogManagementOperation::SelectApplication) {
            auto next=configuration;
            if(command.operation==DialogManagementOperation::SelectBundle) {
                dialog_bundle::BundleLease selected;result.error=dialog_bundle::VerifyBundle(command.path,command.digest,selected);
                if(!result.error){next.bundle_root=selected.Directory();next.manifest_digest=selected.ManifestDigest();}
            } else {next.application=command.path;result.error=ValidateApplication(next.application);}
            if(!result.error && Current(command.generation)){
                result.error=SaveConfiguration(root,next);if(!result.error) {
                    configuration=std::move(next);
                    if(command.operation==DialogManagementOperation::SelectBundle) installed_default=false;
                }
            }
        }
        dialog_bundle::BundleLease bundle;Verify(result,bundle);
        if(command.operation==DialogManagementOperation::EnableAuto || command.operation==DialogManagementOperation::DisableAuto) {
            if(Current(command.generation)) result.error=ChangeAuto(command.operation==DialogManagementOperation::EnableAuto,bundle,result);
            else result.error=ERROR_CANCELLED;
            const auto status=dialog_auto::Inspect();
            result.auto_enabled=status.enabled;result.auto_conflict=status.conflict;
            result.auto_error=status.error;result.auto_dll=status.dll_path;
            Verify(result,bundle);
        }
        dialog_bundle::BundleLease launch_bundle;
        if (installed_default && result.auto_enabled && !install_root.empty()) {
            CommonBridgeInfo common_bridge;
            if (ResolveCommonBridgeForInstall(install_root, common_bridge)) {
                result.admin_auto_checked = true;
                QueryDialogBrokerRunning(result.admin_auto_running, result.admin_auto_error);
            } else {
                if (command.operation == DialogManagementOperation::EnableAuto && !result.error) {
                    const auto started = dialog_admin_auto::StartExisting(install_root.wstring());
                    if (FAILED(started)) result.admin_auto_error = HRESULT_FACILITY(started) == FACILITY_WIN32 ? HRESULT_CODE(started) : static_cast<DWORD>(started);
                }
                dialog_admin_auto::Snapshot automatic;
                const auto inspected_task = dialog_admin_auto::Inspect(install_root.wstring(), automatic);
                result.admin_auto_checked = true;
                result.admin_auto_running = SUCCEEDED(inspected_task) && automatic.exists && automatic.running;
                if (FAILED(inspected_task)) result.admin_auto_error = HRESULT_FACILITY(inspected_task) == FACILITY_WIN32 ? HRESULT_CODE(inspected_task) : static_cast<DWORD>(inspected_task);
                else if (!result.admin_auto_running && !result.admin_auto_error) result.admin_auto_error = ERROR_NOT_READY;
            }
        }
        VerifyLaunch(result,launch_bundle);
        VerifyAdmin(result,launch_bundle);
        if(command.operation==DialogManagementOperation::PrepareLaunch) {
            if(!result.application_valid || !result.launch_bundle_verified) result.error=result.launch_bundle_error ? result.launch_bundle_error : ERROR_INVALID_DATA;
            else if(const auto context_error=OrdinaryLaunchContext()) result.error=context_error;
            else {
                const auto observed=dialog_launcher::ScanExistingTarget(configuration.application,
                    [&]{return !Current(command.generation);});
                result.launch_preflight_checked=true;
                result.existing_target_count=observed.existing_target_count;
                result.launch_scan_complete=observed.scan_complete;
                result.launch_query_errors=observed.query_errors;
                if(observed.cancelled) result.error=ERROR_CANCELLED;
            }
        }
        if(command.operation==DialogManagementOperation::Launch) {
            if(!result.application_valid || !result.launch_bundle_verified) result.error=result.launch_bundle_error ? result.launch_bundle_error : ERROR_INVALID_DATA;
            else if(const auto context_error=OrdinaryLaunchContext()) result.error=context_error;
            else result.error=Start(command,launch_bundle,result.launcher_started);
        }
        if(command.operation==DialogManagementOperation::PrepareAdminLaunch) {
            if(!result.admin_launch_ready) result.error=result.admin_launch_error ? result.admin_launch_error : ERROR_INVALID_DATA;
            else {
                const auto observed=dialog_launcher::ScanExistingTarget(configuration.application,[&]{return !Current(command.generation);});
                result.launch_preflight_checked=true;result.existing_target_count=observed.existing_target_count;
                result.launch_scan_complete=observed.scan_complete;result.launch_query_errors=observed.query_errors;
                if(observed.cancelled) result.error=ERROR_OPERATION_ABORTED;
            }
        }
        if(command.operation==DialogManagementOperation::LaunchAdmin) {
            if(!result.admin_launch_ready) result.error=result.admin_launch_error ? result.admin_launch_error : ERROR_INVALID_DATA;
            else result.error=StartAdmin(command,launch_bundle,result.launcher_started);
        }
        if(command.operation==DialogManagementOperation::Refresh || command.operation==DialogManagementOperation::ExportDiagnostics) ObserveHistory(command,result);
        else {result.history=history;result.history_error=history_load_error;result.paused=paused;}
        if(command.operation==DialogManagementOperation::ExportDiagnostics && Current(command.generation)) {
            const auto* reproduction=command.reproduction.start_filetime ? &command.reproduction : nullptr;
            const bool include_raw=!command.path.empty() || reproduction;
            bool managed_scan_limited=false;
            if(include_raw || !requested_root.empty() || !configuration.application.empty()) Sessions(result,{},&managed_scan_limited);
            const auto nonce=Unique();
            if(nonce.empty()) { result.error=ERROR_GEN_FAILURE; return result; }
            CurrentUserSecurityAttributes security;
            if(!security) { result.error=ERROR_ACCESS_DENIED; return result; }
            struct Staging {fs::path dir;~Staging(){if(!dir.empty()){std::error_code ignored;fs::remove_all(dir,ignored);}}} staging;
            fs::path archive_destination;
            if(include_raw) {
                std::wstring target=command.path;
                if(reproduction && target.empty()) {
                    PWSTR desktop=nullptr;
                    const HRESULT hr=SHGetKnownFolderPath(FOLDERID_Desktop,KF_FLAG_DEFAULT,nullptr,&desktop);
                    if(SUCCEEDED(hr) && desktop) target=desktop;
                    CoTaskMemFree(desktop);
                    if(target.empty()) {result.error=ERROR_PATH_NOT_FOUND;return result;}
                }
                const auto directory=Plain(dialog_session::Canonical(fs::path(target),true));
                if(directory.empty() || target.starts_with(L"\\\\.\\")) {result.error=ERROR_PATH_NOT_FOUND;return result;}
                const auto new_stage=fs::path(directory)/(L".pulse-dialog-export-"+nonce);
                if(!CreateDirectoryW(new_stage.c_str(),security.get())){result.error=GetLastError();return result;}
                staging.dir=new_stage;
                SYSTEMTIME now{};GetLocalTime(&now);wchar_t date[32]{};
                swprintf_s(date,L"%04u%02u%02u-%02u%02u%02u",now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond);
                archive_destination=fs::path(directory)/(L"Pulse-dialog-diagnostics-"+std::wstring(date)+L"-"+nonce.substr(1,8)+L".zip");
            }
            const auto path=include_raw ? staging.dir/L"report.txt" : root/(L"diagnostics-"+nonce+L".txt");
            const std::string description=include_raw ?
                "Scope: dialog takeover status, selected original dialog-component logs and a structured Explorer handoff trace. Raw files may contain private fields.\r\n" :
                "Scope: status only; read-only process metadata and sanitized event excerpts; no raw logs, paths, filenames or file contents.\r\n";
            uint64_t bucket_succeeded=0,bucket_contract_fallback=0,bucket_provider_fallback=0,bucket_shown_then_failed=0;
            for(const auto& rec : result.history.records) {
                using R=dialog_history::Reason;
                if(rec.state==dialog_history::State::Success || rec.reason==R::Succeeded || rec.reason==R::ShownClosed) {
                    bucket_succeeded+=rec.count;
                } else if(rec.reason==R::ShownThenFailed) {
                    bucket_shown_then_failed+=rec.count;
                } else if(rec.reason==R::UnsupportedRequest || rec.reason==R::InitialFolderUnsupported || rec.reason==R::NativeFallback) {
                    bucket_contract_fallback+=rec.count;
                } else {
                    bucket_provider_fallback+=rec.count;
                }
            }
            const auto resolved_bridge_root=dialog_bridge_paths::ResolveActiveBridgeRoot(Plain(install_root.wstring()));
            const bool bridge_root_protected=!resolved_bridge_root.empty() &&
                dialog_admin::VerifyProtectedPath(resolved_bridge_root,true)==ERROR_SUCCESS;
            std::string report="Pulse dialog integration diagnostics v3\r\n"+description+
                "Process snapshots are not dialog requests, activation proof or a coverage denominator. Registration status is the collector context only.\r\n"
                "auto_enabled="+std::to_string(result.auto_enabled)+"\r\nauto_conflict="+std::to_string(result.auto_conflict)
                +"\r\nauto_path_mismatch="+std::to_string(result.auto_path_mismatch)
                +"\r\nauto_error="+std::to_string(result.auto_error)+"\r\nauto_rollback_error="+std::to_string(result.auto_rollback_error)+"\r\nbundle_verified="+std::to_string(result.bundle_verified)
                +"\r\nbundle_error="+std::to_string(result.bundle_error)
                +"\r\nbridge_root_protected="+std::to_string(bridge_root_protected)
                +"\r\nhistory_records="+std::to_string(result.history.records.size())
                +"\r\nhistory_unattributed="+std::to_string(result.history.unattributed_events)
                +"\r\nhistory_partial="+std::to_string(result.history.partial)
                +"\r\nhistory_error="+std::to_string(result.history_error)
                +"\r\nbucket_succeeded="+std::to_string(bucket_succeeded)
                +"\r\nbucket_contract_fallback="+std::to_string(bucket_contract_fallback)
                +"\r\nbucket_provider_fallback="+std::to_string(bucket_provider_fallback)
                +"\r\nbucket_shown_then_failed="+std::to_string(bucket_shown_then_failed)
                +"\r\nrestart_checked="+std::to_string(result.restart_checked)
                +"\r\nrestart_applications="+std::to_string(result.restart_inventory.records.size())
                +"\r\nrestart_scan_complete="+std::to_string(result.restart_inventory.scan_complete)
                +"\r\nrestart_query_errors="+std::to_string(result.restart_inventory.query_errors)
                +"\r\nselected_application_machine="+std::to_string(result.application_machine)
                +"\r\nmanaged_launch_bundle_verified="+std::to_string(result.launch_bundle_verified)
                +"\r\nmanaged_launch_bundle_error="+std::to_string(result.launch_bundle_error)
                +"\r\nadmin_launch_ready="+std::to_string(result.admin_launch_ready)
                +"\r\nadmin_launch_error="+std::to_string(result.admin_launch_error)
                +"\r\nadmin_auto_checked="+std::to_string(result.admin_auto_checked)
                +"\r\nadmin_auto_running="+std::to_string(result.admin_auto_running)
                +"\r\nadmin_auto_error="+std::to_string(result.admin_auto_error)
                +"\r\nselected_application="+(configuration.application.empty()?"not_selected":result.application_valid?"valid":"invalid")+"\r\n"
                +dialog_diagnostics::CaptureVisibleProcesses([&]{return !Current(command.generation);});
            for(bool x86:{false,true}) {
                const auto status=dialog_auto::InspectArchStatus(HKEY_CURRENT_USER,x86,component_directories[x86?1:0]);
                constexpr const char* registrations[]{"unregistered","registered","disabled_matching","conflict","read_error"};
                constexpr const char* components[]{"unknown","missing","present"};
                report+="{\"schema\":1,\"event\":\"registration_snapshot\",\"data\":{\"scope\":\"current_collector_context\",\"process_arch\":"+std::to_string(x86?32:64)
                    +",\"registration\":\""+registrations[static_cast<size_t>(status.registration)]+"\",\"component\":\""+components[static_cast<size_t>(status.component)]
                    +"\",\"registry_error\":"+std::to_string(status.registry_error)+",\"component_error\":"+std::to_string(status.component_error)+"}}\n";
            }
            const auto data=GetPulseDataDir();
            const auto runtime_dir=data.empty()?std::wstring{}:(fs::path(data)/L"Diagnostics"/L"Runtime").wstring();
            const auto cancelled=[&]{return !Current(command.generation);};
            if(include_raw) {
                const bool flushed=diagnostics::runtime::Flush(1500);
                report+="{\"schema\":1,\"event\":\"explorer_log_export\",\"data\":{\"runtime_flush\":"+
                    std::string(flushed?"true":"false")+"}}\n";
                report+=dialog_diagnostics::CollectExplorerLogExcerpt(runtime_dir,cancelled,reproduction);
            }
            dialog_diagnostics::RawLogCopy raw;
            unsigned started=0,finished=0,foreground_started=0,foreground_finished=0;
            const auto append=[&](const std::wstring& source,bool managed,const std::wstring& label){
                dialog_diagnostics::ExcerptCounts counts;
                report+=dialog_diagnostics::CollectDialogLogExcerpt(source,cancelled,&counts,managed,reproduction);
                started+=counts.shows_started;finished+=counts.shows_finished;
                foreground_started+=counts.foreground_shows_started;foreground_finished+=counts.foreground_shows_finished;
                if(!include_raw || cancelled())return;
                const auto copy_to=staging.dir/label;
                if(!CreateDirectoryW(copy_to.c_str(),nullptr)) {
                    ++raw.read_errors;raw.last_error=GetLastError();return;
                }
                dialog_diagnostics::CopyDialogRawLogs(source,copy_to.wstring(),managed,raw,cancelled);
            };
            append(runtime_dir,false,L"runtime");
            unsigned collected=0, unavailable=0; DWORD source_error=0; bool source_limit=false;
            for(const auto& session:result.sessions) {
                if(!Current(command.generation)) break;
                if(session.status_error) {++unavailable;source_error=session.status_error;continue;}
                if(collected==4) {source_limit=true;break;}
                ++collected;
                append(session.directory,true,L"managed-"+std::to_wstring(collected));
            }
            report+="{\"schema\":1,\"event\":\"managed_log_sources\",\"data\":{\"collected\":"+std::to_string(collected)+",\"unavailable_sources\":"+std::to_string(unavailable)
                +",\"last_status_error\":"+std::to_string(source_error)+",\"limit_reached\":"+((source_limit||managed_scan_limited)?"true":"false")+",\"no_entry_inference\":true}}\n";
            result.diagnostics_raw_files=raw.files;
            result.diagnostics_raw_incomplete=raw.read_errors || raw.skipped || raw.limited || managed_scan_limited || source_limit || unavailable;
            if(reproduction) {
                json::Value context=json::Value::Object();
                context.set("schema",1);context.set("event","user_reproduction");
                context.set("start_filetime",std::to_string(reproduction->start_filetime));
                context.set("end_filetime",std::to_string(reproduction->end_filetime));
                context.set("foreground_pid",static_cast<uint64_t>(reproduction->foreground_pid));
                constexpr const char* operations[]{"other","open","save","folder"};
                context.set("reported_operation",operations[(std::min)(reproduction->reported_operation,3u)]);
                context.set("foreground_is_hint",true);
                context.set("foreground_show_begin",static_cast<uint64_t>(foreground_started));
                context.set("foreground_show_terminal",static_cast<uint64_t>(foreground_finished));
                if(reproduction->foreground_pid) {
                    Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,reproduction->foreground_pid)};
                    wchar_t image[32768]{};DWORD units=ARRAYSIZE(image);
                    FILETIME creation{},exit{},kernel{},user{};
                    if(process.h && GetProcessTimes(process.h,&creation,&exit,&kernel,&user) &&
                        ((static_cast<uint64_t>(creation.dwHighDateTime)<<32)|creation.dwLowDateTime)<=reproduction->end_filetime &&
                        QueryFullProcessImageNameW(process.h,0,image,&units))
                        context.set("foreground_image",json::Utf8(fs::path(image).filename().wstring()));
                }
                report+=json::Serialize(context)+"\n";
            }
            report.insert(report.find("\r\n")+2,"show_begin_events="+std::to_string(started)+"\r\nshow_terminal_events="+std::to_string(finished)
                +"\r\nraw_files="+std::to_string(raw.files)+"\r\nraw_bytes="+std::to_string(raw.bytes)
                +"\r\nraw_skipped="+std::to_string(raw.skipped)+"\r\nraw_read_errors="+std::to_string(raw.read_errors)
                +"\r\nraw_last_error="+std::to_string(raw.last_error)+"\r\nraw_limited="+std::to_string(raw.limited)
                +"\r\nraw_incomplete="+std::to_string(result.diagnostics_raw_incomplete)
                +"\r\nrequest_evidence="+(finished?"terminal_records_available":started?"begin_only":"no_request_observed")
                +"\r\n\r\n");
            if(!Current(command.generation)){result.error=ERROR_CANCELLED;return result;}
            Handle file{CreateFileW(path.c_str(),GENERIC_WRITE,0,security.get(),CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr)};
            if(file.h==INVALID_HANDLE_VALUE) {result.error=GetLastError();return result;}
            DWORD written=0;
            if(!WriteFile(file.h,report.data(),static_cast<DWORD>(report.size()),&written,nullptr) || written!=report.size() || !FlushFileBuffers(file.h)) result.error=ERROR_WRITE_FAULT;
            else result.diagnostics_path=path.wstring();
            CloseHandle(file.h);file.h=nullptr;
            if(result.error || !Current(command.generation)) {DeleteFileW(path.c_str());result.diagnostics_path.clear();return result;}
            if(include_raw) {
                result.diagnostics_path.clear();
                const auto staged_zip=staging.dir/L"package.zip";
                if(!diagnostics::CreateSupportArchive(staging.dir.wstring(),staged_zip.wstring(),nullptr,&result.error))return result;
                if(cancelled()){result.error=ERROR_CANCELLED;return result;}
                if(!MoveFileExW(staged_zip.c_str(),archive_destination.c_str(),MOVEFILE_WRITE_THROUGH)) {
                    result.error=GetLastError();return result;
                }
                result.diagnostics_path=archive_destination.wstring();
            }
        }
        if(command.operation!=DialogManagementOperation::ExportDiagnostics && Current(command.generation) && (!requested_root.empty() || !configuration.application.empty())) Sessions(result,command.operation==DialogManagementOperation::Disable?command.path:std::wstring{});
        return result;
    }
    void Run() noexcept {
        for(;;) {
            Command command;
            {std::unique_lock lock(mutex);cv.wait(lock,[&]{return stopping || !commands.empty();});if(stopping) return;command=std::move(commands.front());commands.pop_front();}
            DialogManagementResult result;
            try{result=Execute(command);}catch(...){result.operation=command.operation;result.generation=command.generation;result.error=ERROR_GEN_FAILURE;}
            result.auto_arch={dialog_auto::InspectArchStatus(HKEY_CURRENT_USER,false,component_directories[0]),
                dialog_auto::InspectArchStatus(HKEY_CURRENT_USER,true,component_directories[1])};
            try {
                std::lock_guard lock(mutex);if(stopping || generation!=command.generation) continue;
                results.clear();results.push_back(std::move(result));if(host && wake) PostMessageW(host,wake,0,0);
            } catch(...) {}
        }
    }
};
DialogManagementController::DialogManagementController(HWND host,UINT wake,std::wstring root,RecordAppResolver resolve_record_apps):state_(std::make_shared<State>()){
    state_->host=host;state_->wake=wake;state_->requested_root=std::move(root);state_->resolve_record_apps=std::move(resolve_record_apps);
    std::thread([state=state_]{state->Run();}).detach();
}
DialogManagementController::~DialogManagementController(){
    std::lock_guard lock(state_->mutex);state_->stopping=true;state_->host=nullptr;state_->commands.clear();state_->results.clear();++state_->generation;state_->cv.notify_one();
}
uint64_t DialogManagementController::Submit(DialogManagementOperation operation,std::wstring path,std::string digest,dialog_diagnostics::ReproductionWindow reproduction){
    std::lock_guard lock(state_->mutex);const auto generation=++state_->generation;
    state_->commands.clear();state_->results.clear();state_->commands.push_back({operation,generation,std::move(path),std::move(digest),reproduction});state_->cv.notify_one();return generation;
}
uint64_t DialogManagementController::SelectBundle(std::wstring path,std::string digest){return Submit(DialogManagementOperation::SelectBundle,std::move(path),std::move(digest));}
uint64_t DialogManagementController::SelectApplication(std::wstring path){return Submit(DialogManagementOperation::SelectApplication,std::move(path));}
uint64_t DialogManagementController::PrepareLaunch(){return Submit(DialogManagementOperation::PrepareLaunch);}
uint64_t DialogManagementController::Launch(){return Submit(DialogManagementOperation::Launch);}
uint64_t DialogManagementController::PrepareAdminLaunch(){return Submit(DialogManagementOperation::PrepareAdminLaunch);}
uint64_t DialogManagementController::LaunchAdmin(){return Submit(DialogManagementOperation::LaunchAdmin);}
uint64_t DialogManagementController::EnableAuto(){return Submit(DialogManagementOperation::EnableAuto);}
uint64_t DialogManagementController::DisableAuto(){return Submit(DialogManagementOperation::DisableAuto);}
uint64_t DialogManagementController::ExportDiagnostics(std::wstring destination_directory){
    return Submit(DialogManagementOperation::ExportDiagnostics,std::move(destination_directory));
}
uint64_t DialogManagementController::ExportFeedback(dialog_diagnostics::ReproductionWindow reproduction,std::wstring destination_directory){
    if(!reproduction.start_filetime || reproduction.end_filetime<reproduction.start_filetime) return 0;
    return Submit(DialogManagementOperation::ExportDiagnostics,std::move(destination_directory),{},reproduction);
}
uint64_t DialogManagementController::PollHistory(bool refresh_running_applications){return Submit(DialogManagementOperation::PollHistory,refresh_running_applications ? L"refresh-running" : L"");}
uint64_t DialogManagementController::ResumePaused(std::wstring basename){return basename.empty() ? 0 : Submit(DialogManagementOperation::ResumePaused,std::move(basename));}
uint64_t DialogManagementController::ClearHistory(bool all,uint64_t app_image_id,uint32_t reason){
    return Submit(DialogManagementOperation::ClearHistory,all ? std::wstring(L"all") : std::to_wstring(app_image_id)+L":"+std::to_wstring(reason));
}
uint64_t DialogManagementController::Refresh(){return Submit(DialogManagementOperation::Refresh);}
uint64_t DialogManagementController::Disable(std::wstring path){return Submit(DialogManagementOperation::Disable,std::move(path));}
void DialogManagementController::Cancel() noexcept{std::lock_guard lock(state_->mutex);++state_->generation;state_->commands.clear();state_->results.clear();}
std::vector<DialogManagementResult> DialogManagementController::Drain(){std::lock_guard lock(state_->mutex);auto result=std::move(state_->results);state_->results.clear();return result;}
}

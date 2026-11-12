#include "../common/app_runtime_paths.h"
#include "../common/dialog_admin_trust.h"
#include <shellapi.h>
#include "dialog_management_controller.h"
#include <algorithm>
#include "../common/dialog_auto_registration.h"
#include "../common/dialog_auto_transaction.h"
#include "../common/dialog_bundle.h"
#include "../common/dialog_session.h"
#include "../common/dialog_process_diagnostics.h"
#include "../common/dialog_log_excerpt.h"
#include "../common/dialog_admin_auto_task.h"
#include "../common/current_user_security.h"
#include "../dialog_launcher/managed_launch_profile.h"
#include "../dialog_launcher/target_preflight.h"
#include <objbase.h>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <array>
namespace pulse::app {
std::wstring GetPulseDataDir();
bool DialogAutoPathMismatch(bool enabled, const std::wstring& registered, const std::wstring& bundle_root) {
    if (!enabled || bundle_root.empty()) return false;
    const auto expected = (std::filesystem::path(bundle_root) / dialog_auto::DllName).lexically_normal().wstring();
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
}
struct DialogManagementController::State {
    struct Command {DialogManagementOperation operation;uint64_t generation;std::wstring path;std::string digest;};
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
        else if(!result.application_valid || !result.launch_bundle_verified)
            result.admin_launch_error=result.launch_bundle_error ? result.launch_bundle_error : ERROR_BAD_EXE_FORMAT;
        else if(!dialog_session::AllowedProcess(GetCurrentProcess(),dialog_admin::IsElevated())) result.admin_launch_error=ERROR_ACCESS_DENIED;
        else result.admin_launch_error=dialog_admin::VerifyAdminLaunchLocation(Plain(install_root.wstring()),
            Plain(launch.Directory()),Plain(launch.Host()),static_cast<WORD>(launch.Machine()));
        result.admin_launch_ready=result.admin_launch_error==ERROR_SUCCESS;
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
        if(enable) {
            DWORD error=result.bundle_verified?dialog_bundle::RecheckDirectory(primary):result.bundle_error;
            if(error) return error;
            if(_wcsicmp(primary.Directory().c_str(),install_root.c_str())==0) return ERROR_NOT_SUPPORTED;
            const auto arch=primary.Machine()==IMAGE_FILE_MACHINE_I386?Architecture::X86:Architecture::X64;
            targets.push_back({arch,primary.Directory()+L"\\"+dialog_auto::DllNameFor(arch)});
            if(installed_default && !component_directories[1].empty() && arch==Architecture::X64) {
                error=dialog_bundle::ResolveInstalledBundleForMachine(install_root.wstring(),IMAGE_FILE_MACHINE_I386,companion);
                if(error) return error;
                if((error=dialog_bundle::RecheckDirectory(companion))) return error;
                targets.push_back({Architecture::X86,companion.Directory()+L"\\"+dialog_auto::DllNameFor(Architecture::X86)});
            }
        } else {
            for(const auto arch:{Architecture::X64,Architecture::X86}) {
                const auto selected_arch=primary?(primary.Machine()==IMAGE_FILE_MACHINE_I386?Architecture::X86:Architecture::X64):dialog_auto::CurrentArchitecture;
                if(!installed_default && arch!=selected_arch) continue;
                const auto status=dialog_auto::Inspect(arch);
                if(status.error==ERROR_NOT_SUPPORTED && arch!=dialog_auto::CurrentArchitecture) continue;
                if(status.error) return status.error;
                if(status.conflict) return ERROR_ALREADY_EXISTS;
                if(status.dll_path.empty()) continue;
                const auto name=std::wstring(dialog_auto::DllNameFor(arch));
                const auto base=installed_default?install_root.wstring():configuration.bundle_root;
                bool owned=!base.empty() && _wcsicmp(status.dll_path.c_str(),(base+L"\\"+name).c_str())==0;
                const auto prefix=base+L"\\DialogBridge\\";
                if(installed_default && !base.empty() && status.dll_path.size()==prefix.size()+65+name.size() &&
                    _wcsnicmp(status.dll_path.c_str(),prefix.c_str(),prefix.size())==0) {
                    const auto digest=status.dll_path.substr(prefix.size(),64);
                    owned=digest.find_first_not_of(L"0123456789abcdef")==std::wstring::npos &&
                        _wcsicmp(status.dll_path.substr(prefix.size()+64).c_str(),(L"\\"+name).c_str())==0;
                }
                if(!owned) {
                    if(status.enabled && (installed_default || (primary && primary.Machine()==static_cast<unsigned>(arch==Architecture::X86?IMAGE_FILE_MACHINE_I386:IMAGE_FILE_MACHINE_AMD64))))
                        return ERROR_ALREADY_EXISTS;
                    continue;
                }
                targets.push_back({arch,status.dll_path});
            }
        }
        if(targets.empty()) return ERROR_SUCCESS;
        const auto changed=dialog_auto::ExecuteRegistrationChange(targets,enable);
        result.auto_rollback_error=changed.rollback_error;
        return changed.error;
    }
    void Sessions(DialogManagementResult& result,const std::wstring& disable={}) {
        bool matched=disable.empty();
        for(const auto& operation:fs::directory_iterator(root/L"operations")) {
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
            dialog_bundle::BundleLease primary,companion;
            const auto primary_error=dialog_bundle::ResolveInstalledBundle(install_root.wstring(),primary);
            const auto companion_error=dialog_bundle::ResolveInstalledBundleForMachine(install_root.wstring(),IMAGE_FILE_MACHINE_I386,companion);
            auto current=dialog_restart::CaptureCurrent(install_root.wstring(),
                primary && primary.Machine()==IMAGE_FILE_MACHINE_AMD64 ? &primary : nullptr,
                companion ? &companion : primary && primary.Machine()==IMAGE_FILE_MACHINE_I386 ? &primary : nullptr);
            if(primary_error) current.x64_error=primary_error;
            if(companion_error) current.x86_error=companion_error;
            auto next=dialog_restart::Scan(current,[&]{return !Current(command.generation);},restart_inventory.next_pid);
            if(!next.cancelled && Current(command.generation)) {
                restart_inventory=std::move(next);restart_checked_at=GetTickCount64();
            }
        }
        result.restart_inventory=restart_inventory;
        result.restart_checked=restart_checked_at!=0;
    }
    void ObserveHistory(const Command& command,DialogManagementResult& result) {
        ObserveRestart(command,result);
        if(!history_loaded) {
            history_load_error=dialog_history::LoadHistory(root.wstring(),history);
            if(history_load_error==ERROR_FILE_NOT_FOUND || history_load_error==ERROR_PATH_NOT_FOUND) history_load_error=0;
            history_loaded=true;
        }
        // No PID-only historical joins. The collector requires producer identity.
        // New managed events also use the bounded global runtime log.
        auto next=history;
        const auto data=GetPulseDataDir();
        const auto observed=dialog_history::CollectTakeoverHistory(data.empty()?std::wstring{}:(fs::path(data)/L"Diagnostics"/L"Runtime").wstring(),{},next,
            [&]{return !Current(command.generation);});
        result.history_error=history_load_error ? history_load_error : observed.error;
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
        if(command.operation==DialogManagementOperation::PollHistory) {ObserveHistory(command,result);return result;}
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
            if(!result.admin_launch_ready) result.error=result.admin_launch_error;
            else {
                const auto observed=dialog_launcher::ScanExistingTarget(configuration.application,[&]{return !Current(command.generation);});
                result.launch_preflight_checked=true;result.existing_target_count=observed.existing_target_count;
                result.launch_scan_complete=observed.scan_complete;result.launch_query_errors=observed.query_errors;
                if(observed.cancelled) result.error=ERROR_OPERATION_ABORTED;
            }
        }
        if(command.operation==DialogManagementOperation::LaunchAdmin) {
            if(!result.admin_launch_ready) result.error=result.admin_launch_error;
            else result.error=StartAdmin(command,launch_bundle,result.launcher_started);
        }
        if(command.operation==DialogManagementOperation::Refresh || command.operation==DialogManagementOperation::ExportDiagnostics) ObserveHistory(command,result);
        else {result.history=history;result.history_error=history_load_error;}
        if(command.operation==DialogManagementOperation::ExportDiagnostics && Current(command.generation)) {
            if(!requested_root.empty() || !configuration.application.empty()) Sessions(result);
            const auto nonce=Unique();
            if(nonce.empty()) { result.error=ERROR_GEN_FAILURE; return result; }
            const auto path=root/(L"diagnostics-"+nonce+L".txt");
            std::string report="Pulse dialog integration diagnostics v2\r\nScope: status only; read-only process metadata and sanitized event excerpts; no raw logs, paths, filenames or file contents.\r\n"
                "Process snapshots are not dialog requests, activation proof or a coverage denominator. Registration status is the collector context only.\r\n"
                "auto_enabled="+std::to_string(result.auto_enabled)+"\r\nauto_conflict="+std::to_string(result.auto_conflict)
                +"\r\nauto_error="+std::to_string(result.auto_error)+"\r\nauto_rollback_error="+std::to_string(result.auto_rollback_error)+"\r\nbundle_verified="+std::to_string(result.bundle_verified)
                +"\r\nbundle_error="+std::to_string(result.bundle_error)
                +"\r\nhistory_records="+std::to_string(result.history.records.size())
                +"\r\nhistory_unattributed="+std::to_string(result.history.unattributed_events)
                +"\r\nhistory_partial="+std::to_string(result.history.partial)
                +"\r\nhistory_error="+std::to_string(result.history_error)
                +"\r\nrestart_checked="+std::to_string(result.restart_checked)
                +"\r\nrestart_applications="+std::to_string(result.restart_inventory.records.size())
                +"\r\nrestart_scan_complete="+std::to_string(result.restart_inventory.scan_complete)
                +"\r\nrestart_query_errors="+std::to_string(result.restart_inventory.query_errors)
                +"\r\nselected_application_machine="+std::to_string(result.application_machine)
                +"\r\nmanaged_launch_bundle_verified="+std::to_string(result.launch_bundle_verified)
                +"\r\nmanaged_launch_bundle_error="+std::to_string(result.launch_bundle_error)
                +"\r\nadmin_launch_ready="+std::to_string(result.admin_launch_ready)
                +"\r\nadmin_launch_error="+std::to_string(result.admin_launch_error)+"\r\n"
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
            report+=dialog_diagnostics::CollectDialogLogExcerpt(data.empty()?std::wstring{}:(fs::path(data)/L"Diagnostics"/L"Runtime").wstring(),[&]{return !Current(command.generation);});
            unsigned collected=0, unavailable=0; DWORD source_error=0; bool source_limit=false;
            for(const auto& session:result.sessions) {
                if(!Current(command.generation)) break;
                if(session.status_error) {++unavailable;source_error=session.status_error;continue;}
                if(collected==4) {source_limit=true;break;}
                ++collected;
                report+=dialog_diagnostics::CollectDialogLogExcerpt(session.directory,[&]{return !Current(command.generation);});
            }
            report+="{\"schema\":1,\"event\":\"managed_log_sources\",\"data\":{\"collected\":"+std::to_string(collected)+",\"unavailable_sources\":"+std::to_string(unavailable)
                +",\"last_status_error\":"+std::to_string(source_error)+",\"limit_reached\":"+(source_limit?"true":"false")+",\"no_entry_inference\":true}}\n";
            if(!Current(command.generation)){result.error=ERROR_CANCELLED;return result;}
            CurrentUserSecurityAttributes security;
            if(!security) { result.error=ERROR_ACCESS_DENIED; return result; }
            Handle file{CreateFileW(path.c_str(),GENERIC_WRITE,0,security.get(),CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr)};
            if(file.h==INVALID_HANDLE_VALUE) {result.error=GetLastError();return result;}
            DWORD written=0;
            if(!WriteFile(file.h,report.data(),static_cast<DWORD>(report.size()),&written,nullptr) || written!=report.size() || !FlushFileBuffers(file.h)) result.error=ERROR_WRITE_FAULT;
            else result.diagnostics_path=path.wstring();
            CloseHandle(file.h);file.h=nullptr;
            if(result.error || !Current(command.generation)) {DeleteFileW(path.c_str());result.diagnostics_path.clear();}
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
uint64_t DialogManagementController::Submit(DialogManagementOperation operation,std::wstring path,std::string digest){
    std::lock_guard lock(state_->mutex);const auto generation=++state_->generation;
    state_->commands.clear();state_->results.clear();state_->commands.push_back({operation,generation,std::move(path),std::move(digest)});state_->cv.notify_one();return generation;
}
uint64_t DialogManagementController::SelectBundle(std::wstring path,std::string digest){return Submit(DialogManagementOperation::SelectBundle,std::move(path),std::move(digest));}
uint64_t DialogManagementController::SelectApplication(std::wstring path){return Submit(DialogManagementOperation::SelectApplication,std::move(path));}
uint64_t DialogManagementController::PrepareLaunch(){return Submit(DialogManagementOperation::PrepareLaunch);}
uint64_t DialogManagementController::Launch(){return Submit(DialogManagementOperation::Launch);}
uint64_t DialogManagementController::PrepareAdminLaunch(){return Submit(DialogManagementOperation::PrepareAdminLaunch);}
uint64_t DialogManagementController::LaunchAdmin(){return Submit(DialogManagementOperation::LaunchAdmin);}
uint64_t DialogManagementController::EnableAuto(){return Submit(DialogManagementOperation::EnableAuto);}
uint64_t DialogManagementController::DisableAuto(){return Submit(DialogManagementOperation::DisableAuto);}
uint64_t DialogManagementController::ExportDiagnostics(){return Submit(DialogManagementOperation::ExportDiagnostics);}
uint64_t DialogManagementController::PollHistory(bool refresh_running_applications){return Submit(DialogManagementOperation::PollHistory,refresh_running_applications ? L"refresh-running" : L"");}
uint64_t DialogManagementController::Refresh(){return Submit(DialogManagementOperation::Refresh);}
uint64_t DialogManagementController::Disable(std::wstring path){return Submit(DialogManagementOperation::Disable,std::move(path));}
void DialogManagementController::Cancel() noexcept{std::lock_guard lock(state_->mutex);++state_->generation;state_->commands.clear();state_->results.clear();}
std::vector<DialogManagementResult> DialogManagementController::Drain(){std::lock_guard lock(state_->mutex);auto result=std::move(state_->results);state_->results.clear();return result;}
}

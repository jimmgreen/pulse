#include "../common/app_runtime_paths.h"
#include "dialog_management_controller.h"
#include "../common/dialog_auto_registration.h"
#include "../common/dialog_bundle.h"
#include "../common/dialog_session.h"
#include "../common/current_user_security.h"
#include "../dialog_launcher/managed_launch_profile.h"
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
DWORD ValidateApplication(std::wstring& path) {
    auto canonical=dialog_session::Canonical(path,false);WORD machine=0;
    if(canonical.empty() || !dialog_session::Machine(canonical,machine)) return ERROR_BAD_EXE_FORMAT;
#ifdef _WIN64
    if(machine!=IMAGE_FILE_MACHINE_AMD64) return ERROR_EXE_MACHINE_TYPE_MISMATCH;
#else
    if(machine!=IMAGE_FILE_MACHINE_I386) return ERROR_EXE_MACHINE_TYPE_MISMATCH;
#endif
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
    DialogManagementConfiguration configuration;
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
        if(!requested_root.empty()) {const DWORD error=ReadConfiguration(root/L"configuration.bin",configuration);if(error) return error;}
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
        auto application=configuration.application;
        result.application_valid=!application.empty() && ValidateApplication(application)==ERROR_SUCCESS;
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
                item.status_error=bundle_error?bundle_error:dialog_session::Inspect(item.directory,bundle.Launcher(),selected,status);
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
        const auto nonce=Unique();if(nonce.empty()) return ERROR_GEN_FAILURE;
        const auto operation=root/L"operations"/nonce,control=operation/L"control";
        CurrentUserSecurityAttributes security;if(!security) return ERROR_ACCESS_DENIED;
        if(!CreateDirectoryW(operation.c_str(),security.get()) || !CreateDirectoryW(control.c_str(),security.get())) return GetLastError();
        Profile profile;profile.format_version=2;profile.bundle_root=bundle.Directory();profile.manifest_digest=bundle.ManifestDigest();
        profile.host=bundle.Host();profile.runtime=bundle.Runtime();profile.target=configuration.application;
        profile.cwd=fs::path(profile.target).parent_path().wstring();profile.control_root=control.wstring();
        DWORD error=dialog_launcher::CaptureProfile(profile,bundle.Launcher());
        const auto profile_file=operation/L"launch.profile";
        if(!error) error=dialog_launcher::SaveProfile(profile_file.wstring(),profile);
        if(!error) error=dialog_bundle::RecheckDirectory(bundle);
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
    DialogManagementResult Execute(const Command& command) {
        DialogManagementResult result;result.operation=command.operation;result.generation=command.generation;
        if(command.operation==DialogManagementOperation::DisableAuto) result.error=dialog_auto::Disable();
        const auto inspected=dialog_auto::Inspect();
        result.auto_enabled=inspected.enabled;result.auto_conflict=inspected.conflict;
        result.auto_error=inspected.error;result.auto_dll=inspected.dll_path;
        if(command.operation==DialogManagementOperation::DisableAuto) return result;
        result.error=Initialize();if(result.error) return result;
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
        if(command.operation==DialogManagementOperation::EnableAuto) {
            result.error=result.bundle_verified ? dialog_bundle::RecheckDirectory(bundle) : result.bundle_error;
            // Legacy root bundles remain inspectable/launchable explicitly, but
            // automatic registration must never pin the main application's exe.
            if(!result.error && _wcsicmp(bundle.Directory().c_str(),install_root.c_str())==0)
                result.error=ERROR_NOT_SUPPORTED;
            if(!result.error && Current(command.generation)) result.error=dialog_auto::Enable((fs::path(bundle.Directory())/dialog_auto::DllName).wstring());
            const auto status=dialog_auto::Inspect();
            result.auto_enabled=status.enabled;result.auto_conflict=status.conflict;
            result.auto_error=status.error;result.auto_dll=status.dll_path;
            result.auto_path_mismatch=(installed_default && result.auto_enabled && !result.bundle_verified) ||
                DialogAutoPathMismatch(result.auto_enabled,result.auto_dll,result.configuration.bundle_root);
        }
        if(command.operation==DialogManagementOperation::Launch) {
            if(!result.bundle_verified || !result.application_valid) result.error=ERROR_INVALID_DATA;
            else result.error=Start(command,bundle,result.launcher_started);
        }
        if(command.operation==DialogManagementOperation::ExportDiagnostics && Current(command.generation)) {
            const auto nonce=Unique();
            if(nonce.empty()) { result.error=ERROR_GEN_FAILURE; return result; }
            const auto path=root/(L"diagnostics-"+nonce+L".txt");
            const std::string report="Pulse dialog integration diagnostics v1\r\nScope: status only; no raw logs, paths, filenames or file contents.\r\n"
                "auto_enabled="+std::to_string(result.auto_enabled)+"\r\nauto_conflict="+std::to_string(result.auto_conflict)
                +"\r\nauto_error="+std::to_string(result.auto_error)+"\r\nbundle_verified="+std::to_string(result.bundle_verified)
                +"\r\nbundle_error="+std::to_string(result.bundle_error)+"\r\n";
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
        if(Current(command.generation) && (!requested_root.empty() || !configuration.application.empty())) Sessions(result,command.operation==DialogManagementOperation::Disable?command.path:std::wstring{});
        return result;
    }
    void Run() noexcept {
        for(;;) {
            Command command;
            {std::unique_lock lock(mutex);cv.wait(lock,[&]{return stopping || !commands.empty();});if(stopping) return;command=std::move(commands.front());commands.pop_front();}
            DialogManagementResult result;
            try{result=Execute(command);}catch(...){result.operation=command.operation;result.generation=command.generation;result.error=ERROR_GEN_FAILURE;}
            try {
                std::lock_guard lock(mutex);if(stopping || generation!=command.generation) continue;
                results.clear();results.push_back(std::move(result));if(host && wake) PostMessageW(host,wake,0,0);
            } catch(...) {}
        }
    }
};
DialogManagementController::DialogManagementController(HWND host,UINT wake,std::wstring root):state_(std::make_shared<State>()){
    state_->host=host;state_->wake=wake;state_->requested_root=std::move(root);
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
uint64_t DialogManagementController::Launch(){return Submit(DialogManagementOperation::Launch);}
uint64_t DialogManagementController::EnableAuto(){return Submit(DialogManagementOperation::EnableAuto);}
uint64_t DialogManagementController::DisableAuto(){return Submit(DialogManagementOperation::DisableAuto);}
uint64_t DialogManagementController::ExportDiagnostics(){return Submit(DialogManagementOperation::ExportDiagnostics);}
uint64_t DialogManagementController::Refresh(){return Submit(DialogManagementOperation::Refresh);}
uint64_t DialogManagementController::Disable(std::wstring path){return Submit(DialogManagementOperation::Disable,std::move(path));}
void DialogManagementController::Cancel() noexcept{std::lock_guard lock(state_->mutex);++state_->generation;state_->commands.clear();state_->results.clear();}
std::vector<DialogManagementResult> DialogManagementController::Drain(){std::lock_guard lock(state_->mutex);auto result=std::move(state_->results);state_->results.clear();return result;}
}

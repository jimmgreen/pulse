#include "admin_auto_attach.h"
#include "../common/dialog_admin_trust.h"
#include "../common/dialog_auto_binding.h"
#include "../common/dialog_auto_registration.h"
#include "../common/dialog_bundle.h"
#include "../shell_bridge/runtime_auto_entry.h"
#include "../shell_bridge/dialog_application_identity.h"
#include <map>
#include <objbase.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
#include <set>
#include <vector>
#include <utility>

namespace pulse::dialog_launcher::admin_auto {
namespace {
namespace fs=std::filesystem;
namespace cfg=pulse::shell_bridge::runtime;
namespace binding=pulse::dialog_auto_binding;
#if defined(_WIN64)
constexpr WORD kMachine=IMAGE_FILE_MACHINE_AMD64;
constexpr wchar_t kRuntime[]=L"PulseFileDialogAutoRuntime64.dll";
#else
constexpr WORD kMachine=IMAGE_FILE_MACHINE_I386;
constexpr wchar_t kRuntime[]=L"PulseFileDialogAutoRuntime32.dll";
#endif
struct Handle {
    HANDLE value=nullptr;
    Handle()=default;
    explicit Handle(HANDLE h):value(h){}
    ~Handle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    Handle(Handle&& other) noexcept:value(std::exchange(other.value,nullptr)){}
    Handle& operator=(Handle&& other) noexcept {if(this!=&other){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);value=std::exchange(other.value,nullptr);}return *this;}
    Handle(const Handle&)=delete;
    explicit operator bool()const{return value&&value!=INVALID_HANDLE_VALUE;}
};
struct Module { HMODULE value=nullptr;~Module(){if(value)FreeLibrary(value);} };
DWORD Last(){const DWORD error=GetLastError();return error?error:ERROR_GEN_FAILURE;}
std::wstring Plain(std::wstring path){if(path.starts_with(L"\\\\?\\"))path.erase(0,4);return path;}
bool Equal(const std::wstring&a,const std::wstring&b){return _wcsicmp(Plain(a).c_str(),Plain(b).c_str())==0;}
bool Within(const std::wstring&path,const std::wstring&root){const auto p=Plain(path),r=Plain(root);return p.size()>r.size()&&_wcsnicmp(p.c_str(),r.c_str(),r.size())==0&&(p[r.size()]==L'\\'||p[r.size()]==L'/');}
std::wstring Image(HANDLE process){std::wstring result(32768,L'\0');DWORD units=static_cast<DWORD>(result.size());if(!QueryFullProcessImageNameW(process,0,result.data(),&units))return {};result.resize(units);return result;}
DWORD Machine(HANDLE process,WORD& value){
    // Resolved at run time like dialog_session.cpp: the Win8.1 build has no IsWow64Process2.
    using IsWow64Process2Fn=BOOL(WINAPI*)(HANDLE,USHORT*,USHORT*);
    const auto query=reinterpret_cast<IsWow64Process2Fn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"IsWow64Process2"));
    if(!query)return ERROR_NOT_SUPPORTED;
    USHORT process_machine=0,native=0;if(!query(process,&process_machine,&native))return Last();value=process_machine?process_machine:native;return 0;}
DWORD FileHash(const std::wstring& path,std::array<BYTE,32>& output){
    Handle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr));if(!file)return Last();
    BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return ERROR_GEN_FAILURE;
    struct Close {BCRYPT_ALG_HANDLE& algorithm;BCRYPT_HASH_HANDLE& hash;~Close(){if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);}} close{algorithm,hash};
    if(BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)<0)return ERROR_GEN_FAILURE;
    std::array<BYTE,65536> bytes{};DWORD count=0;
    for(;;){if(!ReadFile(file.value,bytes.data(),static_cast<DWORD>(bytes.size()),&count,nullptr))return Last();if(!count)break;if(BCryptHashData(hash,bytes.data(),count,0)<0)return ERROR_GEN_FAILURE;}
    return BCryptFinishHash(hash,output.data(),static_cast<ULONG>(output.size()),0)<0?ERROR_GEN_FAILURE:0;
}
DWORD WriteNew(const fs::path& path,const void* data,DWORD bytes){
    Handle file(CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));if(!file)return Last();DWORD written=0;
    if(!WriteFile(file.value,data,bytes,&written,nullptr)||written!=bytes||!FlushFileBuffers(file.value))return Last();return 0;
}
DWORD ReadExact(const fs::path& path,void* data,DWORD bytes){
    Handle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));if(!file)return Last();
    BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};DWORD read=0;
    if(!GetFileInformationByHandle(file.value,&info)||!GetFileSizeEx(file.value,&size))return Last();
    if((info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)||size.QuadPart!=bytes)return ERROR_INVALID_DATA;
    return ReadFile(file.value,data,bytes,&read,nullptr)&&read==bytes?0:Last();
}
DWORD StatusWrite(const fs::path& root,const HelperStatusV1& status){
    const auto destination=root/StatusBasename(status.session_id,static_cast<WORD>(status.machine));
    if(fs::exists(destination)){if(const DWORD error=dialog_admin::VerifyProtectedPath(destination.wstring(),false))return error;}
    const auto temporary=root/(StatusBasename(status.session_id,static_cast<WORD>(status.machine))+L"."+std::to_wstring(status.pid)+L".tmp");
    DWORD error=WriteNew(temporary,&status,sizeof(status));if(error)return error;
    if(!MoveFileExW(temporary.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){error=Last();DeleteFileW(temporary.c_str());}return error;
}
struct TargetModule {uintptr_t base=0;DWORD size=0;std::wstring path;};
DWORD Modules(DWORD pid,std::vector<TargetModule>& result){
    result.clear();Handle snapshot;
    for(unsigned retry=0;retry!=3;++retry){snapshot=Handle(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid));if(snapshot)break;if(GetLastError()!=ERROR_BAD_LENGTH)return Last();}
    if(!snapshot)return Last();MODULEENTRY32W entry{};entry.dwSize=sizeof(entry);
    if(!Module32FirstW(snapshot.value,&entry))return Last();
    do{if(result.size()>=2048)return ERROR_TOO_MANY_NAMES;result.push_back({reinterpret_cast<uintptr_t>(entry.modBaseAddr),entry.modBaseSize,entry.szExePath});}while(Module32NextW(snapshot.value,&entry));
    return GetLastError()==ERROR_NO_MORE_FILES?0:Last();
}
DWORD RemoteAddress(DWORD pid,const void* local,uintptr_t& address){
    HMODULE module=nullptr;if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(local),&module))return Last();
    Module keep{module};wchar_t path[32768]{};if(!GetModuleFileNameW(module,path,32768))return Last();
    const auto offset=reinterpret_cast<uintptr_t>(local)-reinterpret_cast<uintptr_t>(module);
    std::vector<TargetModule> modules;if(const DWORD error=Modules(pid,modules))return error;
    for(const auto& target:modules)if(Equal(target.path,path)){if(offset>=target.size)return ERROR_BAD_EXE_FORMAT;address=target.base+offset;return 0;}
    return ERROR_MOD_NOT_FOUND;
}
struct Work {
    Handle process,thread;
    DWORD pid=0;FILETIME created{};GUID nonce{};
    fs::path control;
    void* remote=nullptr;ULONGLONG deadline=0;
    enum class Phase {Load,Attach,Ready,Done} phase=Phase::Done;
    bool timeout_logged=false;
    // A live remote thread may still own this buffer. Never free underneath it.
    ~Work(){if(remote&&(!thread||WaitForSingleObject(thread.value,0)==WAIT_OBJECT_0))VirtualFreeEx(process.value,remote,0,MEM_RELEASE);}
};
DWORD StartRemote(Work& work,uintptr_t address,const void* data,size_t size){
    if(!address||!size||size>1024*1024)return ERROR_INVALID_PARAMETER;
    work.remote=VirtualAllocEx(work.process.value,nullptr,size,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);if(!work.remote)return Last();
    SIZE_T written=0;if(!WriteProcessMemory(work.process.value,work.remote,data,size,&written)||written!=size){const DWORD error=Last();VirtualFreeEx(work.process.value,work.remote,0,MEM_RELEASE);work.remote=nullptr;return error;}
    work.thread=Handle(CreateRemoteThread(work.process.value,nullptr,0,reinterpret_cast<LPTHREAD_START_ROUTINE>(address),work.remote,0,nullptr));
    if(!work.thread){const DWORD error=Last();VirtualFreeEx(work.process.value,work.remote,0,MEM_RELEASE);work.remote=nullptr;return error;}
    work.deadline=GetTickCount64()+10000;work.timeout_logged=false;return 0;
}
void Disable(const fs::path& directory){const BYTE value=1;(void)WriteNew(directory/cfg::kDisableFile,&value,1);}
using Key=std::pair<DWORD,uint64_t>;
Key Identity(DWORD pid,FILETIME time){return {pid,(static_cast<uint64_t>(time.dwHighDateTime)<<32)|time.dwLowDateTime};}
struct Engine {
    const Options& options;
    dialog_bundle::BundleLease bundle;
    Module module;
    fs::path control_root;
    std::wstring runtime,system_root;
    uintptr_t attach_offset=0;
    uint64_t generation=0;
    std::vector<std::unique_ptr<Work>> active;
    std::vector<fs::path> sessions;
    std::set<Key> attempted;
    std::map<DWORD,std::pair<FILETIME,std::wstring>> identities;
    explicit Engine(const Options& input):options(input){}
    void Log(const char* stage,DWORD pid=0,DWORD error=0,DWORD entries=0,FILETIME created={})const noexcept {try{if(options.log){Event event{stage,pid,error,entries,created,0,{}};const auto found=identities.find(pid);if(found!=identities.end()){event.created=found->second.first;event.app_basename=pulse::shell_bridge::application_identity::ExecutableBasename(found->second.second);event.app_instance_id=pulse::shell_bridge::application_identity::ImageHash(found->second.second);}options.log(event);}}catch(...) {}}
    bool Enabled()const {return dialog_auto::IsEnabledFor(bundle.Directory()+L"\\"+dialog_auto::DllName);}
    bool Stopped()const {return options.stop&&WaitForSingleObject(options.stop,0)!=WAIT_TIMEOUT;}
    DWORD Begin(DWORD pid){
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid));if(!process)return Last();
        FILETIME created{},exit{},kernel{},user{};if(!GetProcessTimes(process.value,&created,&exit,&kernel,&user))return Last();
        const auto key=Identity(pid,created);if(attempted.contains(key))return ERROR_ALREADY_EXISTS;
        // Fixed upper bound prevents unbounded memory growth on a hostile GUI churn.
        if(attempted.size()>=8192)return ERROR_TOO_MANY_NAMES;attempted.insert(key);
        Log("discovered",pid,0,0,created);
        GUID nonce{};if(FAILED(CoCreateGuid(&nonce)))return ERROR_GEN_FAILURE;
        binding::BindingV1 bound{};DWORD error=binding::CaptureProcessWithHelper(process.value,GetCurrentProcess(),generation,nonce,bound);
        if(error){Log("identity_rejected",pid,error,0,created);return error;}
        WORD machine=0;if((error=Machine(process.value,machine))||machine!=kMachine){Log("architecture_rejected",pid,error?error:ERROR_BAD_EXE_FORMAT);return error?error:ERROR_BAD_EXE_FORMAT;}
        PROCESS_PROTECTION_LEVEL_INFORMATION protection{};
        if(!GetProcessInformation(process.value,ProcessProtectionLevelInfo,&protection,sizeof(protection))){error=Last();Log("protection_query_failed",pid,error);return error;}
        if(protection.ProtectionLevel!=PROTECTION_LEVEL_NONE){Log("protected_process_rejected",pid,ERROR_ACCESS_DENIED);return ERROR_ACCESS_DENIED;}
        const auto image=Image(process.value);if(image.empty())return Last();identities[pid]={created,image};
        if(pid==GetCurrentProcessId()||Within(image,options.installed_root)||Within(image,system_root)){Log("excluded_image",pid);return ERROR_NOT_SUPPORTED;}
        if(options.probe_only){Log("probe_eligible_no_attach",pid);return 0;}
        Handle privileged(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_QUERY_INFORMATION|PROCESS_VM_READ|PROCESS_VM_WRITE|PROCESS_VM_OPERATION|PROCESS_CREATE_THREAD|SYNCHRONIZE,FALSE,pid));
        if(!privileged){error=Last();Log("target_access_failed",pid,error);return error;}
        FILETIME reopened{};if(!GetProcessTimes(privileged.value,&reopened,&exit,&kernel,&user)||CompareFileTime(&reopened,&created)!=0){Log("target_reused",pid,ERROR_INVALID_DATA);return ERROR_INVALID_DATA;}
        process=std::move(privileged);
        std::vector<TargetModule> modules;if((error=Modules(pid,modules)))return error;
        for(const auto& item:modules){const auto name=fs::path(item.path).filename().wstring();if(name.starts_with(L"PulseFileDialogRuntime")||name.starts_with(L"PulseFileDialogAutoRuntime")){Log("existing_runtime_native",pid);return ERROR_ALREADY_INITIALIZED;}}
        if((error=dialog_bundle::RecheckDirectory(bundle)))return error;
        if(!Enabled()||Stopped())return ERROR_CANCELLED;
        auto work=std::make_unique<Work>();work->process=std::move(process);work->pid=pid;work->created=created;work->nonce=nonce;
        wchar_t nonce_text[40]{};StringFromGUID2(nonce,nonce_text,40);work->control=control_root/nonce_text;
        if(!CreateDirectoryW(work->control.c_str(),nullptr))return Last();
        if((error=dialog_admin::VerifyProtectedPath(work->control.wstring(),true)))return error;
        auto session=std::make_unique<cfg::Session>();session->reserved=cfg::kAdminSession;session->target_pid=pid;session->target_created=created;session->machine=kMachine;
        wcscpy_s(session->nonce,nonce_text);wcscpy_s(session->host_path,bundle.Host().c_str());wcscpy_s(session->target_path,image.c_str());wcscpy_s(session->control_path,work->control.c_str());wcscpy_s(session->launcher_path,bundle.Launcher().c_str());
        if((error=WriteNew(work->control/cfg::kSessionFile,session.get(),sizeof(*session)))||(error=WriteNew(work->control/L"auto-binding.bin",&bound,sizeof(bound)))){Disable(work->control);return error;}
        sessions.push_back(work->control);Log("binding_published",pid,0,0,created);
        uintptr_t load=0;if((error=RemoteAddress(pid,reinterpret_cast<const void*>(&LoadLibraryW),load))){Disable(work->control);return error;}
        if((error=StartRemote(*work,load,runtime.c_str(),(runtime.size()+1)*sizeof(wchar_t)))){Disable(work->control);return error;}
        work->phase=Work::Phase::Load;active.push_back(std::move(work));Log("load_queued",pid);return 0;
    }
    void Tick(){
        for(auto& pointer:active){auto& work=*pointer;if(work.phase==Work::Phase::Done)continue;
            if(WaitForSingleObject(work.process.value,0)!=WAIT_TIMEOUT){Log("target_exited",work.pid);work.phase=Work::Phase::Done;continue;}
            if(work.phase==Work::Phase::Ready){cfg::Ready ready{};const DWORD error=ReadExact(work.control/cfg::kReadyFile,&ready,sizeof(ready));
                if(!error){wchar_t nonce[40]{};StringFromGUID2(work.nonce,nonce,40);
                    const bool identity=ready.magic==cfg::kReadyMagic&&ready.version==cfg::kVersion&&ready.size==sizeof(ready)&&!ready.reserved&&ready.target_pid==work.pid&&ready.machine==kMachine&&CompareFileTime(&ready.target_created,&work.created)==0&&wcsncmp(ready.nonce,nonce,40)==0;
                    const bool accepted=identity&&ready.status==S_OK&&ready.installed_entries;
                    Log(accepted?"ready":"ready_rejected",work.pid,accepted?0:ERROR_INVALID_DATA,ready.installed_entries,work.created);if(!accepted)Disable(work.control);work.phase=Work::Phase::Done;
                }else if(error!=ERROR_FILE_NOT_FOUND||GetTickCount64()>work.deadline){Log("ready_failed",work.pid,error==ERROR_FILE_NOT_FOUND?ERROR_TIMEOUT:error);Disable(work.control);work.phase=Work::Phase::Done;}continue;}
            const DWORD waited=WaitForSingleObject(work.thread.value,0);
            if(waited==WAIT_TIMEOUT){if(GetTickCount64()>work.deadline&&!work.timeout_logged){work.timeout_logged=true;Disable(work.control);Log("remote_thread_timeout_retained",work.pid,ERROR_TIMEOUT);}continue;}
            if(waited!=WAIT_OBJECT_0){Disable(work.control);Log("remote_thread_wait_failed",work.pid,Last());work.phase=Work::Phase::Done;continue;}
            DWORD result=0;GetExitCodeThread(work.thread.value,&result);work.thread=Handle();if(work.remote){VirtualFreeEx(work.process.value,work.remote,0,MEM_RELEASE);work.remote=nullptr;}
            if(work.timeout_logged){work.phase=Work::Phase::Done;continue;}
            if(work.phase==Work::Phase::Attach){if(static_cast<HRESULT>(result)!=S_OK){Log("attach_rejected",work.pid,result);Disable(work.control);work.phase=Work::Phase::Done;}else{Log("attach_submitted",work.pid);work.phase=Work::Phase::Ready;work.deadline=GetTickCount64()+15000;}continue;}
            std::vector<TargetModule> modules;DWORD error=Modules(work.pid,modules);uintptr_t address=0;
            if(!error)for(const auto& loaded:modules)if(Equal(loaded.path,runtime)&&attach_offset<loaded.size){address=loaded.base+attach_offset;break;}
            if(!address){Log("loaded_module_missing",work.pid,error?error:ERROR_MOD_NOT_FOUND);Disable(work.control);work.phase=Work::Phase::Done;continue;}
            auto request=std::make_unique<cfg::PulseRuntimeAutoStartV1>();StringFromGUID2(work.nonce,request->nonce,40);wcscpy_s(request->host,bundle.Host().c_str());wcscpy_s(request->control,work.control.c_str());
            if(!Enabled()||Stopped()){Disable(work.control);work.phase=Work::Phase::Done;continue;}
            error=StartRemote(work,address,request.get(),sizeof(*request));if(error){Log("attach_thread_failed",work.pid,error);Disable(work.control);work.phase=Work::Phase::Done;}else work.phase=Work::Phase::Attach;
        }
        std::erase_if(active,[](const auto& work){return work->phase==Work::Phase::Done;});
    }
    ~Engine(){for(const auto& session:sessions)Disable(session);}
};
}
DWORD CheckEligibility(const Eligibility& value) noexcept {
    if(!value.same_user_session||!value.elevated||!value.unrestricted||!value.unprotected_process)return ERROR_ACCESS_DENIED;
    if(!value.same_architecture)return ERROR_BAD_EXE_FORMAT;
    return value.excluded?ERROR_NOT_SUPPORTED:ERROR_SUCCESS;
}
std::wstring StatusBasename(DWORD session_id,WORD machine){return L"auto-helper-"+std::to_wstring(session_id)+L"-"+std::to_wstring(machine)+L".bin";}
DWORD Run(const Options& options) noexcept {
    try {
        Engine engine(options);DWORD error=dialog_bundle::ResolveInstalledBundleForMachine(options.installed_root,kMachine,engine.bundle);if(error){engine.Log("bundle_rejected",0,error);return error;}
        if((error=dialog_admin::VerifyAdminLaunchLocation(options.installed_root,engine.bundle.Directory(),engine.bundle.Host(),kMachine)))return error;
        std::wstring control;if((error=dialog_admin::AdminControlRoot(options.installed_root,true,control)))return error;engine.control_root=control;
        wchar_t own[32768]{},windows[32768]{};if(!GetModuleFileNameW(nullptr,own,32768)||!GetWindowsDirectoryW(windows,32768))return Last();engine.system_root=windows;
        std::array<BYTE,32> self_hash{},bundle_hash{};
        if((error=dialog_admin::VerifyProtectedPath(own,false))||(error=FileHash(own,self_hash))||(error=FileHash(engine.bundle.AdminAutoHelper(),bundle_hash)))return error;
        if(self_hash!=bundle_hash)return ERROR_REVISION_MISMATCH;
        GUID nonce{};if(FAILED(CoCreateGuid(&nonce)))return ERROR_GEN_FAILURE;
        FILETIME now{};GetSystemTimeAsFileTime(&now);engine.generation=(static_cast<uint64_t>(now.dwHighDateTime)<<32)|now.dwLowDateTime;
        binding::BindingV1 self{};if((error=binding::CaptureCurrentWithHelper(GetCurrentProcess(),engine.generation,nonce,self)))return error;
        LPWSTR sid=nullptr;if(!ConvertSidToStringSidW(self.target.user_sid,&sid))return Last();std::wstring sid_text=sid;LocalFree(sid);
        if(!Equal(own,engine.bundle.AdminAutoHelper()))return ERROR_REVISION_MISMATCH;
        Handle root_handle(CreateFileW(options.installed_root.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr));BY_HANDLE_FILE_INFORMATION root_info{};
        if(!root_handle||!GetFileInformationByHandle(root_handle.value,&root_info))return Last();
        const std::wstring discriminator=std::to_wstring(root_info.dwVolumeSerialNumber)+L"-"+std::to_wstring(root_info.nFileIndexHigh)+L"-"+std::to_wstring(root_info.nFileIndexLow);
        const auto mutex_name=L"Local\\Pulse.AdminAuto."+sid_text+L"."+std::to_wstring(self.target.session_id)+L"."+std::to_wstring(kMachine)+L"."+discriminator;
        Handle mutex(CreateMutexW(nullptr,FALSE,mutex_name.c_str()));if(!mutex)return Last();if(GetLastError()==ERROR_ALREADY_EXISTS)return ERROR_ALREADY_INITIALIZED;
        if(!engine.Enabled()){engine.Log("disabled_exit");return ERROR_CANCELLED;}
        engine.runtime=engine.bundle.AutoRuntime();
        if((error=dialog_admin::VerifyProtectedPath(engine.runtime,false)))return error;
        if(!options.probe_only){engine.module.value=LoadLibraryExW(engine.runtime.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);if(!engine.module.value)return Last();
        auto attach=GetProcAddress(engine.module.value,"PulseFileDialogRuntimeAutoAttach");if(!attach)return ERROR_PROC_NOT_FOUND;
        HMODULE owner=nullptr;if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(attach),&owner))return Last();Module export_owner{owner};if(owner!=engine.module.value)return ERROR_INVALID_DLL;
        engine.attach_offset=reinterpret_cast<uintptr_t>(attach)-reinterpret_cast<uintptr_t>(engine.module.value);}
        Handle companion_process,companion_job;
#if defined(_WIN64)
        if(!options.companion&&!options.probe_only){
            dialog_bundle::BundleLease companion;if((error=dialog_bundle::ResolveInstalledBundleForMachine(options.installed_root,IMAGE_FILE_MACHINE_I386,companion)))return error;
            if((error=dialog_admin::VerifyProtectedPath(companion.AdminAutoHelper(),false)))return error;
            companion_job=Handle(CreateJobObjectW(nullptr,nullptr));if(!companion_job)return Last();JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if(!SetInformationJobObject(companion_job.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits)))return Last();
            std::wstring command=L"\""+companion.AdminAutoHelper()+L"\" --installed-root \""+options.installed_root+L"\" --companion";
            STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION child{};
            if(!CreateProcessW(companion.AdminAutoHelper().c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_SUSPENDED|CREATE_NO_WINDOW,nullptr,options.installed_root.c_str(),&startup,&child))return Last();
            companion_process=Handle(child.hProcess);Handle thread(child.hThread);
            if(!AssignProcessToJobObject(companion_job.value,child.hProcess)||ResumeThread(child.hThread)==static_cast<DWORD>(-1)){error=Last();TerminateProcess(child.hProcess,error);return error;}
            engine.Log("companion_started",child.dwProcessId);
        }
#endif
        HelperStatusV1 status{};status.pid=GetCurrentProcessId();status.created=self.target.created;status.session_id=self.target.session_id;status.machine=kMachine;status.generation=engine.generation;status.ready=options.probe_only?0u:1u;status.error=options.probe_only?ERROR_NOT_SUPPORTED:0u;
        if(!options.probe_only&&(error=StatusWrite(engine.control_root,status)))return error;
        engine.Log("helper_ready",status.pid,0,kMachine,status.created);
        ULONGLONG next_scan=0,next_pointer_check=0;const ULONGLONG began=GetTickCount64();
        const auto pointer=fs::path(options.installed_root)/(kMachine==IMAGE_FILE_MACHINE_I386?L"dialog-x86-current.json":L"dialog-current.json");
        std::array<BYTE,32> pointer_hash{},observed_pointer{};if((error=FileHash(pointer.wstring(),pointer_hash)))return error;
        while(engine.Enabled()&&!engine.Stopped()){
            if(options.duration_ms&&GetTickCount64()-began>=options.duration_ms)break;
            if(GetTickCount64()>=next_pointer_check){next_pointer_check=GetTickCount64()+1000;if(FileHash(pointer.wstring(),observed_pointer)||observed_pointer!=pointer_hash){engine.Log("bundle_changed_exit");break;}}
            if(companion_process&&WaitForSingleObject(companion_process.value,0)==WAIT_OBJECT_0){DWORD code=0;GetExitCodeProcess(companion_process.value,&code);if(code!=ERROR_ALREADY_INITIALIZED){engine.Log("companion_exited",0,code);break;}companion_process=Handle();}
            engine.Tick();
            if(GetTickCount64()>=next_scan&&engine.active.size()<4){next_scan=GetTickCount64()+1000;std::set<DWORD> pids;
                EnumWindows([](HWND hwnd,LPARAM raw)->BOOL {if(IsWindowVisible(hwnd)){DWORD pid=0;GetWindowThreadProcessId(hwnd,&pid);if(pid)reinterpret_cast<std::set<DWORD>*>(raw)->insert(pid);}return TRUE;},reinterpret_cast<LPARAM>(&pids));
                for(DWORD pid:pids){if(pid==GetCurrentProcessId())continue;if(engine.active.size()>=4||engine.Stopped()||!engine.Enabled())break;const DWORD attached=engine.Begin(pid);if(attached&&attached!=ERROR_ALREADY_EXISTS&&attached!=ERROR_ACCESS_DENIED)engine.Log("candidate_skipped",pid,attached);}
            }
            if(options.stop)WaitForSingleObject(options.stop,100);else Sleep(100);
        }
        status.ready=0;status.error=ERROR_CANCELLED;if(!options.probe_only)(void)StatusWrite(engine.control_root,status);engine.Log("helper_stopping",status.pid);return 0;
    }catch(const std::bad_alloc&){return ERROR_NOT_ENOUGH_MEMORY;}catch(...){return ERROR_GEN_FAILURE;}
}
}

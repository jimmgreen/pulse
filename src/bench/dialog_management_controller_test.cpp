#include "../app/dialog_management_controller.h"
#include "../common/dialog_session.h"
#include "../common/dialog_bundle.h"
#include "../common/dialog_auto_registration.h"
#include <algorithm>
#include <objbase.h>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <thread>
#include <fstream>
using namespace pulse::app;
namespace fs=std::filesystem;
namespace cfg=pulse::shell_bridge::runtime;
namespace pulse::app { std::wstring GetPulseDataDir(){return {};} }
namespace {
int checks=0,failures=0;
void Check(bool ok,const char* text){++checks;if(!ok)++failures;std::printf("[%s] %s\n",ok?"PASS":"FAIL",text);std::fflush(stdout);}
std::wstring Unique(){
    GUID id{};wchar_t value[40]{};
    const HRESULT hr=CoCreateGuid(&id);
    if(FAILED(hr) || StringFromGUID2(id,value,40)!=39){
        std::printf("[FAIL] fixture GUID creation hr=%08lX\n",static_cast<unsigned long>(hr));
        std::fflush(stdout);std::exit(1);
    }
    return value;
}
bool DirectoryIdentity(const fs::path& path,BY_HANDLE_FILE_INFORMATION& identity){
    HANDLE h=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(h==INVALID_HANDLE_VALUE)return false;
    const bool ok=GetFileInformationByHandle(h,&identity) &&
        (identity.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) && !(identity.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT);
    CloseHandle(h);return ok;
}
bool SameDirectory(const BY_HANDLE_FILE_INFORMATION& a,const BY_HANDLE_FILE_INFORMATION& b){
    return a.dwVolumeSerialNumber==b.dwVolumeSerialNumber && a.nFileIndexHigh==b.nFileIndexHigh && a.nFileIndexLow==b.nFileIndexLow;
}
bool SafeCleanup(const fs::path& bench,const fs::path& root,const std::wstring& owned_name,
                 const BY_HANDLE_FILE_INFORMATION& bench_id,const BY_HANDLE_FILE_INFORMATION& root_id){
    auto failure=[](const char* stage,DWORD error,const fs::path& path){
        std::printf("CLEANUP stage=%s error=%lu path=%ls\n",stage,error,path.c_str());std::fflush(stdout);return false;
    };
    auto validate=[&](std::vector<fs::path>& paths){
        BY_HANDLE_FILE_INFORMATION current_bench{},current_root{};
        if(root.parent_path()!=bench || root.filename()!=owned_name)
            return failure("owned-name",ERROR_INVALID_DATA,root);
        if(!DirectoryIdentity(bench,current_bench) || !DirectoryIdentity(root,current_root))
            return failure("directory-identity-read",GetLastError(),root);
        if(!SameDirectory(bench_id,current_bench) || !SameDirectory(root_id,current_root))
            return failure("directory-identity-changed",ERROR_INVALID_DATA,root);
        std::error_code error;
        const auto canonical_bench=fs::canonical(bench,error);
        if(error || canonical_bench!=bench)return failure("bench-canonical",error?static_cast<DWORD>(error.value()):ERROR_INVALID_DATA,bench);
        const auto canonical_root=fs::canonical(root,error);
        if(error || canonical_root!=root)return failure("root-canonical",error?static_cast<DWORD>(error.value()):ERROR_INVALID_DATA,root);
        paths.clear();paths.push_back(root);
        fs::recursive_directory_iterator it(root,fs::directory_options::none,error),end;
        if(error)return failure("enumerate-start",static_cast<DWORD>(error.value()),root);
        for(;it!=end;it.increment(error)){
            if(error)return failure("enumerate-next",static_cast<DWORD>(error.value()),root);
            const DWORD attributes=GetFileAttributesW(it->path().c_str());
            if(attributes==INVALID_FILE_ATTRIBUTES)return failure("attributes",GetLastError(),it->path());
            if(attributes&FILE_ATTRIBUTE_REPARSE_POINT)return failure("reparse",ERROR_REPARSE_TAG_INVALID,it->path());
            paths.push_back(it->path());
        }
        return !error || failure("enumerate-end",static_cast<DWORD>(error.value()),root);
    };
    std::vector<fs::path> paths;
    const auto deadline=GetTickCount64()+5000;
    bool reported_wait=false;
    for(;;){
        if(!validate(paths))return false;
        DWORD blocked=ERROR_SUCCESS;fs::path blocked_path;
        for(const auto& path:paths){
            // No deletion here: DELETE access detects the launcher's still-held
            // non-share-delete directory lock after its application has exited.
            HANDLE probe=CreateFileW(path.c_str(),DELETE|FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
            if(probe==INVALID_HANDLE_VALUE){blocked=GetLastError();blocked_path=path;break;}
            CloseHandle(probe);
        }
        if(!blocked)break;
        if(blocked!=ERROR_SHARING_VIOLATION && blocked!=ERROR_LOCK_VIOLATION)
            return failure("delete-access-preflight",blocked,blocked_path);
        if(!reported_wait){failure("waiting-owned-lock-release",blocked,blocked_path);reported_wait=true;}
        if(GetTickCount64()>=deadline)return failure("owned-lock-release-timeout",blocked,blocked_path);
        Sleep(25);
    }
    // Release is not permission to trust the earlier scan: repeat every guard.
    if(!validate(paths))return false;
    if(reported_wait){std::puts("CLEANUP stage=owned-locks-released");std::fflush(stdout);}
    std::error_code error;fs::remove_all(root,error);
    return !error || failure("remove-owned-tree",static_cast<DWORD>(error.value()),root);
}
std::wstring Self(){wchar_t path[32768]{};GetModuleFileNameW(nullptr,path,32768);return path;}
std::optional<DialogManagementResult> Await(DialogManagementController& c,uint64_t generation){
    const auto until=GetTickCount64()+25000;
    while(GetTickCount64()<until){
        auto results=c.Drain();
        for(auto& result:results){Check(result.generation==generation,"only newest generation delivered");if(result.generation==generation)return std::move(result);}
        Sleep(10);
    }
    Check(false,"bounded controller result");return {};
}
bool Write(const fs::path& path,const void* data,DWORD size){
    HANDLE f=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f==INVALID_HANDLE_VALUE)return false;DWORD n=0;const bool ok=WriteFile(f,data,size,&n,nullptr)&&n==size;CloseHandle(f);return ok;
}
void SessionTests(const fs::path& root){
    const auto nonce=Unique();const auto directory=root/nonce;
    if(!CreateDirectoryW(directory.c_str(),nullptr)){Check(false,"exclusive session fixture creation");return;}
    auto s=std::make_unique<cfg::Session>();s->target_pid=GetCurrentProcessId();
#ifdef _WIN64
    s->machine=IMAGE_FILE_MACHINE_AMD64;
#else
    s->machine=IMAGE_FILE_MACHINE_I386;
#endif
    FILETIME exit{},kernel{},user{};GetProcessTimes(GetCurrentProcess(),&s->target_created,&exit,&kernel,&user);
    const auto self=pulse::dialog_session::Canonical(Self(),false),control=pulse::dialog_session::Canonical(directory,true);
    wcscpy_s(s->nonce,nonce.c_str());wcscpy_s(s->target_path,self.c_str());wcscpy_s(s->launcher_path,self.c_str());
    wcscpy_s(s->control_path,control.c_str());wcscpy_s(s->host_path,self.c_str());
    Check(Write(directory/cfg::kSessionFile,s.get(),static_cast<DWORD>(sizeof(*s))),"write only owned session fixture");
    cfg::Ready ready;ready.target_pid=s->target_pid;ready.target_created=s->target_created;ready.machine=s->machine;
    ready.status=S_OK;ready.installed_entries=1;wcscpy_s(ready.nonce,nonce.c_str());
    Check(Write(directory/cfg::kReadyFile,&ready,static_cast<DWORD>(sizeof(ready))),"write historical handshake fixture");
    pulse::dialog_session::Status status;
    const DWORD inspected=pulse::dialog_session::Inspect(directory.wstring(),Self(),false,status);
    Check(inspected==0 && status.process_alive && status.historical_handshake && !status.local_disabled,"live process and historical handshake are independent facts");
    Check(pulse::dialog_session::Inspect(directory.wstring(),Self(),true,status)==0 && status.local_disabled,"exact own session disable marker");
    Check(WaitForSingleObject(GetCurrentProcess(),0)==WAIT_TIMEOUT,"disable leaves application running");
    Check(pulse::dialog_session::Inspect(directory.wstring(),L"C:\\missing-launcher.exe",true,status)!=0,"wrong launcher identity cannot modify session");
    for (const uint32_t reserved : {uint32_t{cfg::kAdminSession}, uint32_t{0x80000000u}}) {
        const auto rejected=root/Unique();
        if(!CreateDirectoryW(rejected.c_str(),nullptr)){Check(false,"exclusive reserved session fixture creation");return;}
        wcscpy_s(s->nonce,rejected.filename().c_str());
        wcscpy_s(s->control_path,pulse::dialog_session::Canonical(rejected,true).c_str());
        s->reserved=reserved;
        Check(Write(rejected/cfg::kSessionFile,s.get(),static_cast<DWORD>(sizeof(*s))),"write reserved session in owned user directory");
        Check(pulse::dialog_session::Inspect(rejected.wstring(),Self(),false,status)!=0,"ordinary inspection rejects admin or unknown reserved session");
        Check(pulse::dialog_session::Inspect(rejected.wstring(),Self(),true,status)!=0 && !fs::exists(rejected/cfg::kDisableFile),"rejected reserved session creates no disable marker");
    }
    s->reserved=0;
    const auto other=root/Unique();
    if(!CreateDirectoryW(other.c_str(),nullptr)){Check(false,"exclusive second session fixture creation");return;}
    wcscpy_s(s->nonce,other.filename().c_str());wcscpy_s(s->control_path,pulse::dialog_session::Canonical(other,true).c_str());
    ++s->target_created.dwLowDateTime;
    Check(Write(other/cfg::kSessionFile,s.get(),static_cast<DWORD>(sizeof(*s))),"write mismatching creation-time fixture");
    Check(pulse::dialog_session::Inspect(other.wstring(),Self(),true,status)!=0 && !fs::exists(other/cfg::kDisableFile),"PID reuse mismatch cannot disable another process");
}
}
int wmain(int argc,wchar_t** argv){
    if (argc == 2 && std::wstring_view(argv[1]) == L"--auto-path") {
        const auto expected = (fs::path(L"C:\\fixture\\current") / pulse::dialog_auto::DllName).wstring();
        Check(!DialogAutoPathMismatch(true, expected, L"C:\\fixture\\current"), "same registered component is not a mismatch");
        Check(!DialogAutoPathMismatch(true, expected, L"c:\\fixture\\current\\."), "case and lexical path normalization preserve registration match");
        Check(DialogAutoPathMismatch(true, expected, L"C:\\fixture\\new"), "enabled component from another installation is a mismatch");
        Check(!DialogAutoPathMismatch(false, expected, L"C:\\fixture\\new"), "disabled registration is not an enabled-path mismatch");
        std::printf("Checks: %d, failures: %d\n", checks, failures);
        return failures ? 1 : 0;
    }
    wchar_t event_name[256]{};
    if(GetEnvironmentVariableW(L"PULSE_TEST_MANAGEMENT_CHILD",event_name,256)){
        HANDLE ready=OpenEventW(EVENT_MODIFY_STATE,FALSE,(std::wstring(event_name)+L".ready").c_str());
        HANDLE done=OpenEventW(SYNCHRONIZE,FALSE,event_name);
        if(ready)SetEvent(ready);
        const DWORD wait=done?WaitForSingleObject(done,30000):WAIT_FAILED;
        if(ready)CloseHandle(ready);if(done)CloseHandle(done);return wait==WAIT_OBJECT_0?0:2;
    }
    std::error_code canonical_error;
    const auto bench=fs::canonical(fs::path(L"bench_data"),canonical_error);
    BY_HANDLE_FILE_INFORMATION bench_id{},root_id{};
    if(canonical_error || !DirectoryIdentity(bench,bench_id)){Check(false,"canonical bench_data directory required");return 1;}
    const auto owned_name=L"dialog-management-"+Unique();const auto root=bench/owned_name;
    if(!CreateDirectoryW(root.c_str(),nullptr)){Check(false,"exclusive fixture root creation required");return 1;}
    if(!DirectoryIdentity(root,root_id)){Check(false,"new fixture root identity required");return 1;}
    if(argc==2 && wcscmp(argv[1],L"--session-only")==0){
        SessionTests(root);
        if(!failures)Check(SafeCleanup(bench,root,owned_name,bench_id,root_id),"owned session fixture cleanup");
        std::printf("Checks: %d, failures: %d\n",checks,failures);return failures?1:0;
    }
    HWND host=CreateWindowExW(0,L"STATIC",L"management fixture",0,0,0,0,0,HWND_MESSAGE,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(host!=nullptr,"private wake host");
    if(argc==2 && std::wstring_view(argv[1])==L"--installed-default") {
        pulse::dialog_bundle::BundleLease installed;
        const auto error=pulse::dialog_bundle::ResolveInstalledBundle(fs::path(Self()).parent_path().wstring(),installed);
        Check(error==0,"installed-default fixture has a verified pointer");
        const auto selected=installed.Directory();installed=pulse::dialog_bundle::BundleLease{};
        if(!error) {
            {
                DialogManagementController c(host,WM_APP+970,(root/L"state").wstring());
                auto result=Await(c,c.Refresh());
                Check(result && result->configuration.bundle_root==selected,"default refresh publishes resolved bundle");
                result=Await(c,c.SelectApplication(Self()));
                Check(result && !result->error && result->application_valid,"default application selection persists");
                std::ifstream input(root/L"state/configuration.bin",std::ios::binary);
                uint32_t magic=0,bundle_length=1;
                input.read(reinterpret_cast<char*>(&magic),sizeof(magic));
                input.read(reinterpret_cast<char*>(&bundle_length),sizeof(bundle_length));
                Check(input && magic==0x314D4450 && bundle_length==0,"default configuration does not pin resolved version as manual selection");
            }
            DialogManagementController c(host,WM_APP+970,(root/L"state").wstring());
            const auto result=Await(c,c.Refresh());
            Check(result && result->application_valid && result->configuration.bundle_root==selected,"default configuration restores application and resolves installed pointer again");
        }
        DestroyWindow(host);
        if(!failures) Check(SafeCleanup(bench,root,owned_name,bench_id,root_id),"owned installed-default fixture cleanup");
        std::printf("Checks: %d, failures: %d\n",checks,failures);return failures?1:0;
    }
    if(argc==3 && std::wstring_view(argv[1])==L"--auto-bundle") {
        const auto registry_name=L"Software\\PulseStartupTest_"+Unique();HKEY registry=nullptr;
        const LSTATUS create_error=RegCreateKeyExW(HKEY_CURRENT_USER,registry_name.c_str(),0,nullptr,0,KEY_ALL_ACCESS,nullptr,&registry,nullptr);
        if(create_error!=ERROR_SUCCESS) {
            std::printf("[FAIL] isolated registry fixture creation error=%ld\n",create_error);
            DestroyWindow(host);return 2;
        }
        const LSTATUS override_error=RegOverridePredefKey(HKEY_CURRENT_USER,registry);
        if(override_error!=ERROR_SUCCESS) {
            std::printf("[FAIL] isolated registry fixture override error=%ld\n",override_error);
            RegCloseKey(registry);RegDeleteTreeW(HKEY_CURRENT_USER,registry_name.c_str());
            DestroyWindow(host);return 2;
        }
        {
            DialogManagementController c(host,WM_APP+970,(root/L"state").wstring());
            auto result=Await(c,c.Refresh());
            pulse::dialog_bundle::BundleLease installed;
            const auto installed_error=pulse::dialog_bundle::ResolveInstalledBundle(fs::path(Self()).parent_path().wstring(),installed);
            Check(result && result->bundle_error==installed_error && result->configuration.bundle_root==installed.Directory(),"formal refresh resolves only the installed pointer snapshot");
            installed=pulse::dialog_bundle::BundleLease{};
            result=Await(c,c.SelectBundle(argv[2]));Check(result && !result->error && result->bundle_verified,"verified component fixture accepted");
            if(!result || result->error || !result->bundle_verified)
                std::printf("[DETAIL] selection result=%d error=%lu bundle_error=%lu\n",result.has_value(),
                    result ? result->error : ERROR_TIMEOUT,result ? result->bundle_error : ERROR_TIMEOUT);
            if(result && !result->error && result->bundle_verified) {
                {
                    DialogManagementController restored(host,WM_APP+970,(root/L"state").wstring());
                    const auto saved=Await(restored,restored.Refresh());
                    Check(saved && saved->bundle_verified && saved->configuration.bundle_root==result->configuration.bundle_root &&
                        saved->configuration.manifest_digest==result->configuration.manifest_digest,"explicit experimental bundle survives controller recreation");
                }
                result=Await(c,c.EnableAuto());Check(result && !result->error && result->auto_enabled,"controller enable reaches actual isolated COM registration");
                result=Await(c,c.Refresh());Check(result && result->auto_enabled,"refresh reports actual enabled registration");
                result=Await(c,c.DisableAuto());Check(result && !result->error && !result->auto_enabled,"controller disable restores isolated COM registration");
                result=Await(c,c.DisableAuto());Check(result && !result->error,"controller disable is idempotent");
                result=Await(c,c.ExportDiagnostics());Check(result && !result->error && fs::exists(result->diagnostics_path),"diagnostics exported after restoration");
            }
        }
        RegOverridePredefKey(HKEY_CURRENT_USER,nullptr);RegCloseKey(registry);RegDeleteTreeW(HKEY_CURRENT_USER,registry_name.c_str());
        DestroyWindow(host);
        if(!failures) Check(SafeCleanup(bench,root,owned_name,bench_id,root_id),"owned automatic-entry fixture cleanup");
        std::printf("Checks: %d, failures: %d\n",checks,failures);return failures?1:0;
    }
    SessionTests(root);
    {
        DialogManagementController c(host,WM_APP+970,(root/L"state").wstring());
        auto result=Await(c,c.SelectApplication(L"C:\\missing-application.exe"));
        Check(result && result->error && !result->application_valid,"failed selection returns error");
        result=Await(c,c.SelectApplication(Self()));
        Check(result && !result->error && result->application_valid,"selection retry validates same-architecture own fixture");
        result=Await(c,c.SelectBundle(L"C:\\missing-bundle"));
        Check(result && result->error && result->application_valid && !result->bundle_verified,"failed package selection retains valid application");
        result=Await(c,c.ExportDiagnostics());
        Check(result && !result->error && !result->diagnostics_path.empty() && fs::path(result->diagnostics_path).parent_path()==root/L"state","diagnostics exported only to isolated state directory");
        if(result && !result->diagnostics_path.empty()) {
            std::ifstream input{fs::path(result->diagnostics_path)};
            const std::string contents((std::istreambuf_iterator<char>(input)),{});
            Check(contents.find("Scope: status only")!=std::string::npos && contents.find("auto_enabled=")!=std::string::npos && contents.find("C:")==std::string::npos,"diagnostics contain status but no selected paths or raw logs");
        }
        c.Refresh();c.Cancel();Check(c.Drain().empty(),"cancel removes previous result generation");
        result=Await(c,c.Refresh());Check(result && result->application_valid,"refresh remains usable after cancellation");
        result=Await(c,c.Disable((root/L"unknown").wstring()));
        Check(result && result->error==ERROR_NOT_FOUND,"unknown session path cannot be disabled");
        result=Await(c,c.Launch());Check(result && result->error && !result->launcher_started,"invalid package never starts launcher");
    }
    {
        DialogManagementController c(host,WM_APP+970,(root/L"state").wstring());
        auto result=Await(c,c.Refresh());Check(result && result->application_valid,"configuration survives controller recreation");
    }
    if(argc==3 && std::wstring_view(argv[1])==L"--bundle"){
        const std::wstring event=L"Local\\Pulse.Management.Test."+Unique();
        HANDLE done=CreateEventW(nullptr,TRUE,FALSE,event.c_str()),ready=CreateEventW(nullptr,TRUE,FALSE,(event+L".ready").c_str());
        SetEnvironmentVariableW(L"PULSE_TEST_MANAGEMENT_CHILD",event.c_str());
        auto c=std::make_unique<DialogManagementController>(host,WM_APP+970,(root/L"state").wstring());
        const std::wstring raw_bundle=argv[2];
        DWORD raw_error=0;
        { pulse::dialog_bundle::BundleLease raw;raw_error=pulse::dialog_bundle::VerifyBundle(raw_bundle,{},raw); }
        // CLI accepts Windows slash spelling; only separators are normalized.
        // Absolute/no-dot/reparse/manifest checks remain in VerifyBundle unchanged.
        fs::path bundle_argument(raw_bundle);bundle_argument.make_preferred();
        std::printf("BUNDLE_INPUT raw_error=%lu raw_forward_slashes=%zu preferred_absolute=%d\n",
            raw_error,static_cast<size_t>(std::count(raw_bundle.begin(),raw_bundle.end(),L'/')),bundle_argument.is_absolute()?1:0);
        auto result=Await(*c,c->SelectBundle(bundle_argument.wstring()));
        std::printf("BUNDLE_RESULT received=%d error=%lu verified=%d application_valid=%d\n",
            result?1:0,result?result->error:ERROR_TIMEOUT,result&&result->bundle_verified?1:0,
            result&&result->application_valid?1:0);std::fflush(stdout);
        Check(result && !result->error && result->bundle_verified,"explicit fixture bundle verified");
        if(result && !result->error){
            c->Launch();
            Check(WaitForSingleObject(ready,20000)==WAIT_OBJECT_0,"owned managed application actually resumed");
            c->Cancel();Check(c->Drain().empty(),"late launch result discarded after Cancel");
            const auto receipt_deadline=GetTickCount64()+15000;
            do {
                result=Await(*c,c->Refresh());
                if(result && !result->sessions.empty() && result->sessions.front().historical_handshake) break;
                Sleep(20);
            } while(GetTickCount64()<receipt_deadline);
            Check(result && !result->sessions.empty(),"refresh rediscovers operation after cancelled launch observation");
            Check(result && !result->sessions.empty() && result->sessions.front().historical_handshake,
                "bounded observation waits for actual initialization receipt, never infers it from launch");
            if(result && !result->sessions.empty()){
                const auto session=result->sessions.front();
                HANDLE child=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,session.pid);
                Check(child && WaitForSingleObject(child,0)==WAIT_TIMEOUT,"cancel did not terminate resumed application");
                result=Await(*c,c->Disable(session.directory));
                Check(result && !result->error && result->sessions.front().local_disabled,"exact retained operation can be disabled");
                c.reset();
                Check(child && WaitForSingleObject(child,0)==WAIT_TIMEOUT,"controller destruction leaves application alive");
                SetEvent(done);Check(child && WaitForSingleObject(child,10000)==WAIT_OBJECT_0,"owned child exits only on its fixture signal");
                if(child)CloseHandle(child);
                DialogManagementController after(host,WM_APP+970,(root/L"state").wstring());
                result=Await(after,after.Refresh());
                Check(result && !result->sessions.empty() && !result->sessions.front().process_alive &&
                    result->sessions.front().historical_handshake,"exited application retains historical handshake without live claim");
            }
        }
        SetEvent(done);SetEnvironmentVariableW(L"PULSE_TEST_MANAGEMENT_CHILD",nullptr);
        CloseHandle(ready);CloseHandle(done);
    } else std::puts("[NOT RUN] --bundle <verified fixture bundle> required for managed launch/cancel/destruction case");
    DestroyWindow(host);
    // Keep the bounded fixture/report evidence on failure, including pending operations.
    if(!failures)Check(SafeCleanup(bench,root,owned_name,bench_id,root_id),"owned fixture cleanup");
    std::printf("Checks: %d, failures: %d\n",checks,failures);return failures?1:0;
}

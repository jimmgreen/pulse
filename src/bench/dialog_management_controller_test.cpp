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
namespace pulse::app { std::wstring diagnostics_test_data; std::wstring GetPulseDataDir(){return diagnostics_test_data;} }
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
    auto extended=[](const fs::path& path) {
        const auto value=path.wstring();
        return value.starts_with(LR"(\\?\)") ? path : fs::path(std::wstring(LR"(\\?\)")+value);
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
        paths.clear();paths.push_back(extended(root));
        fs::recursive_directory_iterator it(extended(root),fs::directory_options::none,error),end;
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
    std::error_code error;fs::remove_all(extended(root),error);
    return !error || failure("remove-owned-tree",static_cast<DWORD>(error.value()),root);
}
std::wstring Self(){wchar_t path[32768]{};GetModuleFileNameW(nullptr,path,32768);return path;}
std::optional<DialogManagementResult> Await(DialogManagementController& c,uint64_t generation,DWORD timeout=25000){
    const auto until=GetTickCount64()+timeout;
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
    {
        // Keep session.bin below MAX_PATH and the six-character-longer receipt
        // above it. Do not shorten the original failed live fixtures.
        const auto plain_root=fs::absolute(root).wstring();
        constexpr size_t directory_units=244;
        const size_t fixed=plain_root.size()+1+1+38;
        Check(fixed<directory_units,"long receipt fixture has room for a bounded padding component");
        if(fixed<directory_units) {
            const auto parent=root/std::wstring(directory_units-fixed,L'p');
            const auto long_directory=parent/Unique();
            fs::create_directories(long_directory);
            const auto extended=pulse::dialog_session::Canonical(long_directory,true);
            auto long_session=*s;cfg::Ready long_ready=ready;
            wcscpy_s(long_session.nonce,long_directory.filename().c_str());
            wcscpy_s(long_ready.nonce,long_session.nonce);
            wcscpy_s(long_session.control_path,extended.c_str());
            const auto session_path=long_directory/cfg::kSessionFile,ready_path=long_directory/cfg::kReadyFile;
            Check(session_path.wstring().size()<=259 && ready_path.wstring().size()>=260 && long_directory.wstring().size()<260,
                "raw session path fits while receipt crosses MAX_PATH");
            Check(!extended.empty() && Write(fs::path(extended)/cfg::kSessionFile,&long_session,sizeof(long_session)) &&
                Write(fs::path(extended)/cfg::kReadyFile,&long_ready,sizeof(long_ready)),"write real long-boundary session and receipt via extended path");
            cfg::Ready observed;
            Check(pulse::dialog_session::Ready(long_directory,long_session,observed) && observed.installed_entries==1,
                "Ready resolves raw directory before appending long receipt leaf");
            ++long_session.target_created.dwLowDateTime;
            Check(!pulse::dialog_session::Ready(long_directory,long_session,observed),"long receipt still rejects mismatched process identity");
        }
    }
    pulse::dialog_session::Status status;
    const DWORD inspected=pulse::dialog_session::Inspect(directory.wstring(),Self(),false,status);    Check(inspected==0 && status.process_alive && status.historical_handshake && !status.local_disabled,"live process and historical handshake are independent facts");
    Check(pulse::dialog_session::InspectForMachine(directory.wstring(),Self(),static_cast<WORD>(s->machine),status)==0 && status.process_alive &&
        !fs::exists(directory/cfg::kDisableFile),"explicit native-machine inspection is read-only");
    const WORD other_machine=s->machine==IMAGE_FILE_MACHINE_AMD64 ? IMAGE_FILE_MACHINE_I386 : IMAGE_FILE_MACHINE_AMD64;
    Check(pulse::dialog_session::InspectForMachine(directory.wstring(),Self(),other_machine,status)==ERROR_EXE_MACHINE_TYPE_MISMATCH &&
        !fs::exists(directory/cfg::kDisableFile),"wrong verified launcher architecture is rejected without mutation");
    Check(pulse::dialog_session::InspectForMachine(directory.wstring(),Self(),0,status)==ERROR_EXE_MACHINE_TYPE_MISMATCH,
        "unsupported explicit machine is rejected");
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
    {
        const auto foreign=root/Unique();CreateDirectoryW(foreign.c_str(),nullptr);
        wcscpy_s(s->nonce,foreign.filename().c_str());
        wcscpy_s(s->control_path,pulse::dialog_session::Canonical(foreign,true).c_str());
        const auto original_machine=s->machine;s->machine=other_machine;
        Check(Write(foreign/cfg::kSessionFile,s.get(),static_cast<DWORD>(sizeof(*s))),"write opposite-machine metadata fixture");
        auto loaded=std::make_unique<cfg::Session>();
        Check(!pulse::dialog_session::LoadSession(foreign,Self(),*loaded) &&
            pulse::dialog_session::Inspect(foreign.wstring(),Self(),false,status)==ERROR_INVALID_DATA,
            "default LoadSession and Inspect still reject opposite-machine metadata");
        s->machine=original_machine;
    }
    const auto other=root/Unique();
    if(!CreateDirectoryW(other.c_str(),nullptr)){Check(false,"exclusive second session fixture creation");return;}
    wcscpy_s(s->nonce,other.filename().c_str());wcscpy_s(s->control_path,pulse::dialog_session::Canonical(other,true).c_str());
    ++s->target_created.dwLowDateTime;
    Check(Write(other/cfg::kSessionFile,s.get(),static_cast<DWORD>(sizeof(*s))),"write mismatching creation-time fixture");
    Check(pulse::dialog_session::Inspect(other.wstring(),Self(),true,status)!=0 && !fs::exists(other/cfg::kDisableFile),"PID reuse mismatch cannot disable another process");
}
}
int wmain(int argc,wchar_t** argv){
    if(argc==1 && _wcsicmp(fs::path(Self()).filename().c_str(),L"pulse_admin_inert_fixture.exe")==0) {
        // Explicitly renamed owned test image only. No documents, UI, or preferences.
        Sleep(20000);return 0;
    }

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
    if(argc==3 && wcscmp(argv[1],L"--restart-live")==0) {
        wchar_t image[32768]{};GetModuleFileNameW(nullptr,image,32768);
        const auto installed=fs::path(image).parent_path();
        if(!installed.filename().wstring().starts_with(L"PulseRestartInstalled-")) return 2;
        diagnostics_test_data=(root/L"data").wstring();
        const auto old=installed/L"DialogBridge"/std::wstring(64,L'c');
        if(fs::exists(old)) return 3;
        fs::create_directories(old);
        const auto dll=old/L"PulseDialogAuto64.dll";
        fs::copy_file(argv[2],dll);
        HMODULE loaded=LoadLibraryW(dll.c_str());
        Check(loaded!=nullptr,"owned old-component fixture loads in the test process");
        DialogManagementController c(nullptr,0,(root/L"state").wstring());
        bool found=false;
        for(int attempt=0;attempt<8 && !found;++attempt) {
            const auto result=Await(c,c.PollHistory(true));
            if(result) for(const auto& record:result->restart_inventory.records)
                for(const auto& process:record.processes) if(process.pid==GetCurrentProcessId()) found=true;
        }
        Check(found,"verified installed bundle plus worker scan identifies actual loaded old component");
        if(loaded) FreeLibrary(loaded);
        auto result=Await(c,c.PollHistory(true));
        bool remains=false;
        if(result) for(const auto& record:result->restart_inventory.records)
            for(const auto& process:record.processes) if(process.pid==GetCurrentProcessId()) remains=true;
        Check(result && result->restart_checked && !remains,"manual refresh replaces old observation after owned module unload");
        std::printf("Checks: %d, failures: %d\n",checks,failures);return failures?1:0;
    }
    if(argc==2 && wcscmp(argv[1],L"--history-poll")==0) {
        diagnostics_test_data=(root/L"data").wstring();
        const auto logs=fs::path(diagnostics_test_data)/L"Diagnostics"/L"Runtime";
        fs::create_directories(logs);
        std::ofstream(logs/L"dialog_auto-42.jsonl")<<"{\"schema\":1,\"component\":\"dialog_auto\",\"utc\":\"2026-11-11T04:00:00.000Z\",\"session\":134388388389325565,\"pid\":42,\"seq\":1,\"event\":\"token_rejected\",\"data\":{\"reason\":5,\"error\":5,\"reason_name\":\"ELEVATED_BUNDLE_UNTRUSTED\",\"stage\":\"activation\",\"detail\":\"private-document.dwg\"}}\n";
        uint64_t unattributed=0;
        {
            DialogManagementController c(nullptr,0,(root/L"state").wstring());
            auto result=Await(c,c.PollHistory());
            Check(result && !result->error && !result->history_error,"background history poll works without choosing a bundle or application");
            Check(result && result->restart_checked,"background poll includes live restart inventory without opening settings");
            Check(result && result->restart_inventory.records.empty() && !result->restart_inventory.scan_complete,
                "uninstalled fixture does not infer restart requirements from an unverified bundle");
            Check(result && result->history.records.empty() && result->history.unattributed_events>0,"legacy log without process identity is not attributed as an application failure");
            if(result) unattributed=result->history.unattributed_events;
            result=Await(c,c.PollHistory());
            Check(result && !result->history_error && result->history.unattributed_events==unattributed,"repeated background polling does not recount the same unknown event");
            const auto generation=c.PollHistory();c.Cancel();Sleep(100);
            const auto drained=c.Drain();
            Check(std::none_of(drained.begin(),drained.end(),[&](const auto& value){return value.generation==generation;}),"cancelled history generations do not replace visible records");
        }
        {
            DialogManagementController c(nullptr,0,(root/L"state").wstring());
            const auto result=Await(c,c.PollHistory());
            Check(result && !result->error && !result->history_error && result->history.unattributed_events==unattributed,"history cursors survive controller recreation without duplicate counts");
        }
        std::ifstream input{root/L"state"/L"takeover-history.json"};
        const std::string contents((std::istreambuf_iterator<char>(input)),{});
        Check(!contents.empty() && contents.find("private-document")==std::string::npos,"persisted history excludes raw document details");
        input.close();
        if(!failures)Check(SafeCleanup(bench,root,owned_name,bench_id,root_id),"owned history poll fixture cleanup");
        std::printf("Checks: %d, failures: %d\n",checks,failures);return failures?1:0;
    }
    if(argc==2 && wcscmp(argv[1],L"--diagnostics-export")==0){
        diagnostics_test_data=(root/L"data").wstring();
        const auto logs=fs::path(diagnostics_test_data)/L"Diagnostics"/L"Runtime";
        fs::create_directories(logs);
        std::ofstream(logs/L"dialog_auto-42.jsonl")<<"{\"schema\":1,\"component\":\"dialog_auto\",\"utc\":\"2026-11-11T04:00:00.000Z\",\"session\":134388388389325565,\"pid\":42,\"seq\":1,\"event\":\"token_rejected\",\"data\":{\"reason\":5,\"error\":5,\"reason_name\":\"ELEVATED_BUNDLE_UNTRUSTED\",\"stage\":\"activation\",\"detail\":\"private-document.dwg\"}}\n";
        {
            DialogManagementController controller(nullptr,0,(root/L"state").wstring());
            auto result=Await(controller,controller.ExportDiagnostics());
            Check(result && !result->error && !result->diagnostics_path.empty(),"diagnostic export succeeds without a selected bundle or UI");
            if(result && !result->diagnostics_path.empty()){
                std::ifstream input{fs::path(result->diagnostics_path)};
                const std::string text((std::istreambuf_iterator<char>(input)),{});
                Check(text.find("process_snapshot_collection")!=std::string::npos,"export includes outside-process observations when no hook log exists");
                Check(text.find("\"counts_as_requests\":false")!=std::string::npos,"process inventory never becomes a request denominator");
                Check(text.find("ELEVATED_BUNDLE_UNTRUSTED")!=std::string::npos && text.find("134388388389325565")!=std::string::npos,
                    "export includes a readable rejection with exact logger session identity");
                Check(text.find("private-document")==std::string::npos && text.find("dialog_log_collection")!=std::string::npos,
                    "export strips private event details and states collection limits");
                Check(text.find("restart_applications=")!=std::string::npos && text.find("restart_query_errors=")!=std::string::npos &&
                    text.find("history_records=")!=std::string::npos && text.find("history_error=")!=std::string::npos &&
                    text.find("history_unattributed=")!=std::string::npos,"export includes local history completeness without application names");
                Check(text.find("selected_application_machine=")!=std::string::npos &&
                    text.find("managed_launch_bundle_verified=")!=std::string::npos &&
                    text.find("managed_launch_bundle_error=")!=std::string::npos,"export separates selected architecture and launch component readiness");
                Check(text.find("admin_launch_ready="+std::to_string(result->admin_launch_ready)+"\n")!=std::string::npos &&
                    text.find("admin_launch_error="+std::to_string(result->admin_launch_error)+"\n")!=std::string::npos,
                    "export reports exact numeric admin readiness without claiming managed admin sessions");
                Check(text.find("registration_snapshot")!=std::string::npos && text.find("\"process_arch\":32")!=std::string::npos && text.find("\"process_arch\":64")!=std::string::npos,
                    "export includes both registration views with collector-context scope");
                Check(text.find("auto_enabled=")!=std::string::npos && text.find("C:")==std::string::npos,
                    "export retains registration status without process paths or command lines");
            }
        }
        if(!failures)Check(SafeCleanup(bench,root,owned_name,bench_id,root_id),"owned diagnostic export fixture cleanup");
        std::printf("Checks: %d, failures: %d\n",checks,failures);return failures?1:0;
    }
    if(argc==3 && (wcscmp(argv[1],L"--prepare-launch")==0 || wcscmp(argv[1],L"--prepare-installed")==0)){
        const bool installed=wcscmp(argv[1],L"--prepare-installed")==0;
        const auto target=installed ? std::wstring(argv[2]) : Self();
        {
            DialogManagementController c(nullptr,0,(root/L"state").wstring());
            auto result=Await(c,c.PrepareLaunch());
            Check(result && result->error && !result->launch_preflight_checked && !result->launcher_started,
                "prepare rejects absent selection without starting a launcher");
            result=Await(c,installed ? c.Refresh() : c.SelectBundle(argv[2]));
            Check(result && !result->error && result->bundle_verified,"prepare fixture bundle verified");
            result=Await(c,c.SelectApplication(target));
            Check(result && !result->error && result->application_valid,"prepare selects owned executable");
            const auto event=L"Local\\PulsePrepare"+Unique();
            HANDLE done=CreateEventW(nullptr,TRUE,FALSE,event.c_str());
            HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,(event+L".ready").c_str());
            SetEnvironmentVariableW(L"PULSE_TEST_MANAGEMENT_CHILD",event.c_str());
            auto command=L"\""+target+L"\"";STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION info{};
            const bool launched=CreateProcessW(target.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&info)!=FALSE;
            SetEnvironmentVariableW(L"PULSE_TEST_MANAGEMENT_CHILD",nullptr);
            Check(launched && WaitForSingleObject(ready,5000)==WAIT_OBJECT_0,"owned existing process ready");
            result=Await(c,c.PrepareLaunch());
            Check(result && !result->error && result->launch_preflight_checked && result->existing_target_count>=1,
                "prepare observes an existing same-image process on worker");
            Check(result && !result->launcher_started && result->sessions.empty(),"prepare creates no managed process or session");
            Check(result && (!result->launch_query_errors || !result->launch_scan_complete),"partial query is never complete");
            Check(launched && WaitForSingleObject(info.hProcess,0)==WAIT_TIMEOUT,"preflight leaves old process alive");
            c.PrepareLaunch();c.Cancel();Sleep(100);
            Check(c.Drain().empty(),"cancel suppresses stale preflight result");
            SetEvent(done);
            Check(launched && WaitForSingleObject(info.hProcess,5000)==WAIT_OBJECT_0,"owned child exits by event only");
            if(info.hProcess)CloseHandle(info.hProcess);if(info.hThread)CloseHandle(info.hThread);
            ResetEvent(done);ResetEvent(ready);
            SetEnvironmentVariableW(L"PULSE_TEST_MANAGEMENT_CHILD",event.c_str());
            result=Await(c,c.Launch());
            SetEnvironmentVariableW(L"PULSE_TEST_MANAGEMENT_CHILD",nullptr);
            Check(result && !result->error && result->launcher_started,"controller starts verified managed launcher");
            Check(WaitForSingleObject(ready,5000)==WAIT_OBJECT_0,"managed owned application resumed");
            result=Await(c,c.Refresh());
            if(result && result->sessions.size()==1) {
                const auto& observed=result->sessions.front();
                for(const auto& file:fs::directory_iterator(observed.directory)) std::printf("LIVE_FILE %ls\n",file.path().filename().c_str());
                std::printf("LIVE_SESSION pid=%lu alive=%u handshake=%u status_error=%lu initialization=%08lX entries=%u\n",
                    observed.pid,observed.process_alive,observed.historical_handshake,observed.status_error,
                    static_cast<unsigned long>(observed.initialization),observed.entry_coverage);
            }
            Check(result && result->sessions.size()==1 && result->sessions.front().historical_handshake &&
                result->sessions.front().process_alive && !result->sessions.front().status_error,"controller observes valid live session and historical handshake separately");
            HANDLE managed=nullptr;
            if(result && result->sessions.size()==1)
                managed=OpenProcess(SYNCHRONIZE,FALSE,result->sessions.front().pid);
            SetEvent(done);
            Check(managed && WaitForSingleObject(managed,5000)==WAIT_OBJECT_0,"managed owned child exits by event only");
            if(managed)CloseHandle(managed);
            CloseHandle(ready);CloseHandle(done);
        }
        if(!failures)Check(SafeCleanup(bench,root,owned_name,bench_id,root_id),"owned preflight fixture cleanup");
        std::printf("Checks: %d, failures: %d\n",checks,failures);return failures?1:0;
    }
    if(argc==2 && wcscmp(argv[1],L"--session-only")==0){
        SessionTests(root);
        if(!failures)Check(SafeCleanup(bench,root,owned_name,bench_id,root_id),"owned session fixture cleanup");
        std::printf("Checks: %d, failures: %d\n",checks,failures);return failures?1:0;
    }
    HWND host=CreateWindowExW(0,L"STATIC",L"management fixture",0,0,0,0,0,HWND_MESSAGE,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(host!=nullptr,"private wake host");
    if(argc==4 && std::wstring_view(argv[1])==L"--admin-installed-ui") {
        const bool expect_cancel=std::wstring_view(argv[3])==L"cancel";
        if(!expect_cancel && std::wstring_view(argv[3])!=L"launch") return 2;
        DialogManagementController c(host,WM_APP+970,(root/L"admin-state").wstring());
        auto result=Await(c,c.SelectApplication(argv[2]));
        Check(result && !result->error && result->application_valid,"explicit admin UI fixture selects owned target");
        result=Await(c,c.PrepareAdminLaunch());
        Check(result && !result->error && result->admin_launch_ready && result->launch_preflight_checked && !result->launcher_started,
            "protected installed components are admin-ready before user consent");
        if(result && !result->error && result->admin_launch_ready) {
            result=Await(c,c.LaunchAdmin(),120000);
            if(result) std::printf("ADMIN_UI_RESULT error=%lu launcher_started=%u admin_ready=%u\n",result->error,result->launcher_started,result->admin_launch_ready);
            Check(result && (expect_cancel ? result->error==ERROR_CANCELLED && !result->launcher_started : !result->error && result->launcher_started),
                expect_cancel ? "requested UAC cancellation is retained without launch" : "requested UAC consent starts verified launcher");
        }
        DestroyWindow(host);
        if(!failures)Check(SafeCleanup(bench,root,owned_name,bench_id,root_id),"owned admin UI controller state cleanup");
        std::printf("Checks: %d, failures: %d\n",checks,failures);return failures?1:0;
    }
    if(argc==3 && std::wstring_view(argv[1])==L"--admin-boundaries") {
        DialogManagementController c(host,WM_APP+970,(root/L"admin-state").wstring());
        auto result=Await(c,c.SelectApplication(Self()));
        Check(result && !result->error && result->application_valid,"admin boundary fixture selects valid application");
        result=Await(c,c.PrepareAdminLaunch());
        Check(result && result->error && !result->admin_launch_ready && !result->launcher_started,"unprotected installed tree cannot prepare elevation");
        result=Await(c,c.LaunchAdmin());
        Check(result && result->error && !result->launcher_started,"unprotected installed tree never opens UAC launcher");
        result=Await(c,c.SelectBundle(argv[2]));
        Check(result && !result->error && result->launch_bundle_verified && !result->admin_launch_ready && result->admin_launch_error==ERROR_NOT_SUPPORTED,
            "explicit experimental bundle stays ordinary-only even when valid");
        result=Await(c,c.PrepareAdminLaunch());
        Check(result && result->error==ERROR_NOT_SUPPORTED && !result->launch_preflight_checked,"experimental package cannot prepare admin launch");
        result=Await(c,c.LaunchAdmin());
        Check(result && result->error==ERROR_NOT_SUPPORTED && !result->launcher_started,"experimental package cannot trigger runas");
        const auto stale=c.PrepareAdminLaunch();c.Cancel();Sleep(100);
        const auto cancelled=c.Drain();Check(std::none_of(cancelled.begin(),cancelled.end(),[&](const auto& value){return value.generation==stale;}),
            "cancel suppresses stale admin preparation without process termination");
        Check(fs::is_empty(root/L"admin-state"/L"operations"),"rejected admin operations never create ordinary launch sessions");
        DestroyWindow(host);
        if(!failures)Check(SafeCleanup(bench,root,owned_name,bench_id,root_id),"owned admin boundary fixture cleanup");
        std::printf("Checks: %d, failures: %d\n",checks,failures);return failures?1:0;
    }
    if(argc==4 && std::wstring_view(argv[1])==L"--arch-routing") {
        // Run a copied test executable beside an isolated installed dual-bundle
        // fixture. argv[3] is good, missing, or bad; never edit its pointers here.
        const bool companion_ok=std::wstring_view(argv[3])==L"good";
        pulse::dialog_bundle::BundleLease primary,companion;
        const auto install=fs::path(Self()).parent_path().wstring();
        const DWORD primary_error=pulse::dialog_bundle::ResolveInstalledBundleForMachine(install,IMAGE_FILE_MACHINE_AMD64,primary);
        const DWORD companion_error=pulse::dialog_bundle::ResolveInstalledBundleForMachine(install,IMAGE_FILE_MACHINE_I386,companion);
        Check(!primary_error,"routing fixture primary x64 package is verified");
        Check(companion_ok ? !companion_error : companion_error!=0,"routing fixture companion availability matches case");
        if(!primary_error) {
            DialogManagementController c(host,WM_APP+970,(root/L"route-state").wstring());
            auto result=Await(c,c.SelectApplication(Self()));
            Check(result && !result->error && result->application_valid && result->application_machine==IMAGE_FILE_MACHINE_AMD64 && result->launch_bundle_verified,
                "installed x64 application resolves verified x64 launch bundle");
            result=Await(c,c.SelectApplication(argv[2]));
            Check(result && !result->error && result->application_valid && result->application_machine==IMAGE_FILE_MACHINE_I386,
                "x64 controller accepts valid x86 application independently of components");
            Check(result && result->launch_bundle_verified==companion_ok && (companion_ok ? result->launch_bundle_error==0 : result->launch_bundle_error!=0),
                "x86 launch readiness requires its verified companion, never x64 fallback");
            result=Await(c,c.PrepareLaunch());
            Check(result && !result->launcher_started && (companion_ok ? result->launch_preflight_checked :
                result->error!=0 && !result->launch_preflight_checked),"preflight routes selected architecture or refuses before launch");
            {
                DialogManagementController restored(host,WM_APP+970,(root/L"route-state").wstring());
                const auto persisted=Await(restored,restored.Refresh());
                Check(persisted && persisted->application_valid && persisted->application_machine==IMAGE_FILE_MACHINE_I386 &&
                    persisted->configuration.bundle_root==primary.Directory() && persisted->launch_bundle_verified==companion_ok,
                    "default-mode reload restores x86 application and re-resolves installed routing");
            }
            const auto saved_data=pulse::app::diagnostics_test_data;
            pulse::app::diagnostics_test_data=(root/L"owned-data").wstring();
            {
                DialogManagementController experimental(nullptr,0,(root/L"owned-data"/L"DialogManagement").wstring());
                auto selected=Await(experimental,experimental.SelectBundle(companion_ok ? companion.Directory() : primary.Directory()));
                Check(selected && !selected->error,"seed owned production-data fixture with old explicit package selection");
                selected=Await(experimental,experimental.SelectApplication(argv[2]));
                Check(selected && !selected->error,"seed owned production-data fixture application");
            }
            {
                DialogManagementController production(nullptr,0,L"");
                auto selected=Await(production,production.Refresh());
                Check(selected && selected->application_valid && selected->application_machine==IMAGE_FILE_MACHINE_I386 &&
                    selected->configuration.bundle_root==primary.Directory() && selected->launch_bundle_verified==companion_ok,
                    "production reload retains application but ignores persisted experimental bundle");
                selected=Await(production,production.SelectApplication(argv[2]));
                Check(selected && !selected->error,"production application selection persists without package pinning");
            }
            {
                DialogManagementController production(nullptr,0,L"");
                const auto selected=Await(production,production.Refresh());
                Check(selected && selected->application_valid && selected->application_machine==IMAGE_FILE_MACHINE_I386 &&
                    selected->configuration.bundle_root==primary.Directory(),"production empty-root restart restores saved x86 selection");
            }
            pulse::app::diagnostics_test_data=saved_data;
            result=Await(c,c.SelectBundle(primary.Directory()));
            Check(result && !result->error && result->application_valid && !result->launch_bundle_verified &&
                result->launch_bundle_error==ERROR_EXE_MACHINE_TYPE_MISMATCH,"explicit x64 package does not guess an x86 sibling");
            result=Await(c,c.Launch());
            Check(result && result->error==ERROR_EXE_MACHINE_TYPE_MISMATCH && !result->launcher_started,
                "explicit wrong-architecture launch never starts launcher");
            if(companion_ok) {
                result=Await(c,c.SelectBundle(companion.Directory()));
                Check(result && !result->error && result->launch_bundle_verified && result->application_machine==IMAGE_FILE_MACHINE_I386,
                    "explicit verified x86 package matches x86 application");
                result=Await(c,c.SelectApplication(Self()));
                Check(result && !result->error && result->application_valid && !result->launch_bundle_verified &&
                    result->launch_bundle_error==ERROR_EXE_MACHINE_TYPE_MISMATCH,"explicit x86 package rejects x64 target without guessing primary");
            }
        }
        DestroyWindow(host);
        if(!failures)Check(SafeCleanup(bench,root,owned_name,bench_id,root_id),"owned routing state cleanup");
        std::printf("Checks: %d, failures: %d\n",checks,failures);return failures?1:0;
    }
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

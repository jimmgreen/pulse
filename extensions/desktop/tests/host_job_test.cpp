#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <string>
#include "plugin/host_job.h"

std::wstring Image() { wchar_t p[32768]{}; GetModuleFileNameW(nullptr,p,32768); return p; }
int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout,nullptr,_IONBF,0); std::printf("start argc=%d pid=%lu\n",argc,GetCurrentProcessId());
    if (argc > 1 && std::wstring(argv[1]) == L"--child") { Sleep(30000); return 0; }
    if (argc > 2 && std::wstring(argv[1]) == L"--host") {
        const std::wstring name=argv[2];
        HANDLE mapping=OpenFileMappingW(FILE_MAP_WRITE,FALSE,name.c_str());
        auto pid=static_cast<DWORD*>(MapViewOfFile(mapping,FILE_MAP_WRITE,0,0,sizeof(DWORD)));
        HANDLE ready=OpenEventW(EVENT_MODIFY_STATE,FALSE,(name+L"-ready").c_str());
        SHELLEXECUTEINFOW info{}; info.cbSize=sizeof(info);
        info.fMask=SEE_MASK_NOCLOSEPROCESS|SEE_MASK_NOASYNC|SEE_MASK_FLAG_NO_UI;
        const auto image=Image(); info.lpFile=image.c_str(); info.lpParameters=L"--child"; info.nShow=SW_HIDE;
        const bool ok=ShellExecuteExW(&info)!=FALSE && info.hProcess;
        if(pid) *pid=ok?GetProcessId(info.hProcess):0;
        if(info.hProcess) CloseHandle(info.hProcess);
        SetEvent(ready); if(pid)UnmapViewOfFile(pid); CloseHandle(mapping); CloseHandle(ready);
        Sleep(30000); return ok?0:2;
    }
    const bool baseline=argc>1 && std::wstring(argv[1])==L"--kill-only";
    const std::wstring name=L"Local\\PulseDesktopLaunchTest-"+std::to_wstring(GetCurrentProcessId());
    HANDLE mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(DWORD),name.c_str());
    auto pid=static_cast<DWORD*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,sizeof(DWORD)));
    HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,(name+L"-ready").c_str());
    HANDLE job=CreateJobObjectW(nullptr,nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    bool ok=baseline ? SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits))!=FALSE
                     : pulse::desktop::plugin::ConfigureHostJob(job);
    const auto image=Image(); auto cmd=L"\""+image+L"\" --host "+name;
    STARTUPINFOW start{}; start.cb=sizeof(start); PROCESS_INFORMATION host{};
    ok=ok && CreateProcessW(image.c_str(),cmd.data(),nullptr,nullptr,FALSE,CREATE_SUSPENDED|CREATE_NO_WINDOW,nullptr,nullptr,&start,&host);
    ok=ok && AssignProcessToJobObject(job,host.hProcess) && ResumeThread(host.hThread)!=DWORD(-1);
    ok=ok && WaitForSingleObject(ready,10000)==WAIT_OBJECT_0 && pid && *pid;
    DWORD host_exit=0; if(host.hProcess)GetExitCodeProcess(host.hProcess,&host_exit);
    std::printf("launch_ok=%d child_pid=%lu host_exit=%lu error=%lu\n",ok,pid?*pid:0,host_exit,GetLastError());
    HANDLE child=ok?OpenProcess(SYNCHRONIZE|PROCESS_TERMINATE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,*pid):nullptr;
    ok=ok && child;
    BOOL belongs=FALSE; if(child)ok=ok && IsProcessInJob(child,job,&belongs);
    std::puts("closing job"); CloseHandle(job);
    const bool host_ended=host.hProcess && WaitForSingleObject(host.hProcess,5000)==WAIT_OBJECT_0;
    const bool child_alive=child && WaitForSingleObject(child,500)==WAIT_TIMEOUT;
    std::printf("mode=%s app_in_host_job=%d host_ended=%d app_alive=%d\n",baseline?"control-kill-only":"host-job-policy",belongs,host_ended,child_alive);
    if(child) { if(child_alive)TerminateProcess(child,0); WaitForSingleObject(child,5000); CloseHandle(child); }
    if(host.hProcess){if(!host_ended)TerminateProcess(host.hProcess,0);CloseHandle(host.hProcess);}
    if(host.hThread)CloseHandle(host.hThread);
    if(pid)UnmapViewOfFile(pid); CloseHandle(mapping);CloseHandle(ready);
    return ok && host_ended && (baseline ? (!child_alive && belongs) : (child_alive && !belongs)) ? 0 : 1;
}

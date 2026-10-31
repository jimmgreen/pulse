#include "../dialog_launcher/process_launch_path.h"
#include <windows.h>
#include <cstdio>
using pulse::dialog_launcher::ProcessLaunchPath;
int wmain(int argc,wchar_t**) {
    wchar_t image[32768]{},cwd[32768]{};
    if (!GetModuleFileNameW(nullptr,image,32768) || !GetCurrentDirectoryW(32768,cwd)) return 2;
    if (argc > 1) return std::wstring_view(image).starts_with(L"\\\\?\\") || std::wstring_view(cwd).starts_with(L"\\\\?\\") ? 3 : 0;
    unsigned checks=0, failures=0;
    auto check=[&](bool ok,const char* name){++checks;if(!ok)++failures;std::printf("[%s] %s\n",ok?"PASS":"FAIL",name);};
    check(ProcessLaunchPath(L"\\\\?\\E:\\Program Files\\产品\\acad.exe")==L"E:\\Program Files\\产品\\acad.exe","ordinary Unicode path retains DOS spelling");
    check(ProcessLaunchPath(L"\\\\?\\E:\\")==L"E:\\","drive root");
    for (const auto* value : {L"\\\\?\\UNC\\server\\share\\file.exe",L"\\\\?\\C:\\tail.\\app.exe",L"\\\\?\\C:\\tail \\app.exe",
        L"\\\\?\\C:\\NUL.txt",L"\\\\?\\C:\\COM1.txt",L"\\\\?\\C:\\..\\app.exe",L"\\\\?\\Volume{a}\\app.exe",L"C:\\app.exe"})
        check(ProcessLaunchPath(value)==value,"special or already ordinary spelling is preserved");
    const auto long_path=std::wstring(L"\\\\?\\C:\\")+std::wstring(270,L'a');
    check(ProcessLaunchPath(long_path)==long_path,"long path retains extended semantics");
    auto target=ProcessLaunchPath(std::wstring(L"\\\\?\\")+image);
    auto directory=ProcessLaunchPath(std::wstring(L"\\\\?\\")+cwd);
    std::wstring command=L"\""+target+L"\" --child";
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION child{};
    const BOOL started=CreateProcessW(target.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,directory.c_str(),&startup,&child);
    check(started!=FALSE,"launch real owned child using process-facing paths");
    if(started){const auto wait=WaitForSingleObject(child.hProcess,10000);DWORD code=99;GetExitCodeProcess(child.hProcess,&code);
        check(wait==WAIT_OBJECT_0 && code==0,"child observes ordinary module path and working directory");CloseHandle(child.hThread);CloseHandle(child.hProcess);}
    std::printf("Checks=%u failures=%u\n",checks,failures);return failures?1:0;
}

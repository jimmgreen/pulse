#include "../app/native_explorer.h"
#include <cstdio>
#include <string>
#include <vector>

namespace ne = pulse::app::native_explorer;
namespace pulse::app::native_explorer {
DWORD LaunchForTest(const Request&, decltype(&CreateProcessW));
void ClearPendingForTest(); void ExpirePendingForTest();
}
static int failures = 0;
static void Check(bool ok, const char* name) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); if (!ok) ++failures; }
static std::wstring command, image, desktop;
static BOOL WINAPI Capture(LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES,
    BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW start, LPPROCESS_INFORMATION process) {
    image = app; command = cmd; desktop = start->lpDesktop;
    process->hProcess = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    process->hThread = CreateEventW(nullptr, TRUE, FALSE, nullptr); return TRUE;
}
static BOOL WINAPI Fail(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES,
    BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
static HWND Window() { return CreateWindowExW(0,L"STATIC",L"Native guard fixture",WS_OVERLAPPED,0,0,20,20,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr); }
static DWORD completion = 0xffffffff; static LPARAM cookie = 0;
static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == ne::kCompletedMessage && ne::IsCompletionFor(h,l)) { completion=static_cast<DWORD>(w); cookie=l; return 0; }
    return DefWindowProcW(h,m,w,l);
}
static bool Wait() { const auto until=GetTickCount64()+5000; while (completion==0xffffffff && GetTickCount64()<until) {
    MSG m{}; while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)) {TranslateMessage(&m);DispatchMessageW(&m);} Sleep(5); } return completion!=0xffffffff; }
int wmain(int argc, wchar_t** argv) {
    if (argc!=2) return 2;
    const HDESK previous=GetThreadDesktop(GetCurrentThreadId());
    const auto name=L"PulseNativeGuard-"+std::to_wstring(GetCurrentProcessId());
    const HDESK desk=CreateDesktopW(name.c_str(),nullptr,nullptr,0,GENERIC_ALL,nullptr);
    if(!desk || !SetThreadDesktop(desk)) return 3;
    const std::wstring folder=argv[1], file=folder+L"\\sample.txt";
    Check(ne::IsFilesystemPath(L"C:\\a") && ne::IsFilesystemPath(L"\\\\server\\share\\a") &&
        ne::IsFilesystemPath(L"\\\\?\\UNC\\server\\share\\a") && ne::IsFilesystemPath(L"\\\\?\\C:\\a"), "filesystem drive UNC extended paths");
    Check(!ne::IsFilesystemPath(L"shell:Home") && !ne::IsFilesystemPath(L"C:relative") &&
        !ne::IsFilesystemPath(L"\\\\.\\device") && !ne::IsFilesystemPath(L"\\\\server") &&
        !ne::IsFilesystemPath(L"C:\\bad\"arg"), "virtual relative device malformed paths rejected");
    HWND existing=Window();
    Check(ne::LaunchForTest({folder,ne::Intent::OpenFolder},Capture)==0 && command.find(L" /n,/e,")!=command.npos &&
        image.ends_with(L"\\explorer.exe") && desktop==name, "explicit native folder image args desktop");
    Check(!ne::ShouldDelayVeil(existing) && !ne::RetainRequestedWindow(existing,folder,{},true), "existing HWND never claimed");
    HWND fresh=Window();
    Check(ne::ShouldDelayVeil(fresh) && ne::ShouldWaitForRequest(fresh,L""), "new transient Home settles without handoff");
    Check(!ne::RetainRequestedWindow(fresh,folder+L"-wrong",{},true), "wrong folder not claimed");
    Check(ne::RetainRequestedWindow(fresh,folder,{},true) && ne::IsExplicitNativeWindow(fresh), "new exact folder retained by HWND identity");
    auto old=GetPropW(fresh,L"Pulse.NativeExplorer.Pid");SetPropW(fresh,L"Pulse.NativeExplorer.Pid",reinterpret_cast<HANDLE>(1));
    Check(!ne::IsExplicitNativeWindow(fresh), "PID identity mismatch rejects marker");SetPropW(fresh,L"Pulse.NativeExplorer.Pid",old);
    DestroyWindow(fresh); fresh=Window();Check(!ne::IsExplicitNativeWindow(fresh), "destroyed HWND marker does not transfer");
    ne::ClearPendingForTest(); DestroyWindow(fresh);
    Check(ne::LaunchForTest({file,ne::Intent::SelectFile},Capture)==0 && command.find(L" /n,/select,")!=command.npos,"select launch retains exact file argument");
    fresh=Window();
    Check(!ne::RetainRequestedWindow(fresh,folder,{},true) && ne::ShouldWaitForRequest(fresh,folder),"late selection delays handoff");
    Check(!ne::RetainRequestedWindow(fresh,folder,{L"other.txt"},true) && !ne::RetainRequestedWindow(fresh,folder,{L"sample.txt"},false),"wrong or unread selection not claimed");
    Check(ne::RetainRequestedWindow(fresh,folder,{L"sample.txt"},true),"exact file selection claimed");
    ne::ExpirePendingForTest(); Check(ne::IsExplicitNativeWindow(fresh),"claimed window remains native beyond intent deadline");
    DestroyWindow(fresh); fresh=Window(); Check(!ne::ShouldDelayVeil(fresh) && !ne::ShouldWaitForRequest(fresh,folder),"expired pending releases unrelated windows");
    ne::ClearPendingForTest(); Check(ne::LaunchForTest({folder,ne::Intent::OpenFolder},Fail)==ERROR_ACCESS_DENIED,"process creation failure returned");
    DestroyWindow(fresh);fresh=Window();Check(!ne::ShouldDelayVeil(fresh),"failed launch removes pending protection");
    WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"PulseNativeCompletionTest";wc.lpfnWndProc=Proc;RegisterClassW(&wc);
    HWND owner=CreateWindowW(wc.lpszClassName,L"",0,0,0,0,0,HWND_MESSAGE,nullptr,wc.hInstance,nullptr);
    Check(ne::OpenAsync({folder+L"\\missing",ne::Intent::OpenFolder},owner)==0 && Wait() && completion==ERROR_FILE_NOT_FOUND,"real async failure delivered with owner cookie");
    // A new Win32 owner initializes process-wide services asynchronously.
    // Pump that startup before taking the unchanged resource-growth baseline.
    const auto settled=GetTickCount64()+2000;
    while(GetTickCount64()<settled) { MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(5); }
    DWORD before=0,after=0; GetProcessHandleCount(GetCurrentProcess(),&before);
    bool repeats=true; for(int i=0;i<16;++i) {completion=0xffffffff;repeats &= ne::OpenAsync({folder+L"\\missing",ne::Intent::OpenFolder},owner)==0 && Wait();}
    Sleep(2000);GetProcessHandleCount(GetCurrentProcess(),&after);
    Check(repeats && after<=before+2,"repeated worker failures release desktop and thread handles");
    printf("handles_before=%lu handles_after=%lu\n",before,after);
    const auto retired=cookie;DestroyWindow(owner);Check(!ne::IsCompletionFor(owner,retired),"owner destruction rejects stale completion");
    DestroyWindow(fresh);DestroyWindow(existing);ne::ClearPendingForTest();SetThreadDesktop(previous);Check(CloseDesktop(desk)!=0,"private test desktop released");
    printf("failures=%d\n",failures);return failures?1:0;
}

#include "setup_desktop_launch.h"
#include "../common/current_user_security.h"
#include <shlobj.h>
#include <shldisp.h>
#include <exdisp.h>
#include <wrl/client.h>
#include <vector>

namespace pulse::setup {
HRESULT LaunchFromDesktop(const std::wstring& executable, const std::wstring& arguments,
                          const std::wstring& directory) {
    using Microsoft::WRL::ComPtr;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return initialized;
    struct ComScope { ~ComScope() { CoUninitialize(); } } com;
    ComPtr<IShellWindows> windows;
    HRESULT hr = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows));
    if (FAILED(hr)) return hr;
    VARIANT location{}, empty{}; location.vt = VT_I4; location.lVal = CSIDL_DESKTOP;
    long hwnd = 0;
    ComPtr<IDispatch> desktop;
    hr = windows->FindWindowSW(&location, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &desktop);
    if (FAILED(hr) || !desktop) return FAILED(hr) ? hr : E_NOINTERFACE;
    DWORD pid = 0, session = 0, own_session = 0;
    GetWindowThreadProcessId(reinterpret_cast<HWND>(static_cast<LONG_PTR>(hwnd)), &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return HRESULT_FROM_WIN32(GetLastError());
    const auto user = ProcessUserSidString(process);
    HANDLE token = nullptr;
    const bool opened = OpenProcessToken(process, TOKEN_QUERY, &token) != FALSE;
    CloseHandle(process);
    if (!opened) return HRESULT_FROM_WIN32(GetLastError());
    TOKEN_ELEVATION elevation{}; DWORD bytes = 0;
    alignas(TOKEN_MANDATORY_LABEL) BYTE label[256]{};
    const bool queried = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &bytes) &&
        GetTokenInformation(token, TokenIntegrityLevel, label, sizeof(label), &bytes);
    CloseHandle(token);
    if (!queried) return E_ACCESSDENIED;
    const PSID sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(label)->Label.Sid;
    const UCHAR count = *GetSidSubAuthorityCount(sid);
    if (!count || *GetSidSubAuthority(sid, count - 1) != SECURITY_MANDATORY_MEDIUM_RID || elevation.TokenIsElevated ||
        user.empty() || user != CurrentUserSidString() || !ProcessIdToSessionId(pid, &session) ||
        !ProcessIdToSessionId(GetCurrentProcessId(), &own_session) || session != own_session) return E_ACCESSDENIED;
    ComPtr<IServiceProvider> services;
    if (FAILED(hr = desktop.As(&services))) return hr;
    ComPtr<IShellBrowser> browser;
    if (FAILED(hr = services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser)))) return hr;
    ComPtr<IShellView> view;
    if (FAILED(hr = browser->QueryActiveShellView(&view))) return hr;
    ComPtr<IDispatch> background;
    if (FAILED(hr = view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&background)))) return hr;
    ComPtr<IShellFolderViewDual> folder;
    if (FAILED(hr = background.As(&folder))) return hr;
    ComPtr<IDispatch> application;
    if (FAILED(hr = folder->get_Application(&application))) return hr;
    ComPtr<IShellDispatch2> shell;
    if (FAILED(hr = application.As(&shell))) return hr;
    BSTR file = SysAllocString(executable.c_str());
    VARIANT args{}, working{}, verb{}, show{};
    args.vt = working.vt = verb.vt = VT_BSTR;
    args.bstrVal = SysAllocString(arguments.c_str());
    working.bstrVal = SysAllocString(directory.c_str());
    verb.bstrVal = SysAllocString(L"open"); show.vt = VT_I4; show.lVal = SW_SHOWNORMAL;
    hr = file && args.bstrVal && working.bstrVal && verb.bstrVal
        ? shell->ShellExecute(file, args, working, verb, show) : E_OUTOFMEMORY;
    SysFreeString(file); VariantClear(&args); VariantClear(&working); VariantClear(&verb);
    return hr;
}
}

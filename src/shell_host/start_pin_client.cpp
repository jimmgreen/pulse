#include "start_pin_client.h"
#include "../start_pin/protocol.h"
#include <shlobj.h>
#include <shldisp.h>
#include <exdisp.h>
#include <wrl/client.h>

namespace pulse::start_pin {
namespace {
using Microsoft::WRL::ComPtr;
HRESULT Explorer(IShellDispatch2** output) {
    ComPtr<IShellWindows> windows;
    HRESULT hr = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows));
    if (FAILED(hr)) return hr;
    VARIANT empty{}; long hwnd = 0; ComPtr<IDispatch> desktop;
    hr = windows->FindWindowSW(&empty, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &desktop);
    if (FAILED(hr) || !desktop) return FAILED(hr) ? hr : E_NOINTERFACE;
    ComPtr<IServiceProvider> services; hr = desktop.As(&services); if (FAILED(hr)) return hr;
    ComPtr<IShellBrowser> browser;
    hr = services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser)); if (FAILED(hr)) return hr;
    ComPtr<IShellView> view; hr = browser->QueryActiveShellView(&view); if (FAILED(hr)) return hr;
    ComPtr<IDispatch> dispatch; hr = view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&dispatch)); if (FAILED(hr)) return hr;
    ComPtr<IShellFolderViewDual> dual; hr = dispatch.As(&dual); if (FAILED(hr)) return hr;
    ComPtr<IDispatch> app; hr = dual->get_Application(&app); if (FAILED(hr)) return hr;
    return app.CopyTo(output);
}
struct RequestFile {
    std::wstring path;
    ~RequestFile() { if (!path.empty()) DeleteFileW(path.c_str()); }
};
}
HRESULT Invoke(const std::wstring& path, bool pin) {
    if (path.empty() || path.size() > 32767 || path.find(L'\0') != std::wstring::npos) return E_INVALIDARG;
    // Avoid the Shell's no-association UI when this component is not installed.
    wchar_t dll[32768]{}; DWORD bytes = sizeof(dll);
    const std::wstring key = std::wstring(L"Software\\Classes\\CLSID\\") + kClass + L"\\InprocServer32";
    HKEY server = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &server) != ERROR_SUCCESS)
        return REGDB_E_CLASSNOTREG;
    const auto read = RegGetValueW(server, nullptr, nullptr, RRF_RT_REG_SZ, nullptr, dll, &bytes);
    RegCloseKey(server);
    if (read != ERROR_SUCCESS || GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES) return REGDB_E_CLASSNOTREG;
    GUID id{}; HRESULT hr = CoCreateGuid(&id); if (FAILED(hr)) return hr;
    wchar_t nonce[40]{}; StringFromGUID2(id, nonce, 40);
    CurrentUserSecurityAttributes security; if (!security) return E_ACCESSDENIED;
    Handle pipe(CreateNamedPipeW((std::wstring(kPipePrefix) + nonce).c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 65536, 65536, 0, security.get()));
    if (!pipe) return HRESULT_FROM_WIN32(GetLastError());
    wchar_t temp[32768]{}; const auto length = GetTempPathW(32768, temp);
    if (!length || length >= 32768) return HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);
    RequestFile request_file;
    request_file.path = std::wstring(temp) + nonce + L".pulse-start-pin";
    {
        Handle file(CreateFileW(request_file.path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, security.get(),
                                CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr));
        if (!file) { request_file.path.clear(); return HRESULT_FROM_WIN32(GetLastError()); }
    }
    ComPtr<IShellDispatch2> explorer; hr = Explorer(&explorer); if (FAILED(hr)) return hr;
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr)); if (!event) return HRESULT_FROM_WIN32(GetLastError());
    OVERLAPPED connection{}; connection.hEvent = event.value;
    const BOOL connected = ConnectNamedPipe(pipe.value, &connection);
    const DWORD error = connected ? ERROR_SUCCESS : GetLastError();
    if (!connected && error != ERROR_IO_PENDING && error != ERROR_PIPE_CONNECTED) return HRESULT_FROM_WIN32(error);
    VARIANT args{}, directory{}, verb{}, show{};
    args.vt = VT_BSTR; args.bstrVal = SysAllocString(nonce);
    verb.vt = VT_BSTR; verb.bstrVal = SysAllocString(L"PulseStartPin");
    show.vt = VT_I4; show.lVal = SW_HIDE;
    BSTR file = SysAllocString(request_file.path.c_str());
    hr = explorer->ShellExecute(file, args, directory, verb, show);
    SysFreeString(file); VariantClear(&args); VariantClear(&verb);
    DWORD transferred = 0;
    if (hr != S_OK || (error == ERROR_IO_PENDING && WaitForSingleObject(event.value, kTimeout) != WAIT_OBJECT_0)) {
        CancelIoEx(pipe.value, &connection); GetOverlappedResult(pipe.value, &connection, &transferred, TRUE);
        return hr == S_OK ? HRESULT_FROM_WIN32(ERROR_TIMEOUT) : FAILED(hr) ? hr : E_FAIL;
    }
    if (error == ERROR_IO_PENDING && !GetOverlappedResult(pipe.value, &connection, &transferred, FALSE))
        return HRESULT_FROM_WIN32(GetLastError());
    wchar_t windows[32768]{}; GetWindowsDirectoryW(windows, 32768);
    if (!PeerIs(pipe.value, true, std::wstring(windows) + L"\\explorer.exe")) return E_ACCESSDENIED;
    Request request{1, pin ? 1u : 0u, static_cast<uint32_t>(path.size())};
    const auto deadline = GetTickCount64() + kTimeout;
    if (!Io(pipe.value, &request, sizeof(request), true, deadline) ||
        !Io(pipe.value, const_cast<wchar_t*>(path.data()), request.characters * sizeof(wchar_t), true, deadline) ||
        !Io(pipe.value, &hr, sizeof(hr), false, deadline)) return HRESULT_FROM_WIN32(GetLastError());
    return hr;
}
}

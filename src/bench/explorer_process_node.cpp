// Explicitly loaded by a newly created Node test process, never injected into an app.
#include "../shell_bridge/explorer_process_route.h"
#include <detours.h>
#include <shobjidl.h>
#include <cstdio>
#include "../common/explorer_launch_request.h"
namespace {
auto original = &CreateProcessW;
bool observe_installed = false;
bool Gate() noexcept { return true; }
BOOL WINAPI Create(LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta,
    BOOL inherit, DWORD flags, LPVOID env, LPCWSTR cwd, LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi) {
    BOOL result = FALSE;
    if (observe_installed) result = original(app, cmd, pa, ta, inherit, flags, env, cwd, si, pi);
    if (observe_installed || pulse::shell_bridge::explorer_process::TryRoute(original, app, cmd, pa, ta, inherit, flags, env, cwd, si, pi, result)) {
        const DWORD error = GetLastError();
        wchar_t image[32768]{}; DWORD size = ARRAYSIZE(image);
        const BOOL queried = result && QueryFullProcessImageNameW(pi->hProcess, 0, image, &size);
        const auto* name = wcsrchr(image, L'\\');
        const bool bridge = queried && name && wcscmp(name + 1, L"pulse_explorer_bridge.exe") == 0;
        std::fprintf(stderr, "[PRODUCTION_ROUTE] pid=%lu bridge_image_verified=%d\n", result ? pi->dwProcessId : 0, bridge);
        SetLastError(error); return result;
    }
    pulse::explorer_launch::Request request;
    const bool parsed = pulse::explorer_launch::Resolve(app, cmd, flags, request);
    std::fprintf(stderr, "[UNHANDLED] parsed=%d flags=%lu inherit=%d pa=%d ta=%d cb=%lu desktop=%d title=%d reserved=%u shift=%d\n",
        parsed, flags, inherit, !!pa, !!ta, si ? si->cb : 0, si && !!si->lpDesktop,
        si && !!si->lpTitle, si ? si->cbReserved2 : 0, !!(GetAsyncKeyState(VK_SHIFT)&0x8000));
    // Test probe must never accidentally launch Explorer when interception misses.
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE; // This dedicated probe has no permitted unhandled child launches.
}
}
extern "C" __declspec(dllexport) void* napi_register_module_v1(void*, void* exports) {
    wchar_t bridge[32768]{};
    const DWORD length = GetEnvironmentVariableW(L"PULSE_PRODUCTION_PROBE_BRIDGE", bridge, ARRAYSIZE(bridge));
    DWORD config = length && length < ARRAYSIZE(bridge) ?
        pulse::shell_bridge::explorer_process::ConfigureForTest(bridge, Gate) : ERROR_INVALID_PARAMETER;
    wchar_t mode[8]{};
    if (GetEnvironmentVariableW(L"PULSE_PRODUCTION_PROBE_AUTOLOAD", mode, ARRAYSIZE(mode)) && wcscmp(mode, L"1") == 0) {
        // Exercise the installed COM bootstrap and production configuration. The
        // subsequent Detour is observation only; it does not rewrite the request.
        (void)CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        IShellLinkW* link = nullptr;
        const HRESULT created = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
        if (link) link->Release();
        bool ready = false;
        const auto deadline = GetTickCount64() + 10000;
        do {
            const auto core = GetModuleHandleW(L"PulseDialogCore64.dll");
            const auto query = core ? reinterpret_cast<BOOL (WINAPI*)()>(GetProcAddress(core, "PulseIsExplorerProcessInstalled")) : nullptr;
            if (query && query()) { ready = true; break; }
            Sleep(25);
        } while (GetTickCount64() < deadline);
        std::fprintf(stderr, "[INSTALLED_CORE] com=0x%08lx ready=%d\n", static_cast<unsigned long>(created), ready);
        if (!ready) { TerminateProcess(GetCurrentProcess(), ERROR_NOT_READY); return exports; }
        observe_installed = true; config = ERROR_SUCCESS;
        pulse::shell_bridge::explorer_process::Disable();
    }
    LONG status = static_cast<LONG>(config);
    if (!status) {
        status = DetourTransactionBegin();
        if (!status) {
            status = DetourUpdateThread(GetCurrentThread());
            if (!status) status = DetourAttach(reinterpret_cast<PVOID*>(&original), reinterpret_cast<PVOID>(&Create));
            if (!status) status = DetourTransactionCommit(); else DetourTransactionAbort();
        }
    }
    std::fprintf(stderr, "[PRODUCTION_HOOK] status=%ld\n", status);
    return exports;
}

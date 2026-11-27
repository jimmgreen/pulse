// Test-only Node addon. Explicit require() installs outside DllMain; no injection.
#include "route.h"
#include <detours.h>
#include <cstdio>
#include <atomic>
namespace {
decltype(&CreateProcessW) original = &CreateProcessW;
std::wstring bridge, endpoint;
std::atomic<unsigned> redirected{0};
std::wstring Env(const wchar_t* key) {
    wchar_t value[32768]{};
    const DWORD count = GetEnvironmentVariableW(key, value, ARRAYSIZE(value));
    return count && count < ARRAYSIZE(value) ? std::wstring(value, count) : L"";
}
BOOL WINAPI Create(LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta,
    BOOL inherit, DWORD flags, LPVOID environment, LPCWSTR cwd, LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi) {
    const DWORD incoming = GetLastError();
    try {
        explorer_probe::Request request;
        if (si && pi && inherit && si->cb == sizeof(STARTUPINFOW) && !bridge.empty() &&
            explorer_probe::Resolve(app, cmd, flags, request)) {
            std::wstring redirected_command = explorer_probe::Quote(bridge) + L" --dispatch " +
                explorer_probe::Quote(endpoint) + (request.select ? L" select " : L" folder ") +
                explorer_probe::Quote(request.path) + L" " + explorer_probe::Quote(app ? app : L"") +
                L" " + explorer_probe::Quote(cmd);
            if (redirected_command.size() < 32767) {
                SetLastError(incoming);
                const BOOL ok = original(bridge.c_str(), redirected_command.data(), pa, ta, inherit,
                    flags, environment, cwd, si, pi);
                const DWORD error = GetLastError();
                // No IPC has happened if helper creation failed: original call
                // remains possible without a duplicate Pulse request.
                if (ok) {
                    ++redirected;
                    wchar_t image[32768]{}; DWORD size = ARRAYSIZE(image);
                    const BOOL queried = QueryFullProcessImageNameW(pi->hProcess, 0, image, &size);
                    const bool verified = queried && !_wcsicmp(image, bridge.c_str());
                    std::fprintf(stderr, "[REDIRECT] pid=%lu flags=%lu inherit=%d bridge_image_verified=%d\n",
                        pi->dwProcessId, flags, inherit, verified);
                    SetLastError(error); return ok;
                }
            }
        }
    } catch (...) { /* Allocation failure leaves the original request intact. */ }
    SetLastError(incoming);
    return original(app, cmd, pa, ta, inherit, flags, environment, cwd, si, pi);
}
}
// N-API opaque types: no dependency on a downloaded Node SDK. This entry point
// needs no N-API functions and returns the supplied exports unchanged.
extern "C" __declspec(dllexport) void* napi_register_module_v1(void*, void* exports) {
    bridge = Env(L"PULSE_EXPLORER_PROBE_BRIDGE");
    endpoint = Env(L"PULSE_EXPLORER_PROBE_ENDPOINT");
    // Explicitly private endpoint only: never target the user's live instance.
    if (bridge.empty() || !endpoint.starts_with(L"Local\\Pulse.ExplorerProbe.")) {
        std::fprintf(stderr, "[HOOK] invalid private configuration\n"); return exports;
    }
    LONG status = DetourTransactionBegin();
    if (status == NO_ERROR) {
        status = DetourUpdateThread(GetCurrentThread());
        if (status == NO_ERROR) status = DetourAttach(reinterpret_cast<PVOID*>(&original), reinterpret_cast<PVOID>(&Create));
        if (status == NO_ERROR) status = DetourTransactionCommit(); else DetourTransactionAbort();
    }
    std::fprintf(stderr, "[HOOK] status=%ld\n", status);
    return exports;
}

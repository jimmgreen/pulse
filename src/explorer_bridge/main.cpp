#include "dispatch.h"
#include <shellapi.h>
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int count = 0;
    wchar_t** args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!args) return ERROR_INVALID_PARAMETER;
    const DWORD result = pulse::explorer_bridge::Dispatch(count, args,
        pulse::explorer_bridge::Send, pulse::explorer_bridge::LaunchNativeExplorer);
    LocalFree(args);
    return static_cast<int>(result);
}

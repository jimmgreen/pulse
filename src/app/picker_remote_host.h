#pragma once
#include <windows.h>
#include <string>
namespace pulse::app {
int RunPickerRemoteHost(const std::wstring& pipe, DWORD parent_pid);
// Persistent pre-warmed V2 remote picker host (--picker-warm-remote-host).
// Serves sequential V2 remote dialog sessions over WarmRemoteServer without respawning pulse.exe.
int RunPickerWarmRemoteHost(const std::wstring& override_pipe = {},
                            const std::wstring& stop_event_name = {});
}

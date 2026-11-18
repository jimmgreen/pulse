#pragma once
#include <windows.h>
#include <string>
namespace pulse::app {
int RunPickerRemoteHost(const std::wstring& pipe, DWORD parent_pid);
}

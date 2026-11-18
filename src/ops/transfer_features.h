#pragma once
#include <windows.h>
namespace pulse::ops {
inline bool TransferFeatureEnabled(const wchar_t* name, bool default_value = true) {
    wchar_t value[8]{};
    const DWORD size = GetEnvironmentVariableW(name, value, 8);
    return size == 1 && (value[0] == L'0' || value[0] == L'1') ? value[0] == L'1' : default_value;
}
}

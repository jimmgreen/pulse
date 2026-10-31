#include "../common/module_location.h"
#ifdef _M_IX86
#pragma comment(linker, "/EXPORT:PulseModuleDirectory=_PulseModuleDirectory@8")
#endif
extern "C" __declspec(dllexport) DWORD WINAPI PulseModuleDirectory(wchar_t* directory, DWORD capacity) {
    const auto address = +[] {};
    return pulse::module_location::DirectoryFromAddress(reinterpret_cast<const void*>(address), directory, capacity);
}

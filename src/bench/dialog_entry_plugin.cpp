#include <windows.h>
#include <shobjidl.h>
#include <cstring>
#if defined(PULSE_ENTRY_DELAY)
#include <delayimp.h>
namespace {
LONG resolutions = 0;
FARPROC WINAPI ObserveDelay(unsigned notification, PDelayLoadInfo info) {
    if (notification == dliNotePreGetProcAddress && info && info->dlp.fImportByName &&
        std::strcmp(info->dlp.szProcName, "CoCreateInstance") == 0) InterlockedIncrement(&resolutions);
    return nullptr; // Observe the real delay helper; never replace an API.
}
}
extern "C" const PfnDliHook __pfnDliNotifyHook2 = ObserveDelay;
#endif

// Explicit aliases make GetProcAddress identical on x86 and x64.
#if defined(_M_IX86)
#pragma comment(linker, "/EXPORT:PulseEntryCreate=_PulseEntryCreate")
#pragma comment(linker, "/EXPORT:PulseEntryDelayResolutions=_PulseEntryDelayResolutions")
#else
#pragma comment(linker, "/EXPORT:PulseEntryCreate")
#pragma comment(linker, "/EXPORT:PulseEntryDelayResolutions")
#endif
extern "C" HRESULT __cdecl PulseEntryCreate(BOOL save, IFileDialog** dialog) {
    return CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog));
}
extern "C" LONG __cdecl PulseEntryDelayResolutions() {
#if defined(PULSE_ENTRY_DELAY)
    return InterlockedCompareExchange(&resolutions, 0, 0);
#else
    return -1;
#endif
}

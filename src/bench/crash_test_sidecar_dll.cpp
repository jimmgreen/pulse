#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {
volatile int* g_sidecar_crash_sink = nullptr;
} // namespace

extern "C" __declspec(dllexport) int SidecarPing() {
    return 7;
}

extern "C" __declspec(noinline) __declspec(dllexport) void TriggerSidecarCrash() {
    volatile int* target = g_sidecar_crash_sink;
    *target = 0xBEEF;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

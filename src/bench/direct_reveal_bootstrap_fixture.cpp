#include <windows.h>
extern "C" __declspec(dllexport) DWORD WINAPI PulseInitializeDirectReveal(void*) {
    // Deterministic evidence that the second remote phase actually ran.
    return 1234;
}
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }

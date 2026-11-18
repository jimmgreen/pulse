#include "../ui/optional_lumatext.h"
extern "C" __declspec(dllimport) int __cdecl PulseLumaFixtureValue();
#ifdef _M_IX86
#pragma comment(linker, "/EXPORT:PulseLumaBindingProbe=_PulseLumaBindingProbe")
#pragma comment(linker, "/EXPORT:PulseLumaBindingTouch=_PulseLumaBindingTouch")
#endif
extern "C" __declspec(dllexport) BOOL __cdecl PulseLumaBindingProbe(int* value) {
    if (!value) return FALSE;
    *value = 0;
    GetDesktopWindow(); // Ensure user32 availability, without creating a window.
    const bool available = pulse::ui::LoadOptionalLumaText();
    if (available) *value = PulseLumaFixtureValue();
    return available;
}
extern "C" __declspec(dllexport) int __cdecl PulseLumaBindingTouch() { return PulseLumaFixtureValue(); }

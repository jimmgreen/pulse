#include <windows.h>
#ifndef PULSE_LUMA_FIXTURE_VALUE
#define PULSE_LUMA_FIXTURE_VALUE 71
#endif
#ifdef _M_IX86
#pragma comment(linker, "/EXPORT:PulseLumaFixtureValue=_PulseLumaFixtureValue")
#endif
extern "C" __declspec(dllexport) int __cdecl PulseLumaFixtureValue() { return PULSE_LUMA_FIXTURE_VALUE; }

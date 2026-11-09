#ifndef PULSE_NATIVE_UI_H
#define PULSE_NATIVE_UI_H
#include "abi.h"
#ifdef __cplusplus
extern "C" {
#endif
#pragma pack(push, 8)
/* Common UI types. All calls and callbacks use the owner UI thread. */
typedef struct PulseUiState { PulseStringView key; PulseStringView value; } PulseUiState;
typedef void (PULSE_CALL *PulseUiClickFn)(void* user, uint64_t panel, PulseStringView command);
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
#endif

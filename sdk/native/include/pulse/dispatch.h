#ifndef PULSE_NATIVE_DISPATCH_H
#define PULSE_NATIVE_DISPATCH_H
#include "abi.h"
#ifdef __cplusplus
extern "C" {
#endif
#pragma pack(push, 8)
#define PULSE_DISPATCH_INTERFACE "pulse.dispatch"
#define PULSE_DISPATCH_VERSION 1u
#define PULSE_DISPATCH_MAX_PENDING 128u
typedef void (PULSE_CALL *PulseDispatchCallback)(void* user);
typedef struct PulseDispatchV1 {
    uint32_t size;
    uint32_t version;
    void* context;
    /* Any thread. At most 128 pending callbacks. A full/stopped queue returns STATE.
       Callback runs on the owner UI thread, possibly before post returns. Tokens
       are nonzero and never reused during this host process. No exception crosses ABI. */
    PulseStatus (PULSE_CALL *post)(void* context, PulseDispatchCallback callback, void* user, uint64_t* token);
    /* Any thread. OK guarantees this callback cannot subsequently begin: user data
       can be released. STATE means unknown, completed, or already running; it is
       NOT permission to free data that a running callback might still use. */
    PulseStatus (PULSE_CALL *cancel)(void* context, uint64_t token);
} PulseDispatchV1;
/* Host stop suppresses pending work. Callbacks are not nested by this dispatcher.
   The plugin owns callback/user lifetimes and must join its own worker threads
   before destroy returns. The table context remains valid through plugin destroy.
   Query and host Close run on the UI thread; post/cancel are the exception to the
   normal UI-only SDK call rule. This interface carries no AppState, HWND or JSON. */
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
#endif

#ifndef PULSE_NATIVE_UI_EVENTS_H
#define PULSE_NATIVE_UI_EVENTS_H
#include "abi.h"
#ifdef __cplusplus
extern "C" {
#endif
#define PULSE_UI_EVENTS_INTERFACE_NAME "pulse.ui.events"
#define PULSE_UI_EVENTS_VERSION_1 1u
/* UI-thread API. Changes are delivered after control notifications return,
   coalesced per key to its final value in that message turn. Sources are ORed.
   No initial snapshot is sent. key empty subscribes all declared keys.
   USER means direct editable-control commit (including accessibility); local
   actions, data outputs and set_states are PROGRAMMATIC. IME preedit is omitted.
   Each change and batch carries a monotonic state revision. Strings/arrays are
   borrowed only until callback return. Copy with the DLL allocator; never free.
   No exception may cross the ABI. Callback set_states is queued for a later
   batch, never recursively invoked. Unsubscribe/close invalidate queued delivery.
   Successful tree updates discard pre-update batches but retain subscriptions
   for surviving state keys and all-key subscriptions. Deleted keys retire their
   tokens. An update within a callback stops remaining listeners of that old batch.
   Tokens can also
   be removed through pulse.ui.model unsubscribe. At most 128 total subscriptions.
   Table/context follows the pulse.ui attachment lifetime. */
#pragma pack(push, 8)
enum { PULSE_UI_STATE_USER = 1u, PULSE_UI_STATE_PROGRAMMATIC = 2u, PULSE_UI_STATE_ALL = 3u };
typedef struct PulseUiStateChange {
    PulseStringView key;
    PulseStringView value;
    uint64_t revision;
    uint32_t source_mask;
    uint32_t reserved;
} PulseUiStateChange;
typedef struct PulseUiStateBatch {
    uint64_t revision;
    const PulseUiStateChange *changes;
    uint32_t count;
    uint32_t reserved;
} PulseUiStateBatch;
typedef void(PULSE_CALL *PulseUiStateChangedFn)(void *user, uint64_t panel, const PulseUiStateBatch *batch);
typedef struct PulseUiEventsV1 {
    uint32_t size;
    uint32_t version;
    void *context;
    PulseStatus(PULSE_CALL *subscribe_state)(void *context, uint64_t panel, PulseStringView key, uint32_t source_mask,
                                             PulseUiStateChangedFn callback, void *user, uint64_t *token);
    PulseStatus(PULSE_CALL *unsubscribe)(void *context, uint64_t token);
} PulseUiEventsV1;
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
#endif

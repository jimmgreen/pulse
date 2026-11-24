#ifndef PULSE_NATIVE_FILEEVENTS_H
#define PULSE_NATIVE_FILEEVENTS_H
#include "abi.h"
#ifdef __cplusplus
extern "C" {
#endif
#define PULSE_FILEEVENTS_INTERFACE_NAME "pulse.fileevents"
#define PULSE_FILEEVENTS_VERSION_1 1u
#pragma pack(push, 8)
typedef uint64_t PulseFileEventSubscription;
/* What a finished file operation did. */
enum {
    PULSE_FILE_EVENT_COPY = 1u,
    PULSE_FILE_EVENT_MOVE = 2u,
    PULSE_FILE_EVENT_RENAME = 3u,           /* single and batch rename */
    PULSE_FILE_EVENT_RECYCLE = 4u,          /* deleted to the Recycle Bin */
    PULSE_FILE_EVENT_DELETE = 5u,           /* deleted permanently */
    PULSE_FILE_EVENT_CREATE_FOLDER = 6u,
    PULSE_FILE_EVENT_CREATE_FILE = 7u,
    PULSE_FILE_EVENT_RESTORE = 8u,          /* restored from the Recycle Bin */
    PULSE_FILE_EVENT_EMPTY_RECYCLE = 9u
};
/* subscribe kinds mask: 1u << PULSE_FILE_EVENT_*; 0 = every kind. */
#define PULSE_FILE_EVENT_MASK(kind) (1u << (kind))
#define PULSE_FILE_EVENT_ALL_KINDS 0x3FEu
/* One item. COPY / MOVE / RENAME / RESTORE: both paths. RECYCLE / DELETE / EMPTY_RECYCLE:
   source only. CREATE_*: destination only. Absolute UTF-8 paths, borrowed for the callback;
   no "\\?\" prefix unless the path is MAX_PATH (260) characters or longer. */
typedef struct PulseFileEventItem {
    uint32_t size; /* sizeof(PulseFileEventItem) */
    uint32_t reserved;
    PulseStringView source;
    PulseStringView destination;
} PulseFileEventItem;
typedef struct PulseFileEvent {
    uint32_t size; /* sizeof(PulseFileEvent) */
    uint32_t kind; /* PULSE_FILE_EVENT_* */
    uint64_t sequence; /* increases by one per event the host publishes, across all subscribers */
    const PulseFileEventItem* items;
    uint32_t item_count; /* items delivered, at most 1024 */
    uint32_t total_count; /* items the operation changed; > item_count when truncated */
} PulseFileEvent;
typedef void(PULSE_CALL *PulseFileEventFn)(void* user, const PulseFileEvent* event);
/* Read-only notifications of file operations Pulse ran (paste, drag, rename, delete, new
   folder, undo / redo, plugin pulse.fileops requests...), after they took effect. Only items
   that really changed are listed: failed, skipped or cancelled items are not. Changes made
   outside Pulse are not reported (use pulse.directories watch for that).
   All functions and callbacks run on the owner UI thread. Events are delivered once the host
   has updated its views, in order, never batched across operations. Strings are borrowed
   through the callback; copy them before returning and do not start blocking work there.
   Up to 8 subscriptions per session. unsubscribe returns after a running callback for the
   subscription returned. Closing the session removes every subscription. */
typedef struct PulseFileEventsV1 {
    uint32_t size, version;
    void* context;
    PulseStatus(PULSE_CALL *subscribe)(void* context, uint32_t kinds, PulseFileEventFn callback, void* user,
        PulseFileEventSubscription* out_subscription);
    PulseStatus(PULSE_CALL *unsubscribe)(void* context, PulseFileEventSubscription subscription);
} PulseFileEventsV1;
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
#endif

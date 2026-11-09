#ifndef PULSE_NATIVE_CONTEXT_H
#define PULSE_NATIVE_CONTEXT_H
#include "abi.h"
#ifdef __cplusplus
extern "C" {
#endif
#define PULSE_CONTEXT_INTERFACE_NAME "pulse.context"
#define PULSE_CONTEXT_VERSION_1 1u
/* UI thread only. Declarations do not gate calls.
   Observation uses the owner's already loaded state; get/subscribe never enumerate files or
   selections. Changes are coalesced at the host's 30 ms session tick, not a
   lossless event journal. Loading clears selection summary. Revision fields
   are monotonic per attachment; a multi-selection change is observable even
   when its count/name summary is unchanged. Initial delivery is deferred.
   Strings are borrowed through the callback; copy before returning.
   Unsubscribe/stop suppress future callbacks; on-stack callbacks may finish.
   16 subscriptions per attachment. Context lives through plugin destroy.
   No C++ exception may cross an ABI callback. */
#pragma pack(push, 8)
typedef uint64_t PulseContextSubscriptionId;
enum { PULSE_CONTEXT_INITIAL = 1u, PULSE_CONTEXT_NAVIGATION = 2u,
       PULSE_CONTEXT_SELECTION = 4u, PULSE_CONTEXT_VIEW = 8u };
typedef struct PulseContextSnapshot {
    uint32_t size;
    uint32_t changes;
    uint64_t revision;
    uint64_t navigation_revision;
    uint64_t selection_revision;
    PulseStringView directory;
    PulseStringView selection_name;
    PulseStringView view_mode;
    uint64_t selection_count;
    uint32_t loading;
    uint32_t reserved;
} PulseContextSnapshot;
typedef void(PULSE_CALL *PulseContextChangeFn)(void *, const PulseContextSnapshot *);
enum { PULSE_CONTEXT_ENTRY_DIRECTORY = 1u };
typedef struct PulseContextSelectionEntry {
    uint32_t size;
    uint32_t flags;
    PulseStringView path;
    PulseStringView name;
} PulseContextSelectionEntry;
typedef struct PulseContextSelectionSnapshot {
    uint32_t size;
    uint64_t selection_revision;
    PulseStringView directory;
    const PulseContextSelectionEntry *entries;
    uint32_t entry_count;
} PulseContextSelectionSnapshot;
typedef void(PULSE_CALL *PulseContextSelectionFn)(void *, const PulseContextSelectionSnapshot *);

typedef struct PulseContextV1 {
    uint32_t size;
    uint32_t version;
    void *context;
    /* get calls back synchronously with the latest loaded context. It never
       retains the callback or user pointer; use subscribe for deferred events. */
    PulseStatus(PULSE_CALL *get)(void *, PulseContextChangeFn, void *);
    PulseStatus(PULSE_CALL *subscribe)(void *, PulseContextChangeFn, void *, PulseContextSubscriptionId *);
    PulseStatus(PULSE_CALL *unsubscribe)(void *, PulseContextSubscriptionId);
    /* Synchronous loaded selection, no disk access. Borrowed only during callback.
       Ordinary directories support an empty selection. Loading returns STATE;
       virtual/content views or >4096 entries / >4 MiB UTF-8 return UNAVAILABLE.
       No partial delivery or recursive callback reads. */
    PulseStatus(PULSE_CALL *get_selection)(void *, PulseContextSelectionFn, void *);
} PulseContextV1;
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
#endif

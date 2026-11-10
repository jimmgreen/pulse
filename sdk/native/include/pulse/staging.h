#ifndef PULSE_NATIVE_STAGING_H
#define PULSE_NATIVE_STAGING_H
#include "abi.h"
#ifdef __cplusplus
extern "C" {
#endif
#define PULSE_STAGING_INTERFACE_NAME "pulse.staging"
#define PULSE_STAGING_VERSION_1 1u
#pragma pack(push, 8)
typedef uint64_t PulseStageId;
typedef uint32_t PulseStageState;
enum { PULSE_STAGE_PREPARING = 0u, PULSE_STAGE_READY = 1u,
    PULSE_STAGE_COMMITTING = 2u, PULSE_STAGE_RUNNING = 3u,
    PULSE_STAGE_COMPLETED = 4u, PULSE_STAGE_PARTIAL = 5u,
    PULSE_STAGE_FAILED = 6u, PULSE_STAGE_CANCELLED = 7u };
typedef struct PulseStageSnapshot {
    uint32_t size;
    PulseStageId id;
    uint64_t task_id;
    PulseStageState state;
    uint32_t percent;
    uint64_t completed_items, total_items, completed_bytes, total_bytes;
    PulseStringView error;
} PulseStageSnapshot;
typedef void(PULSE_CALL *PulseStageChooseFn)(void*, PulseStatus, PulseStringView);
typedef void(PULSE_CALL *PulseStageCreateFn)(void*, PulseStatus, PulseStageId, PulseStringView);
typedef void(PULSE_CALL *PulseStageSnapshotFn)(void*, const PulseStageSnapshot*);
typedef struct PulseStagingV1 {
    uint32_t size, version;
    void* context;
    PulseStatus(PULSE_CALL *create)(void*, PulseStringView destination, PulseStageCreateFn, void*);
    PulseStatus(PULSE_CALL *commit)(void*, PulseStageId);
    PulseStatus(PULSE_CALL *get)(void*, PulseStageId, PulseStageSnapshotFn, void*);
    PulseStatus(PULSE_CALL *cancel)(void*, PulseStageId);
    PulseStatus(PULSE_CALL *release)(void*, PulseStageId);
    PulseStatus(PULSE_CALL *choose_destination)(void*, PulseStringView initial_path, PulseStageChooseFn, void*);
} PulseStagingV1;
/* All calls except choose_destination may originate on plugin workers.
   choose_destination runs synchronously on the owner UI thread and returns a
   borrowed UTF-8 destination through its callback (empty initial_path allowed). create returns acceptance and later
   calls back on a host worker with a same-volume private staging directory.
   Callback path/error views are borrowed only through callback return. get calls
   back synchronously on the calling thread. create/commit/release disk work runs
   on host workers.
   <=16 stages; destination UTF-8 <=131072 bytes. Only host-created IDs may be
   committed/cleaned. Finish and join all writers before commit/release. commit
   validates the complete ordinary tree then queues normal owner confirmation
   and file operations; acceptance is not completion. Empty stages, reparse
   trees, repeated commit and path replacement are rejected. Commit tree budget:
   <=2,000,000 entries, <=4096 top-level items and <=1 GiB UTF-8 paths.
   release accepts unsubmitted or terminal stages and asynchronously cleans them.
   Stop rejects create/commit but get/cancel/release remain valid through destroy.
   Stop never deletes a directory while plugin writers may still run. Remaining
   cleanup starts only after plugin destroy. Cancellation is not rollback.
   Callbacks must not throw across ABI. Native code is trusted, not sandboxed. */
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
#endif

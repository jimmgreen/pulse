#ifndef PULSE_NATIVE_TASKS_H
#define PULSE_NATIVE_TASKS_H
#include "abi.h"
#ifdef __cplusplus
extern "C" {
#endif
#define PULSE_TASKS_INTERFACE_NAME "pulse.tasks"
#define PULSE_TASKS_VERSION_1 1u
#pragma pack(push, 8)
typedef uint64_t PulseTaskId;
typedef uint32_t PulseTaskState;
enum { PULSE_TASK_RUNNING = 0u, PULSE_TASK_CANCELLING = 1u,
       PULSE_TASK_COMPLETED = 2u, PULSE_TASK_PARTIAL = 3u,
       PULSE_TASK_FAILED = 4u, PULSE_TASK_CANCELLED = 5u };
typedef struct PulseTaskProgress {
    uint32_t size, percent;
    uint64_t completed_items, total_items, completed_bytes, total_bytes;
    PulseStringView error, current_item;
} PulseTaskProgress;
typedef struct PulseTaskContext {
    PulseTaskId id;
    void* context;
    PulseStatus(PULSE_CALL *report)(void*, const PulseTaskProgress*);
    uint32_t(PULSE_CALL *cancelled)(void*);
} PulseTaskContext;
typedef PulseTaskState(PULSE_CALL *PulseTaskWorkFn)(void*, const PulseTaskContext*);
typedef void(PULSE_CALL *PulseTaskCancelFn)(void*, PulseTaskId);
typedef struct PulseTaskDescriptor {
    uint32_t size;
    PulseStringView title, type;
    PulseTaskWorkFn work;
    void* user;
    PulseTaskCancelFn on_cancel;
} PulseTaskDescriptor;
typedef struct PulseTaskSnapshot {
    uint32_t size;
    PulseTaskId id;
    PulseTaskState state;
    PulseTaskProgress progress;
} PulseTaskSnapshot;
typedef void(PULSE_CALL *PulseTaskSnapshotFn)(void*, const PulseTaskSnapshot*);
typedef struct PulseTasksV1 {
    uint32_t size, version;
    void* context;
    PulseStatus(PULSE_CALL *create)(void*, const PulseTaskDescriptor*, PulseTaskId*);
    PulseStatus(PULSE_CALL *cancel)(void*, PulseTaskId);
    PulseStatus(PULSE_CALL *get)(void*, PulseTaskId, PulseTaskSnapshotFn, void*);
    PulseStatus(PULSE_CALL *release)(void*, PulseTaskId);
} PulseTasksV1;
/* create/cancel: owner UI thread. Title/type: 1..128 UTF-8 bytes, copied.
   At most16 retained tasks, two concurrent host-owned workers; others queued.
   work returns COMPLETED/PARTIAL/FAILED/CANCELLED; return is final. report is
   any-thread, synchronously copies error (<=4096 UTF-8 bytes) and progress.
   Current item <=32768 UTF-8 bytes. Percent UINT32_MAX means indeterminate;
   otherwise 0..100, completed <= totals, monotonic completed counters/percent.
   Report/context are valid only until work returns: join own reporting threads.
   get/release are UI-thread only. get borrows strings through its callback.
   Keep userdata until get observes terminal/release succeeds or plugin destroy;
   work must not free it, since on_cancel may still be pending. Oldest terminal
   tasks may be evicted when creating at capacity; IDs are never reused.
   Terminal state is immutable; report remains valid during cancellation cleanup until work returns; the
   cancellation flag is independent of progress. A FAILED return preserves
   cleanup diagnostics rather than being replaced with CANCELLED.
   cancel signals once; optional on_cancel is delivered on owner UI Poll, at
   most once, and must return promptly. Keep userdata through task completion
   and any cancellation callback, or through destroy. No exceptions cross ABI.
   Stop cancels queued/running work; host keeps DLL loaded until work and UI
   cancellation callbacks return. Cancellation is cooperative, not rollback. */
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
#endif

#ifndef PULSE_NATIVE_TASKS_H
#define PULSE_NATIVE_TASKS_H
#include "abi.h"
#include <stddef.h>
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
/* A button the host shows once the task has ended (retry, open, ...). */
typedef struct PulseTaskAction {
    uint32_t size, flags; /* PULSE_TASK_ACTION_PRIMARY: the default button. */
    PulseStringView id;   /* ASCII [A-Za-z0-9._-], 1..64 bytes; passed back to on_action. */
    PulseStringView text; /* UTF-8, 1..64 bytes. */
} PulseTaskAction;
enum { PULSE_TASK_ACTION_PRIMARY = 1u };
typedef struct PulseTaskProgress {
    uint32_t size, percent;
    uint64_t completed_items, total_items, completed_bytes, total_bytes;
    PulseStringView error, current_item;
    /* Optional, read only when size covers them (older reports end at current_item). */
    uint32_t step; /* 1-based index into the descriptor's steps; 0 keeps the last one. */
    uint32_t action_count; /* <=3 actions, shown when the task has ended. */
    uint64_t linked_operation; /* A host file operation this task is waiting on (for example
                                  a staging commit's task_id): the host shows this task's
                                  progress in its place. 0 = none. */
    PulseStringView note; /* UTF-8, 0..256 bytes: a footer hint ("nothing changes until ..."). */
    PulseStringView detail; /* UTF-8, 0..1024 bytes: explains the error (may hold line breaks), or
                               on COMPLETED the result notice's second line. */
    PulseStringView assurance; /* UTF-8, 0..256 bytes: what stayed safe after a failure. */
    const PulseTaskAction* actions;
    PulseStringView log; /* UTF-8, 0..2048 bytes: diagnostics shown in a monospace block and
                            included when the user copies the failure's details. */
    PulseStringView result; /* UTF-8, 0..256 bytes: on COMPLETED, a notice ("Extracted to X") with
                               open and undo buttons; empty shows none. */
    PulseStringView result_path; /* UTF-8, 0..32768 bytes: the folder or file the task produced;
                                    the notice's open button shows it. */
} PulseTaskProgress;
#define PULSE_TASK_PROGRESS_SIZE_V1 ((uint32_t)offsetof(PulseTaskProgress, step))
typedef struct PulseTaskContext {
    PulseTaskId id;
    void* context;
    PulseStatus(PULSE_CALL *report)(void*, const PulseTaskProgress*);
    uint32_t(PULSE_CALL *cancelled)(void*);
} PulseTaskContext;
typedef PulseTaskState(PULSE_CALL *PulseTaskWorkFn)(void*, const PulseTaskContext*);
typedef void(PULSE_CALL *PulseTaskCancelFn)(void*, PulseTaskId);
typedef void(PULSE_CALL *PulseTaskActionFn)(void*, PulseTaskId, PulseStringView);
typedef struct PulseTaskDescriptor {
    uint32_t size;
    PulseStringView title, type;
    PulseTaskWorkFn work;
    void* user;
    PulseTaskCancelFn on_cancel;
    /* Optional, read only when size covers them (older descriptors end at on_cancel). */
    PulseStringView source, destination; /* UTF-8, 0..1024 bytes: the route line. */
    const PulseStringView* steps; /* <=3 step names (UTF-8, 1..64 bytes), shown in order. */
    uint32_t step_count;
    PulseTaskActionFn on_action; /* Called with action_user, never user. */
    void* action_user; /* Must stay valid until plugin destroy: actions outlive release. */
} PulseTaskDescriptor;
#define PULSE_TASK_DESCRIPTOR_SIZE_V1 ((uint32_t)offsetof(PulseTaskDescriptor, source))
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
   cancellation callbacks return. Cancellation is cooperative, not rollback.
   Actions come from the last report before the task ended. on_action runs on the
   owner UI thread when the user picks one (at most once per ended task, never after
   stop) and may create tasks; it must return promptly. */
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
#endif

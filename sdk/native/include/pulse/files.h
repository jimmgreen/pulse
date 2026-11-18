#ifndef PULSE_NATIVE_FILES_H
#define PULSE_NATIVE_FILES_H
#include "abi.h"
#ifdef __cplusplus
extern "C" {
#endif
#define PULSE_FILES_INTERFACE_NAME "pulse.files"
#define PULSE_FILES_VERSION_1 1u
/* QueryInterface copies PulseFilesV1 into caller-owned storage. All functions
   and callbacks use the owning host UI thread. PULSE_OK means accepted, not
   completed. Callbacks are deferred until after the initiating call returns.
   Inputs are copied during the initiating call. Every pointer in a reply is
   borrowed only until that callback returns: copy using the DLL's allocator.
   Never free host pointers. No C++ exception may cross any ABI callback.
   Each attachment admits at most 64 pending requests and tracks at most 256
   submitted tasks not yet observed terminal. Close suppresses callbacks and
   requests cancellation only for its own submitted tasks (operations.own is
   required); cancellation is best effort and never rolls back completed effects.
   A table/context is valid until its host attachment is destroyed. */
#pragma pack(push, 8)
typedef uint64_t PulseFilesRequestId;
enum {
    PULSE_FILES_CONTEXT = 1,
    PULSE_FILES_LIST = 2,
    PULSE_FILES_PREPARE_RENAME = 3,
    PULSE_FILES_COMMIT_RENAME = 4,
    PULSE_FILES_TASK_GET = 5,
    PULSE_FILES_TASK_CANCEL = 6
};
enum { PULSE_FILE_DIRECTORY = 1u, PULSE_FILE_REPARSE_POINT = 2u };
enum { PULSE_RENAME_UNCHANGED = 0u, PULSE_RENAME_READY = 1u };
enum {
    PULSE_FILE_TASK_NOT_STARTED = 0u,
    PULSE_FILE_TASK_COPIED = 1u,
    PULSE_FILE_TASK_MOVED = 2u,
    PULSE_FILE_TASK_FAILED = 3u,
    PULSE_FILE_TASK_CANCELLED = 4u,
    PULSE_FILE_TASK_DESTINATION_COMMITTED_SOURCE_RETAINED = 5u
};
typedef struct PulseDirectoryContext {
    PulseStringView directory;
    PulseStringView selection_name;
    PulseStringView view_mode; /* Manifest machine name; empty means unknown. */
    uint64_t selection_count;
    uint32_t loading;
    uint32_t reserved;
} PulseDirectoryContext;
typedef struct PulseFileEntry {
    PulseStringView entry; /* Opaque, expiring owner/package/snapshot-bound handle. */
    PulseStringView name;
    uint64_t size_bytes;
    uint64_t modified_filetime; /* 100-nanosecond ticks since 1601-01-01 UTC. */
    uint32_t attributes;
    uint32_t flags;
} PulseFileEntry;
typedef struct PulseFilePage {
    const PulseFileEntry *entries;
    uint32_t count;
    uint32_t reserved;
    PulseStringView next_cursor; /* Empty at end; never manufacture a cursor. */
    uint64_t total;
} PulseFilePage;
typedef struct PulseRenameInput {
    PulseStringView entry;
    PulseStringView new_leaf_name; /* UTF-8 Windows leaf only, never a path. */
} PulseRenameInput;
typedef struct PulseRenameRow {
    PulseStringView entry;
    PulseStringView original_name;
    PulseStringView new_leaf_name;
    uint32_t action;
    uint32_t reserved;
} PulseRenameRow;
typedef struct PulseRenamePlan {
    PulseStringView plan_id;
    uint64_t expires_in_ms;
    const PulseRenameRow *rows;
    uint32_t count;
    uint32_t reserved;
} PulseRenamePlan;
typedef struct PulseFileTaskItem {
    PulseStringView source;
    PulseStringView destination;
    uint32_t state;
    uint32_t system_error;
    uint32_t cleanup_error;
    uint32_t reserved;
} PulseFileTaskItem;
typedef struct PulseFileTask {
    uint64_t task_id;
    uint64_t transferred_bytes;
    uint64_t total_bytes;
    const PulseFileTaskItem *items;
    uint32_t count;
    uint32_t cancel_requested;
    uint32_t terminal;
    uint32_t completed;
} PulseFileTask;
typedef struct PulseFilesReply {
    uint32_t size;
    uint32_t operation;
    PulseFilesRequestId request_id;
    PulseStatus status;
    uint32_t cancel_accepted;
    PulseStringView error; /* Stable machine code on failure; not a UI label. */
    PulseDirectoryContext directory;
    PulseFilePage page;
    PulseRenamePlan plan;
    PulseFileTask task;
} PulseFilesReply;
typedef void(PULSE_CALL *PulseFilesCompletionFn)(void *user, const PulseFilesReply *reply);
typedef struct PulseFilesV1 {
    uint32_t size;
    uint32_t version;
    void *context;
    /* Requires context.read. */
    PulseStatus(PULSE_CALL *current_directory)(void *, PulseFilesCompletionFn, void *, PulseFilesRequestId *);
    /* Requires files.readCurrentDirectory. Empty cursor starts; limit 1..256. */
    PulseStatus(PULSE_CALL *list)(void *, PulseStringView cursor, uint32_t limit, PulseFilesCompletionFn, void *,
                                  PulseFilesRequestId *);
    /* Requires files.renameCurrentDirectory. 1..64 mappings. No writes here.
       At most 4 live plans per owner/package, 60-second expiry, one-use commit. */
    PulseStatus(PULSE_CALL *prepare_rename)(void *, const PulseRenameInput *, uint32_t count, PulseFilesCompletionFn,
                                            void *, PulseFilesRequestId *);
    /* Always opens the host's real native review. Never an unattended write.
       Success returns submitted task_id; query task_get for actual completion. */
    PulseStatus(PULSE_CALL *commit_rename)(void *, PulseStringView plan_id, PulseFilesCompletionFn, void *,
                                           PulseFilesRequestId *);
    /* Requires operations.own; caller may only query/cancel its own task. */
    PulseStatus(PULSE_CALL *task_get)(void *, uint64_t task_id, PulseFilesCompletionFn, void *, PulseFilesRequestId *);
    PulseStatus(PULSE_CALL *task_cancel)(void *, uint64_t task_id, PulseFilesCompletionFn, void *,
                                         PulseFilesRequestId *);
    /* Cancels a still-pending request and suppresses its callback. This is not
       task cancellation after commit completes: use task_cancel for that.
       On immediate API failure out_request is zero and no callback is made. */
    PulseStatus(PULSE_CALL *cancel_request)(void *, PulseFilesRequestId);
} PulseFilesV1;
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
#endif

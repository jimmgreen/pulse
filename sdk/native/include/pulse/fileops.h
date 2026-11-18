#ifndef PULSE_NATIVE_FILEOPS_H
#define PULSE_NATIVE_FILEOPS_H
#include "abi.h"
#include "directories.h"
#ifdef __cplusplus
extern "C" {
#endif
#define PULSE_FILEOPS_INTERFACE_NAME "pulse.fileops"
#define PULSE_FILEOPS_VERSION_1 1u
#define PULSE_FILEOPS_VERSION_2 2u
#define PULSE_FILEOPS_VERSION_3 3u
#define PULSE_FILEOPS_VERSION_4 4u
/* V1 controlled transfer actions use the current UI selection and the other
   visible pane as destination. Recycle-delete uses the current file selection
   and rejects sources that cannot be recycled. No plugin paths are accepted. Submit
   starts asynchronous host preparation and always presents Pulse's native
   confirmation before mutation. Query/cancel address only this attachment's
   requests. All calls are owner-UI-thread only; no exception crosses the ABI.
   Stop cancels this attachment's pending/running requests without waiting. */
#pragma pack(push, 8)
typedef uint64_t PulseFileOperationRequestId;
enum {
    PULSE_FILEOPS_COPY_SELECTION_TO_OTHER_PANE = 1u,
    PULSE_FILEOPS_MOVE_SELECTION_TO_OTHER_PANE = 2u,
    PULSE_FILEOPS_DELETE_SELECTION_TO_RECYCLE_BIN = 3u,
    PULSE_FILEOPS_COPY_SELECTION_TO_DIRECTORY = 4u,
    PULSE_FILEOPS_MOVE_SELECTION_TO_DIRECTORY = 5u,
    PULSE_FILEOPS_COPY_ENTRIES_TO_DIRECTORY = 6u,
    PULSE_FILEOPS_MOVE_ENTRIES_TO_DIRECTORY = 7u,
    PULSE_FILEOPS_COPY_DIRECTORY_TREE_TO_DIRECTORY = 8u
};
enum {
    PULSE_FILEOPS_PREPARING = 1u,
    PULSE_FILEOPS_WAITING_CONFIRMATION = 2u,
    PULSE_FILEOPS_QUEUED = 3u,
    PULSE_FILEOPS_RUNNING = 4u,
    PULSE_FILEOPS_CANCELLING = 5u,
    PULSE_FILEOPS_COMPLETED = 6u,
    PULSE_FILEOPS_PARTIAL = 7u,
    PULSE_FILEOPS_CANCELLED = 8u,
    PULSE_FILEOPS_FAILED = 9u
};
typedef struct PulseFileOperation {
    uint32_t size;
    uint32_t state;
    PulseFileOperationRequestId request_id;
    uint64_t task_id; /* Zero until Pulse has accepted the confirmed task. */
    uint64_t transferred_bytes;
    uint64_t total_bytes;
    uint64_t completed_items;
    uint64_t total_items;
    uint32_t percent; /* 0..100; zero while no determinate progress exists. */
    uint32_t cancel_requested;
    uint32_t terminal;
    uint32_t succeeded;
    uint32_t system_error;
    uint32_t reserved;
} PulseFileOperation;
typedef struct PulseFileOpsV1 {
    uint32_t size;
    uint32_t version;
    void *context;
    /* Copy/move require files.transferCurrentSelection and operations.own;
       delete requires files.deleteCurrentSelection and operations.own. All
       operations use the current selection and Pulse's native confirmation. */
    PulseStatus(PULSE_CALL *submit)(void *, uint32_t operation, PulseFileOperationRequestId *request_id);
    /* Synchronous owner-thread snapshot. Requires operations.own. */
    PulseStatus(PULSE_CALL *get)(void *, PulseFileOperationRequestId request_id, PulseFileOperation *out);
    /* Cancels preparation/confirmation or the accepted controlled task. */
    PulseStatus(PULSE_CALL *cancel)(void *, PulseFileOperationRequestId request_id);
} PulseFileOpsV1;
/* V2 preserves V1 unchanged. Query version 2 with sizeof(PulseFileOpsV2).
   submit retains the V1 actions; use submit_to_directory for the two new actions.
   Requires files.transferToDirectory, files.readDirectory and operations.own.
   destination must be a successfully opened pulse.directories handle belonging
   to this attachment. No source/destination paths or confirmation are accepted.
   Closing the handle before task submission cancels preparation/confirmation;
   after task submission, use cancel(request_id) to cancel execution.
   Directory handles resolve paths at worker preparation; object identity is
   checked by the controlled engine after preparation. Existing destinations
   fail without replacement. The request is not an atomic multi-file transaction;
   external changes during execution can still produce partial results.
   File-only limits and engine exclusions still apply. */
typedef struct PulseFileOpsV2 {
    uint32_t size;
    uint32_t version;
    void *context;
    PulseStatus(PULSE_CALL *submit)(void *, uint32_t operation, PulseFileOperationRequestId *request_id);
    PulseStatus(PULSE_CALL *get)(void *, PulseFileOperationRequestId request_id, PulseFileOperation *out);
    PulseStatus(PULSE_CALL *cancel)(void *, PulseFileOperationRequestId request_id);
    PulseStatus(PULSE_CALL *submit_to_directory)(void *, uint32_t operation, PulseDirectoryId destination,
        PulseFileOperationRequestId *request_id);
} PulseFileOpsV2;
/* V3 preserves V2 unchanged. Explicit source and destination handles and every
   entry ID must belong to this attachment; the snapshot must still be current.
   Only 1..64 ordinary, non-reparse files are accepted. Snapshot metadata is
   rechecked during worker preparation. Native confirmation remains mandatory.
   Closing either directory or replacing the source snapshot cancels preparation
   and confirmation. After submission, request cancellation uses cancel(). */
typedef struct PulseFileOpsV3 {
    uint32_t size;
    uint32_t version;
    void *context;
    PulseStatus(PULSE_CALL *submit)(void *, uint32_t operation, PulseFileOperationRequestId *request_id);
    PulseStatus(PULSE_CALL *get)(void *, PulseFileOperationRequestId request_id, PulseFileOperation *out);
    PulseStatus(PULSE_CALL *cancel)(void *, PulseFileOperationRequestId request_id);
    PulseStatus(PULSE_CALL *submit_to_directory)(void *, uint32_t operation, PulseDirectoryId destination,
        PulseFileOperationRequestId *request_id);
    PulseStatus(PULSE_CALL *submit_entries)(void *, uint32_t operation, PulseDirectoryId source,
        uint64_t snapshot_id, const uint64_t *entry_ids, uint32_t entry_count, PulseDirectoryId destination,
        PulseFileOperationRequestId *request_id);
} PulseFileOpsV3;
/* V4 adds copy-only recursive transfer of one ordinary directory entry from
   the exact current source snapshot. It creates a new destination directory
   named after the source leaf and never replaces or merges. get_item reports
   terminal per-file outcomes; path_utf8 is caller-owned output storage and
   path_bytes includes the trailing NUL. */
typedef struct PulseFileOperationItem {
    uint32_t size;
    uint32_t index;
    uint32_t state;
    uint32_t system_error;
    uint64_t transferred_bytes;
    uint32_t path_bytes;
    uint32_t reserved;
} PulseFileOperationItem;
enum {
    PULSE_FILEOPS_ITEM_PENDING = 1u,
    PULSE_FILEOPS_ITEM_COMPLETED = 2u,
    PULSE_FILEOPS_ITEM_CANCELLED = 3u,
    PULSE_FILEOPS_ITEM_FAILED = 4u
};
typedef struct PulseFileOpsV4 {
    uint32_t size;
    uint32_t version;
    void *context;
    PulseStatus(PULSE_CALL *submit)(void *, uint32_t operation, PulseFileOperationRequestId *request_id);
    PulseStatus(PULSE_CALL *get)(void *, PulseFileOperationRequestId request_id, PulseFileOperation *out);
    PulseStatus(PULSE_CALL *cancel)(void *, PulseFileOperationRequestId request_id);
    PulseStatus(PULSE_CALL *submit_to_directory)(void *, uint32_t operation, PulseDirectoryId destination,
        PulseFileOperationRequestId *request_id);
    PulseStatus(PULSE_CALL *submit_entries)(void *, uint32_t operation, PulseDirectoryId source,
        uint64_t snapshot_id, const uint64_t *entry_ids, uint32_t entry_count, PulseDirectoryId destination,
        PulseFileOperationRequestId *request_id);
    PulseStatus(PULSE_CALL *submit_directory_tree)(void *, PulseDirectoryId source, uint64_t snapshot_id,
        uint64_t entry_id, PulseDirectoryId destination, PulseFileOperationRequestId *request_id);
    PulseStatus(PULSE_CALL *get_item)(void *, PulseFileOperationRequestId request_id, uint32_t item_index,
        char *path_utf8, uint32_t path_capacity, PulseFileOperationItem *out);
} PulseFileOpsV4;
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
#endif

#ifndef PULSE_NATIVE_DIRECTORIES_H
#define PULSE_NATIVE_DIRECTORIES_H
#include "abi.h"
#ifdef __cplusplus
extern "C" {
#endif
#define PULSE_DIRECTORIES_INTERFACE_NAME "pulse.directories"
#define PULSE_DIRECTORIES_VERSION_1 1u
/* Additive native directory service. All calls and callbacks use the owner UI
   thread. Filesystem enumeration and watch I/O run on host workers. PULSE_OK
   accepts work; callbacks are deferred, and borrowed fields expire on return.
   Requires files.readDirectory; watch also requires files.watchDirectory;
   open_entry additionally requires files.open. A trusted DLL is not a sandbox.
   open accepts absolute drive/UNC paths, not device/virtual/relative paths.
   At most 4 directories, 32 pending requests and 8 watches per attachment.
   Snapshots contain at most 65536 rows / 16 MiB UTF-8 names, never truncated.
   Snapshot IDs and entry IDs are owner/directory/snapshot bound, not file IDs.
   Successful refresh replaces the snapshot; failed refresh preserves it.
   list pages use explicit offset (limit 1..256) against an immutable snapshot.
   Watch notifications are coalesced invalidations, not an exact change journal.
   After READY, refresh and repeat on INVALIDATED/overflow to avoid an arm gap.
   close/cancel/unwatch suppress future callbacks immediately; an on-stack
   callback may finish. Stop never waits for filesystem I/O on the UI thread.
   No exception may cross this ABI. Context expires after plugin destroy. */
#pragma pack(push, 8)
typedef uint64_t PulseDirectoryId;
typedef uint64_t PulseDirectoryRequestId;
typedef uint64_t PulseDirectoryWatchId;
enum { PULSE_DIRECTORY_OPEN = 1u, PULSE_DIRECTORY_REFRESH = 2u,
       PULSE_DIRECTORY_LIST = 3u, PULSE_DIRECTORY_OPEN_ENTRY = 4u };
enum { PULSE_DIRECTORY_WATCH_READY = 1u, PULSE_DIRECTORY_INVALIDATED = 2u,
       PULSE_DIRECTORY_WATCH_ERROR = 3u };
enum { PULSE_DIRECTORY_ENTRY_FOLDER = 1u, PULSE_DIRECTORY_ENTRY_REPARSE = 2u };
typedef struct PulseDirectoryRow {
    uint64_t entry_id;
    PulseStringView name;
    uint64_t size_bytes;
    uint64_t modified_filetime;
    uint32_t attributes;
    uint32_t flags;
} PulseDirectoryRow;
typedef struct PulseDirectoryReply {
    uint32_t size;
    uint32_t operation;
    PulseDirectoryRequestId request_id;
    PulseStatus status;
    uint32_t system_error;
    PulseStringView error;
    PulseDirectoryId directory_id;
    uint64_t snapshot_id;
    PulseStringView path;
    uint64_t total;
    const PulseDirectoryRow *rows;
    uint32_t count;
    uint32_t next_offset; /* UINT32_MAX when the immutable snapshot is exhausted. */
} PulseDirectoryReply;
typedef struct PulseDirectoryChange {
    uint32_t size;
    uint32_t kind;
    PulseDirectoryWatchId watch_id;
    PulseDirectoryId directory_id;
    uint64_t sequence;
    PulseStatus status;
    uint32_t overflow;
    PulseStringView error;
} PulseDirectoryChange;
typedef void(PULSE_CALL *PulseDirectoryCompletionFn)(void *, const PulseDirectoryReply *);
typedef void(PULSE_CALL *PulseDirectoryChangeFn)(void *, const PulseDirectoryChange *);
typedef struct PulseDirectoriesV1 {
    uint32_t size;
    uint32_t version;
    void *context;
    PulseStatus(PULSE_CALL *open)(void *, PulseStringView path, PulseDirectoryCompletionFn, void *, PulseDirectoryRequestId *);
    PulseStatus(PULSE_CALL *refresh)(void *, PulseDirectoryId, PulseDirectoryCompletionFn, void *, PulseDirectoryRequestId *);
    PulseStatus(PULSE_CALL *list)(void *, PulseDirectoryId, uint64_t snapshot_id, uint32_t offset, uint32_t limit,
        PulseDirectoryCompletionFn, void *, PulseDirectoryRequestId *);
    /* Resolves an existing snapshot entry, then delegates to Pulse's normal
       navigation/open routing. Success means dispatch accepted, not app launch
       or document execution completed. Stale snapshot/entry is rejected. */
    PulseStatus(PULSE_CALL *open_entry)(void *, PulseDirectoryId, uint64_t snapshot_id, uint64_t entry_id,
        PulseDirectoryCompletionFn, void *, PulseDirectoryRequestId *);
    PulseStatus(PULSE_CALL *watch)(void *, PulseDirectoryId, PulseDirectoryChangeFn, void *, PulseDirectoryWatchId *);
    PulseStatus(PULSE_CALL *unwatch)(void *, PulseDirectoryWatchId);
    PulseStatus(PULSE_CALL *close)(void *, PulseDirectoryId);
    PulseStatus(PULSE_CALL *cancel)(void *, PulseDirectoryRequestId);
} PulseDirectoriesV1;
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
#endif

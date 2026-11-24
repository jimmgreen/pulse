#ifndef PULSE_NATIVE_VFS_H
#define PULSE_NATIVE_VFS_H
#include "abi.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#define PULSE_VFS_INTERFACE_NAME "pulse.vfs"
#define PULSE_VFS_VERSION_1 1u
#pragma pack(push, 8)
typedef uint64_t PulseVfsMountHandle;

enum {
    PULSE_VFS_CAP_LIST_DIR = 1u << 0,
    PULSE_VFS_CAP_EXTRACT_FILE = 1u << 1,
    PULSE_VFS_CAP_STAT = 1u << 2,
    PULSE_VFS_CAP_READONLY = 1u << 3,
    PULSE_VFS_CAP_EXTRACT_ITEMS = 1u << 4, /* extract_items is set (desc->size >= PULSE_VFS_MOUNT_DESC_V2_SIZE) */
    PULSE_VFS_CAP_MODIFY = 1u << 5,        /* modify is set; the mount can change its own contents */
    PULSE_VFS_ALL_CAPS = (1u << 6) - 1u
};

enum {
    PULSE_VFS_ENTRY_DIRECTORY = 1u << 0,
    PULSE_VFS_ENTRY_READONLY = 1u << 1,
    PULSE_VFS_ENTRY_HIDDEN = 1u << 2,
    PULSE_VFS_ENTRY_ALL_FLAGS = (1u << 3) - 1u
};

enum {
    PULSE_VFS_OPEN_CURRENT_TAB = 0u,
    PULSE_VFS_OPEN_NEW_TAB = 1u
};

/* Borrowed for the duration of emit_entry; copy before returning. */
typedef struct PulseVfsEntry {
    uint32_t size; /* sizeof(PulseVfsEntry) */
    uint32_t flags; /* PULSE_VFS_ENTRY_* */
    uint64_t size_bytes;
    int64_t modified_unix_ms; /* <= 0 when unknown */
    PulseStringView name; /* Single entry name, 1..255 UTF-8 bytes; no / \\ : * ? " < > | */
    PulseStringView type_text; /* Optional UTF-8 0..128 bytes for localized kind/type */
} PulseVfsEntry;

typedef struct PulseVfsCancelToken {
    void* context;
    uint32_t(PULSE_CALL *is_cancelled)(void* context);
} PulseVfsCancelToken;

/* Emit one directory entry (or stat result). Return 0 to stop early on cancellation. */
typedef uint32_t(PULSE_CALL *PulseVfsEmitEntryFn)(void* emit_context, const PulseVfsEntry* entry);

/* Invoked off the UI thread on a host worker. Never touch UI-thread-only tables here.
   rel_path is normalized forward-slash UTF-8 relative to mount root ("" at root). */
typedef PulseStatus(PULSE_CALL *PulseVfsListDirFn)(
    void* user,
    PulseStringView mount_id,
    PulseStringView rel_path,
    PulseVfsEmitEntryFn emit_entry,
    void* emit_context,
    const PulseVfsCancelToken* cancel);

/* Materializes one virtual file into host-owned temp path dest_path_utf8 (off UI thread). */
typedef PulseStatus(PULSE_CALL *PulseVfsExtractFileFn)(
    void* user,
    PulseStringView mount_id,
    PulseStringView rel_path,
    PulseStringView dest_path_utf8,
    const PulseVfsCancelToken* cancel);

/* Optional single-entry stat off UI thread. Emit at most one entry on success. */
typedef PulseStatus(PULSE_CALL *PulseVfsStatFn)(
    void* user,
    PulseStringView mount_id,
    PulseStringView rel_path,
    PulseVfsEmitEntryFn emit_entry,
    void* emit_context,
    const PulseVfsCancelToken* cancel);

/* Progress of a long provider operation; borrowed for the duration of the call. */
typedef struct PulseVfsProgress {
    uint32_t size; /* sizeof(PulseVfsProgress) */
    uint32_t reserved;
    uint64_t completed_bytes;
    uint64_t total_bytes; /* 0 when unknown */
    uint64_t completed_items;
    uint64_t total_items; /* 0 when unknown */
    PulseStringView current; /* Optional UTF-8 item being processed */
} PulseVfsProgress;
/* Host services for extract_items/modify, valid for the duration of the call. Any function may be null. */
typedef struct PulseVfsOperation {
    uint32_t size; /* sizeof(PulseVfsOperation) */
    void* context;
    const PulseVfsCancelToken* cancel;
    void(PULSE_CALL *progress)(void* context, const PulseVfsProgress* progress);
    /* One short user-facing UTF-8 reason when the call fails (shown verbatim). */
    void(PULSE_CALL *message)(void* context, PulseStringView text);
} PulseVfsOperation;
/* Writes rel_paths (files or folders; all children of one folder) into the existing empty
   directory dest_dir_utf8, each under its own name, off the UI thread. */
typedef PulseStatus(PULSE_CALL *PulseVfsExtractItemsFn)(
    void* user,
    PulseStringView mount_id,
    const PulseStringView* rel_paths,
    uint32_t count,
    PulseStringView dest_dir_utf8,
    const PulseVfsOperation* op);
enum {
    PULSE_VFS_CHANGE_PUT = 0u,     /* rel_path := source_path (file or folder tree), replacing what is there */
    PULSE_VFS_CHANGE_DELETE = 1u,  /* remove rel_path (with everything below it) */
    PULSE_VFS_CHANGE_RENAME = 2u,  /* rel_path -> new_rel_path */
    PULSE_VFS_CHANGE_RESTORE = 3u  /* undo: bring back the state before the last successful modify */
};
typedef struct PulseVfsChange {
    uint32_t size; /* sizeof(PulseVfsChange) */
    uint32_t kind; /* PULSE_VFS_CHANGE_* */
    PulseStringView rel_path;
    PulseStringView source_path_utf8; /* PUT: host file system path */
    PulseStringView new_rel_path;     /* RENAME */
} PulseVfsChange;
/* Applies all changes as one transaction off the UI thread: either all land or nothing changes.
   The host refreshes its listings after PULSE_OK. RESTORE must be the only change of its call. */
typedef PulseStatus(PULSE_CALL *PulseVfsModifyFn)(
    void* user,
    PulseStringView mount_id,
    const PulseVfsChange* changes,
    uint32_t count,
    const PulseVfsOperation* op);
typedef struct PulseVfsMountDesc {
    uint32_t size; /* sizeof(PulseVfsMountDesc) */
    uint32_t capabilities; /* Must include PULSE_VFS_CAP_LIST_DIR */
    PulseStringView mount_id; /* ASCII [A-Za-z0-9._-], 1..64 bytes */
    PulseStringView display_name; /* UTF-8, 1..128 bytes */
    PulseStringView icon_glyph; /* Optional UTF-8 0..16 bytes */
    PulseVfsListDirFn list_dir;
    PulseVfsExtractFileFn extract_file; /* Required when PULSE_VFS_CAP_EXTRACT_FILE is set */
    PulseVfsStatFn stat; /* Required when PULSE_VFS_CAP_STAT is set */
    void* user;
    /* ---- appended; read only when size >= PULSE_VFS_MOUNT_DESC_V2_SIZE ---- */
    PulseVfsExtractItemsFn extract_items; /* Required when PULSE_VFS_CAP_EXTRACT_ITEMS is set */
    PulseVfsModifyFn modify;              /* Required when PULSE_VFS_CAP_MODIFY is set */
} PulseVfsMountDesc;
#define PULSE_VFS_MOUNT_DESC_V1_SIZE ((uint32_t)offsetof(PulseVfsMountDesc, extract_items))
#define PULSE_VFS_MOUNT_DESC_V2_SIZE ((uint32_t)sizeof(PulseVfsMountDesc))

/* Virtual File System provider registration & navigation bridge.
   register_mount, unregister_mount, invalidate_path, and open_path run on the
   owner UI thread. Provider list_dir/extract_file/stat callbacks run on host
   workers and must check cancel->is_cancelled(cancel->context) cooperatively.
   Closing a session unregisters all owned mounts and waits via CallbackActive()
   until in-flight provider callbacks drain before unloading the plugin DLL.
   Virtual URIs follow pulse:vfs:<package_id>/<mount_id>[/<rel_path>]. */
typedef struct PulseVfsV1 {
    uint32_t size, version;
    void* context;
    PulseStatus(PULSE_CALL *register_mount)(
        void* context, const PulseVfsMountDesc* desc, PulseVfsMountHandle* out_handle);
    PulseStatus(PULSE_CALL *unregister_mount)(void* context, PulseVfsMountHandle handle);
    PulseStatus(PULSE_CALL *invalidate_path)(
        void* context, PulseVfsMountHandle handle, PulseStringView rel_path);
    PulseStatus(PULSE_CALL *open_path)(
        void* context, PulseVfsMountHandle handle, PulseStringView rel_path, uint32_t mode);
} PulseVfsV1;
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
#endif
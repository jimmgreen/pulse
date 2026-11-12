#ifndef PULSE_NATIVE_PICKERS_H
#define PULSE_NATIVE_PICKERS_H
#include "abi.h"
#ifdef __cplusplus
extern "C" {
#endif
#define PULSE_PICKERS_INTERFACE_NAME "pulse.pickers"
#define PULSE_PICKERS_VERSION_1 1u
#pragma pack(push, 8)
typedef uint64_t PulsePickerRequestId;

enum {
    PULSE_PICKER_ACCEPTED = 0u,
    PULSE_PICKER_CANCELLED = 1u,
    PULSE_PICKER_FAILED = 2u
};

enum {
    PULSE_PICKER_FLAG_NONE = 0u,
    PULSE_PICKER_FLAG_MULTISELECT = 1u,
    PULSE_PICKER_FLAG_SHOW_HIDDEN = 2u,
    PULSE_PICKER_FLAG_NO_OVERWRITE_PROMPT = 4u,
    PULSE_PICKER_ALL_FLAGS = 7u
};

enum {
    PULSE_QUICK_PICK_ITEM_NONE = 0u,
    PULSE_QUICK_PICK_ITEM_SELECTED = 1u,
    PULSE_QUICK_PICK_ITEM_DISABLED = 2u,
    PULSE_QUICK_PICK_ITEM_ALL_FLAGS = 3u
};

typedef struct PulsePickerFilter {
    uint32_t size;
    uint32_t reserved;
    PulseStringView label;   /* UTF-8, 1..128 bytes ("Archives"). */
    PulseStringView pattern; /* UTF-8, 1..256 bytes ("*.zip;*.7z" or "*.*"). */
} PulsePickerFilter;

typedef struct PulseOpenFilesRequest {
    uint32_t size;
    uint32_t flags;
    uint32_t default_filter_index;
    uint32_t filter_count; /* 0..32 */
    PulseStringView title;        /* UTF-8, 0..128 bytes; empty uses host default. */
    PulseStringView ok_label;     /* UTF-8, 0..64 bytes; empty uses host default. */
    PulseStringView initial_path; /* UTF-8, 0..32768 bytes; optional directory. */
    const PulsePickerFilter* filters;
} PulseOpenFilesRequest;

typedef struct PulseSaveFileRequest {
    uint32_t size;
    uint32_t flags;
    uint32_t default_filter_index;
    uint32_t filter_count; /* 0..32 */
    PulseStringView title;             /* UTF-8, 0..128 bytes. */
    PulseStringView ok_label;          /* UTF-8, 0..64 bytes. */
    PulseStringView initial_path;      /* UTF-8, 0..32768 bytes; optional directory. */
    PulseStringView default_name;      /* UTF-8, 0..255 bytes; optional file name. */
    PulseStringView default_extension; /* UTF-8, 0..32 bytes without leading dot ("sha256"). */
    const PulsePickerFilter* filters;
} PulseSaveFileRequest;

typedef struct PulsePickFoldersRequest {
    uint32_t size;
    uint32_t flags;
    PulseStringView title;        /* UTF-8, 0..128 bytes. */
    PulseStringView ok_label;     /* UTF-8, 0..64 bytes. */
    PulseStringView initial_path; /* UTF-8, 0..32768 bytes; optional directory. */
} PulsePickFoldersRequest;

typedef struct PulseQuickPickItem {
    uint32_t size;
    uint32_t flags; /* PULSE_QUICK_PICK_ITEM_* */
    PulseStringView id;       /* ASCII [A-Za-z0-9._:/-], 1..128 bytes, unique per request. */
    PulseStringView title;    /* UTF-8, 1..256 bytes. */
    PulseStringView detail;   /* UTF-8, 0..256 bytes; optional secondary description. */
    PulseStringView glyph;    /* UTF-8, 0..128 bytes; optional glyph or "icon:..." token. */
    PulseStringView shortcut; /* UTF-8, 0..64 bytes; optional keycap hint. */
} PulseQuickPickItem;

typedef struct PulseQuickPickRequest {
    uint32_t size;
    uint32_t flags;      /* Reserved, must be 0. */
    uint32_t item_count; /* 1..512 */
    uint32_t reserved;
    PulseStringView title;         /* UTF-8, 0..128 bytes. */
    PulseStringView placeholder;   /* UTF-8, 0..128 bytes; search box hint. */
    PulseStringView initial_query; /* UTF-8, 0..128 bytes; optional prefilled filter. */
    const PulseQuickPickItem* items;
} PulseQuickPickRequest;

typedef struct PulsePickerResult {
    uint32_t size;
    uint32_t outcome; /* PULSE_PICKER_ACCEPTED / CANCELLED / FAILED */
    PulsePickerRequestId request_id;
    uint32_t filter_index;
    uint32_t path_count;
    const PulseStringView* paths; /* UTF-8 paths when outcome == ACCEPTED for file/folder pickers. */
    PulseStringView selected_id;  /* Chosen item id when outcome == ACCEPTED for quick_pick. */
    PulseStringView error;        /* UTF-8 diagnostic when outcome == FAILED. */
} PulsePickerResult;

typedef void(PULSE_CALL *PulsePickerResultFn)(void* user, const PulsePickerResult* result);

/* UI-thread API. Requests validate and copy all borrowed inputs synchronously,
   return immediately, and present the modal picker on a subsequent UI turn
   parented to the session's active modal window (an open pulse.dialogs window if
   present, otherwise the owner window) guarded by PickerOwnerGuard. At most one
   picker or quick_pick request may be active per session (busy returns STATE).
   cancel(request_id) dismisses an active request as PULSE_PICKER_CANCELLED.
   PulsePickerResultFn runs at most once on the UI thread with borrowed views,
   unless the session stops first: stopping cancels any open picker and suppresses
   the callback. Keep userdata alive until callback returns or session destroy. */
typedef struct PulsePickersV1 {
    uint32_t size, version;
    void* context;
    PulseStatus(PULSE_CALL *open_files)(void* context, const PulseOpenFilesRequest* request,
        PulsePickerResultFn callback, void* user, PulsePickerRequestId* request_id);
    PulseStatus(PULSE_CALL *save_file)(void* context, const PulseSaveFileRequest* request,
        PulsePickerResultFn callback, void* user, PulsePickerRequestId* request_id);
    PulseStatus(PULSE_CALL *pick_folders)(void* context, const PulsePickFoldersRequest* request,
        PulsePickerResultFn callback, void* user, PulsePickerRequestId* request_id);
    PulseStatus(PULSE_CALL *quick_pick)(void* context, const PulseQuickPickRequest* request,
        PulsePickerResultFn callback, void* user, PulsePickerRequestId* request_id);
    PulseStatus(PULSE_CALL *cancel)(void* context, PulsePickerRequestId request_id);
} PulsePickersV1;
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
#endif

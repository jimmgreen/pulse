#ifndef PULSE_NATIVE_COLUMNS_H
#define PULSE_NATIVE_COLUMNS_H
#include "abi.h"
#ifdef __cplusplus
extern "C" {
#endif
#define PULSE_COLUMNS_INTERFACE_NAME "pulse.columns"
#define PULSE_COLUMNS_VERSION_1 1u
#pragma pack(push, 8)
typedef uint64_t PulseColumnHandle;

enum {
    PULSE_COLUMN_APPLIES_FILES = 1u << 0,
    PULSE_COLUMN_APPLIES_FOLDERS = 1u << 1,
    PULSE_COLUMN_APPLIES_ALL = (1u << 2) - 1u
};

enum {
    PULSE_COLUMN_ALIGN_START = 0u,
    PULSE_COLUMN_ALIGN_END = 1u
};

enum {
    PULSE_COLUMN_TONE_DEFAULT = 0u,
    PULSE_COLUMN_TONE_MUTED = 1u,
    PULSE_COLUMN_TONE_ACCENT = 2u,
    PULSE_COLUMN_TONE_SUCCESS = 3u,
    PULSE_COLUMN_TONE_WARNING = 4u,
    PULSE_COLUMN_TONE_DANGER = 5u
};

enum {
    PULSE_COLUMN_ITEM_DIRECTORY = 1u << 0
};

/* One listed item, borrowed for the duration of the query call. */
typedef struct PulseColumnItem {
    uint32_t size; /* sizeof(PulseColumnItem) */
    uint32_t flags; /* PULSE_COLUMN_ITEM_* */
    PulseStringView path; /* Full UTF-8 file system path */
    uint64_t size_bytes; /* 0 for folders */
    int64_t modified_unix_ms; /* <= 0 when unknown */
} PulseColumnItem;

/* Text of one cell. */
typedef struct PulseColumnValue {
    uint32_t size; /* sizeof(PulseColumnValue) */
    uint32_t tone; /* PULSE_COLUMN_TONE_* */
    PulseStringView text; /* UTF-8, 0..256 bytes, no control characters; empty = blank cell */
} PulseColumnValue;

/* Badge drawn on one item's icon. */
typedef struct PulseBadgeValue {
    uint32_t size; /* sizeof(PulseBadgeValue) */
    uint32_t tone; /* PULSE_COLUMN_TONE_*; DEFAULT draws in the text color */
    PulseStringView icon; /* Built-in icon name, package icon name, "<package>/<name>" or "#<id>"
                             (optional "icon:" prefix), 0..96 bytes; empty = no badge */
} PulseBadgeValue;

/* One batch of items, valid for the duration of the query call. */
typedef struct PulseColumnBatch {
    uint32_t size; /* sizeof(PulseColumnBatch) */
    uint32_t count; /* 1..64 */
    const PulseColumnItem* items;
    void* context;
    /* Non-zero once the host no longer needs this batch (folder left, provider removed). */
    uint32_t(PULSE_CALL *is_cancelled)(void* context);
    /* Column providers: the text of items[index]. At most once per index. */
    PulseStatus(PULSE_CALL *set_text)(void* context, uint32_t index, const PulseColumnValue* value);
    /* Badge providers: the badge of items[index]. At most once per index. */
    PulseStatus(PULSE_CALL *set_badge)(void* context, uint32_t index, const PulseBadgeValue* value);
} PulseColumnBatch;

/* Runs on a host worker thread, never on the UI thread, one batch at a time per
   provider. Items left unset are shown blank; values are cached per path, size and
   modified time until the file changes or the plugin calls invalidate. Return
   PULSE_OK (also when only some items were set) or an error to leave the batch blank. */
typedef PulseStatus(PULSE_CALL *PulseColumnQueryFn)(void* user, const PulseColumnBatch* batch);

typedef struct PulseColumnDesc {
    uint32_t size; /* sizeof(PulseColumnDesc) */
    uint32_t applies_to; /* PULSE_COLUMN_APPLIES_*; 0 = files and folders */
    PulseStringView id; /* ASCII [A-Za-z0-9._-], 1..64 bytes, unique within the package */
    PulseStringView title; /* UTF-8 header text, 1..64 bytes */
    PulseStringView extensions; /* Optional ".jpg;.png", 0..512 bytes: only these files are asked */
    uint32_t width_dip; /* 0 = 120; else 48..480 */
    uint32_t align; /* PULSE_COLUMN_ALIGN_* */
    PulseColumnQueryFn query;
    void* user;
} PulseColumnDesc;

typedef struct PulseBadgeDesc {
    uint32_t size; /* sizeof(PulseBadgeDesc) */
    uint32_t applies_to; /* PULSE_COLUMN_APPLIES_*; 0 = files and folders */
    PulseStringView id; /* ASCII [A-Za-z0-9._-], 1..64 bytes, unique within the package */
    PulseStringView title; /* UTF-8, 1..64 bytes: names the badge in the column menu */
    PulseStringView extensions; /* Optional ".jpg;.png", 0..512 bytes */
    PulseColumnQueryFn query;
    void* user;
} PulseBadgeDesc;

/* Extra Details columns and icon badges for local folders.
   All functions run on the owner UI thread. Up to 4 columns and 4 badge providers per
   package; the user can hide each one from the column header menu. Cells and badges are
   asked for lazily, for the rows on screen, in batches of up to 64 items. Columns are
   display-only (no sorting). invalidate with an empty path drops every cached value of
   the provider; with a path, that item's value. unregister returns once a query call
   already running for the provider has returned (it reads is_cancelled != 0 meanwhile),
   so its user data may be freed right after; a query must therefore never wait for the
   UI thread. Closing the session unregisters everything and waits for running query
   calls before the DLL unloads. */
typedef struct PulseColumnsV1 {
    uint32_t size, version;
    void* context;
    PulseStatus(PULSE_CALL *register_column)(
        void* context, const PulseColumnDesc* desc, PulseColumnHandle* out_handle);
    PulseStatus(PULSE_CALL *register_badge)(
        void* context, const PulseBadgeDesc* desc, PulseColumnHandle* out_handle);
    PulseStatus(PULSE_CALL *set_title)(void* context, PulseColumnHandle handle, PulseStringView title);
    PulseStatus(PULSE_CALL *invalidate)(void* context, PulseColumnHandle handle, PulseStringView path);
    PulseStatus(PULSE_CALL *unregister)(void* context, PulseColumnHandle handle);
} PulseColumnsV1;
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
#endif

#ifndef PULSE_NATIVE_WORKBENCH_H
#define PULSE_NATIVE_WORKBENCH_H
#include "abi.h"
#ifdef __cplusplus
extern "C" {
#endif
#define PULSE_WORKBENCH_INTERFACE_NAME "pulse.workbench"
#define PULSE_WORKBENCH_VERSION_1 1u
#pragma pack(push, 8)
typedef uint64_t PulseNotificationId;

enum {
    PULSE_NOTICE_INFO = 0u,
    PULSE_NOTICE_SUCCESS = 1u,
    PULSE_NOTICE_WARNING = 2u,
    PULSE_NOTICE_ERROR = 3u
};

enum {
    PULSE_SNACK_FLAG_NONE = 0u,
    PULSE_SNACK_FLAG_REPLACE = 1u,
    PULSE_SNACK_ALL_FLAGS = 1u
};

enum {
    PULSE_NAVIGATE_CURRENT_TAB = 0u,
    PULSE_NAVIGATE_NEW_TAB = 1u,
    PULSE_NAVIGATE_REVEAL_ITEM = 2u
};

typedef struct PulseWorkbenchAction {
    uint32_t size;
    uint32_t reserved;
    PulseStringView id;    /* ASCII [A-Za-z0-9._-], 1..64 bytes. */
    PulseStringView label; /* UTF-8, 1..64 bytes. */
} PulseWorkbenchAction;

typedef struct PulseSnackRequest {
    uint32_t size;
    uint32_t flags;       /* PULSE_SNACK_FLAG_* */
    uint32_t kind;        /* PULSE_NOTICE_* */
    uint32_t duration_ms; /* 0 = host default (5000 ms), clamped 1500..15000. */
    PulseStringView text;   /* UTF-8, 1..256 bytes. */
    PulseStringView detail; /* UTF-8, 0..256 bytes. */
    const PulseWorkbenchAction* actions;
    uint32_t action_count; /* 0..2 */
    uint32_t reserved;
} PulseSnackRequest;

typedef struct PulseNoticeRequest {
    uint32_t size;
    uint32_t kind;          /* PULSE_NOTICE_* */
    PulseStringView title;   /* UTF-8, 1..128 bytes. */
    PulseStringView message; /* UTF-8, 1..1024 bytes. */
    const PulseWorkbenchAction* actions;
    uint32_t action_count; /* 0..2 */
    uint32_t reserved;
} PulseNoticeRequest;

typedef void(PULSE_CALL *PulseWorkbenchActionFn)(void* user, PulseNotificationId id, PulseStringView action_id);

/* UI-thread host workbench integration: capsule snacks, persistent corner action
   cards, path navigation / item selection reveal, built-in side-by-side text diff
   viewer, and clipboard text write. Borrowed inputs are copied synchronously.
   Optional notification action callbacks run at most once on the UI thread when
   the user clicks an action button while the session remains active; stopping
   the session invalidates pending action tokens. Keep action userdata valid
   until plugin destroy if actions are supplied. */
typedef struct PulseWorkbenchV1 {
    uint32_t size, version;
    void* context;
    PulseStatus(PULSE_CALL *show_snack)(void* context, const PulseSnackRequest* request,
        PulseWorkbenchActionFn on_action, void* user, PulseNotificationId* out_id);
    PulseStatus(PULSE_CALL *show_notice)(void* context, const PulseNoticeRequest* request,
        PulseWorkbenchActionFn on_action, void* user, PulseNotificationId* out_id);
    PulseStatus(PULSE_CALL *reveal_path)(void* context, PulseStringView path, uint32_t mode);
    PulseStatus(PULSE_CALL *show_text_diff)(void* context, PulseStringView left_path, PulseStringView right_path);
    PulseStatus(PULSE_CALL *set_clipboard_text)(void* context, PulseStringView text);
} PulseWorkbenchV1;
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
#endif

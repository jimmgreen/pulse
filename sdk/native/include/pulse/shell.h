#ifndef PULSE_NATIVE_SHELL_H
#define PULSE_NATIVE_SHELL_H
#include "abi.h"
#ifdef __cplusplus
extern "C" {
#endif
#define PULSE_SHELL_INTERFACE_NAME "pulse.shell"
#define PULSE_SHELL_VERSION_1 1u
#pragma pack(push, 8)
typedef uint64_t PulseShellMenuSessionId;
typedef uint64_t PulseShellPreviewRequestId;

enum {
    PULSE_SHELL_MENU_ITEM_ENABLED = 1u,
    PULSE_SHELL_MENU_ITEM_SEPARATOR_AFTER = 2u,
    PULSE_SHELL_MENU_ITEM_SUBMENU = 4u,
    PULSE_SHELL_MENU_ITEM_CHILD = 8u
};

enum {
    PULSE_SHELL_MENU_QUERY_BACKGROUND = 1u,
    PULSE_SHELL_MENU_QUERY_EXTENDED = 2u,
    PULSE_SHELL_MENU_QUERY_ALL_FLAGS = 3u
};

typedef struct PulseColorTagInfo {
    PulseStringView id;
    PulseStringView name;
    uint32_t argb;
    int32_t favorite_slot;
} PulseColorTagInfo;

typedef void(PULSE_CALL *PulseColorTagListFn)(void* user, const PulseColorTagInfo* tags, uint32_t count);

typedef struct PulseShellMenuItem {
    uint32_t item_id;
    uint32_t flags;
    uint32_t depth;
    uint32_t mnemonic_codepoint;
    PulseStringView verb;
    PulseStringView label;
    PulseStringView handler_name;
} PulseShellMenuItem;

typedef struct PulseShellMenuRequest {
    uint32_t size;
    uint32_t flags; /* PULSE_SHELL_MENU_QUERY_* */
    const PulseStringView* paths;
    uint32_t path_count; /* 1..64 */
    uint32_t reserved;
} PulseShellMenuRequest;

typedef struct PulseShellMenuReply {
    uint32_t size;
    PulseStatus status;
    PulseShellMenuSessionId session_id;
    const PulseShellMenuItem* items;
    uint32_t item_count;
    uint32_t partial;
} PulseShellMenuReply;

typedef void(PULSE_CALL *PulseShellMenuReplyFn)(void* user, const PulseShellMenuReply* reply);

typedef struct PulseShellPreviewSummary {
    uint32_t size;
    PulseStatus status;
    PulseShellPreviewRequestId request_id;
    PulseStringView path;
    PulseStringView display_type;
    PulseStringView text_excerpt;
    uint64_t size_bytes;
    int64_t modified_unix_ms;
    uint32_t image_width;
    uint32_t image_height;
    uint32_t thumb_width;
    uint32_t thumb_height;
    uint32_t thumb_stride;
    uint32_t thumb_byte_count;
    const uint8_t* thumb_bgra;
} PulseShellPreviewSummary;

typedef void(PULSE_CALL *PulseShellPreviewFn)(void* user, const PulseShellPreviewSummary* summary);

/* UI-thread bridge for Pulse color tags, out-of-process isolated Shell context menus
   (delegated to pulse_shell_host.exe), and asynchronous non-blocking Quick Look
   thumbnails/text excerpts. Borrowed slices expire when callbacks return. */
typedef struct PulseShellV1 {
    uint32_t size, version;
    void* context;
    PulseStatus(PULSE_CALL *get_tag_catalog)(void* context, PulseColorTagListFn callback, void* user);
    PulseStatus(PULSE_CALL *get_path_tags)(void* context, PulseStringView path, PulseColorTagListFn callback, void* user);
    PulseStatus(PULSE_CALL *set_path_tag)(void* context, PulseStringView path, PulseStringView tag_id, uint32_t tagged);
    PulseStatus(PULSE_CALL *query_context_menu)(void* context, const PulseShellMenuRequest* request,
        PulseShellMenuReplyFn callback, void* user, PulseShellMenuSessionId* out_session_id);
    PulseStatus(PULSE_CALL *invoke_context_menu)(void* context, PulseShellMenuSessionId session_id,
        uint32_t item_id, PulseStringView verb);
    PulseStatus(PULSE_CALL *close_context_menu)(void* context, PulseShellMenuSessionId session_id);
    PulseStatus(PULSE_CALL *read_preview)(void* context, PulseStringView path, uint32_t max_thumb_px,
        PulseShellPreviewFn callback, void* user, PulseShellPreviewRequestId* out_request_id);
    PulseStatus(PULSE_CALL *cancel_preview)(void* context, PulseShellPreviewRequestId request_id);
} PulseShellV1;
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
#endif

#ifndef PULSE_NATIVE_UI_MOUNT_H
#define PULSE_NATIVE_UI_MOUNT_H
#include "abi.h"
#ifdef __cplusplus
extern "C" {
#endif
#define PULSE_UI_MOUNT_INTERFACE "pulse.ui.mount"
#define PULSE_UI_MOUNT_INTERFACE_NAME PULSE_UI_MOUNT_INTERFACE
#define PULSE_UI_MOUNT_VERSION_1 1u
/* UI-thread attachment API. A mount moves the existing panel and preserves its
   controls, state and subscriptions. SIDEBAR is the host right sidebar;
   WORKSPACE is the host content area, not a tab or desktop. Host-owned slots
   have one occupant; a busy slot returns STATE without replacing its occupant.
   A narrow host may fall back to WINDOW; get/events report actual placement.
   subscribe schedules an initial snapshot for a later host turn. Changes to
   placement, client dimensions, DPI, theme and focus are coalesced per turn.
   At most 16 subscriptions exist per attachment. unsubscribe/close/stop cancel
   queued delivery. Callbacks may unsubscribe, move or close their panel and
   never nest. Info is borrowed until callback return. size must equal sizeof
   PulseUiMountInfo. No exception may cross this ABI. */
#pragma pack(push, 8)
enum { PULSE_UI_MOUNT_WINDOW = 0u, PULSE_UI_MOUNT_SIDEBAR = 1u, PULSE_UI_MOUNT_WORKSPACE = 2u };
typedef struct PulseUiMountInfo {
    uint32_t size;
    uint32_t placement;
    uint32_t width_dip;
    uint32_t height_dip;
    uint32_t dpi;
    uint32_t dark;
    uint32_t focused;
    uint32_t reserved;
    uint64_t revision;
} PulseUiMountInfo;
typedef void(PULSE_CALL *PulseUiMountChangedFn)(void *user, uint64_t panel, const PulseUiMountInfo *info);
typedef struct PulseUiMountV1 {
    uint32_t size;
    uint32_t version;
    void *context;
    PulseStatus(PULSE_CALL *set)(void *context, uint64_t panel, uint32_t placement);
    PulseStatus(PULSE_CALL *get)(void *context, uint64_t panel, PulseUiMountInfo *info);
    PulseStatus(PULSE_CALL *subscribe)(void *context, uint64_t panel, PulseUiMountChangedFn callback,
                                       void *user, uint64_t *token);
    PulseStatus(PULSE_CALL *unsubscribe)(void *context, uint64_t token);
} PulseUiMountV1;
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
#endif

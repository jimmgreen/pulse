#ifndef PULSE_NATIVE_STATUSBAR_H
#define PULSE_NATIVE_STATUSBAR_H
#include "abi.h"
#ifdef __cplusplus
extern "C" {
#endif
#define PULSE_STATUSBAR_INTERFACE_NAME "pulse.statusbar"
#define PULSE_STATUSBAR_VERSION_1 1u
#pragma pack(push, 8)
typedef uint64_t PulseStatusItemHandle;

enum {
    PULSE_STATUS_TONE_DEFAULT = 0u,
    PULSE_STATUS_TONE_MUTED = 1u,
    PULSE_STATUS_TONE_ACCENT = 2u,
    PULSE_STATUS_TONE_SUCCESS = 3u,
    PULSE_STATUS_TONE_WARNING = 4u,
    PULSE_STATUS_TONE_DANGER = 5u
};

/* Runs on the owner UI thread when the user clicks the item. Keep it short: open a page,
   run a command, start a task. Never called after remove_item returned or the session closed. */
typedef void(PULSE_CALL *PulseStatusItemClickFn)(void* user, PulseStatusItemHandle handle);

typedef struct PulseStatusItemDesc {
    uint32_t size; /* sizeof(PulseStatusItemDesc) */
    uint32_t tone; /* PULSE_STATUS_TONE_*; DEFAULT draws in the status bar's secondary text color */
    PulseStringView id; /* ASCII [A-Za-z0-9._-], 1..64 bytes, unique within the package;
                           update_item: empty or the registered id */
    PulseStringView text; /* UTF-8, 0..64 bytes, no control characters */
    PulseStringView icon; /* Built-in icon name, package icon name, "<package>/<name>" or "#<id>"
                             (optional "icon:" prefix), 0..96 bytes; text or icon is required */
    PulseStringView tooltip; /* UTF-8, 0..256 bytes, no control characters; shown on hover */
    PulseStatusItemClickFn on_click; /* Optional; without it the item is a plain label */
    void* user;
} PulseStatusItemDesc;

/* Small items in the main window's status bar, after the folder text and the Git chip.
   All functions run on the owner UI thread. Up to 2 items per package, shown in
   registration order; a narrow window drops the ones that no longer fit. update_item
   replaces text, icon, tooltip, tone and the click callback in one call (repaints once).
   remove_item returns after a click callback already running for the item has returned,
   so its user data may be freed right after. Closing the session removes every item. */
typedef struct PulseStatusBarV1 {
    uint32_t size, version;
    void* context;
    PulseStatus(PULSE_CALL *add_item)(
        void* context, const PulseStatusItemDesc* desc, PulseStatusItemHandle* out_handle);
    PulseStatus(PULSE_CALL *update_item)(
        void* context, PulseStatusItemHandle handle, const PulseStatusItemDesc* desc);
    PulseStatus(PULSE_CALL *set_visible)(void* context, PulseStatusItemHandle handle, uint32_t visible);
    PulseStatus(PULSE_CALL *remove_item)(void* context, PulseStatusItemHandle handle);
} PulseStatusBarV1;
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
#endif

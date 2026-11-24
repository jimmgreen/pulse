#ifndef PULSE_NATIVE_WORKBENCH_H
#define PULSE_NATIVE_WORKBENCH_H
#include "abi.h"
#ifdef __cplusplus
extern "C" {
#endif
#define PULSE_WORKBENCH_INTERFACE_NAME "pulse.workbench"
#define PULSE_WORKBENCH_VERSION_1 1u
#define PULSE_WORKBENCH_VERSION_2 2u
#define PULSE_WORKBENCH_VERSION_3 3u
#pragma pack(push, 8)
typedef uint64_t PulseNotificationId;
typedef uint64_t PulseAppearanceSubscription;
typedef uint64_t PulseLocaleSubscription;

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
    PULSE_NAVIGATE_REVEAL_ITEM = 2u,
    /* V2 reveal_path_from_rect only: preview without navigating a file tab.
       Hover obeys host preview prefs and dwell; dismiss matches the last path.
       Older hosts return ARGUMENT/UNAVAILABLE. No interface layout change. */
    PULSE_NAVIGATE_PREVIEW = 3u,
    PULSE_NAVIGATE_HOVER_PREVIEW = 4u,
    PULSE_NAVIGATE_DISMISS_HOVER_PREVIEW = 5u
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

enum {
    PULSE_APPEARANCE_LIGHT = 0u,
    PULSE_APPEARANCE_DARK = 1u
};

enum {
    PULSE_BACKDROP_NONE = 0u,
    PULSE_BACKDROP_ACRYLIC = 1u,
    PULSE_BACKDROP_MICA = 2u,
    PULSE_BACKDROP_MICA_ALT = 3u
};

typedef struct PulseScreenRect {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
} PulseScreenRect;

typedef struct PulseHostAppearance {
    uint32_t size;
    uint32_t flags;            /* bit 0 = PULSE_APPEARANCE_DARK */
    uint32_t window_effect;    /* PULSE_BACKDROP_* */
    uint32_t opacity_percent;  /* 30..100 */
    uint32_t accent_argb;      /* 0xAARRGGBB */
    uint32_t font_scale_milli; /* 1000 = 100% */
} PulseHostAppearance;

typedef void(PULSE_CALL *PulseWorkbenchAppearanceFn)(void* user, const PulseHostAppearance* appearance);

/* Append-only V2 prefix extension of PulseWorkbenchV1: adds search-tab opening,
   path reveal with optional screen-space origin rectangle (bringing hidden-to-tray
   host windows forward cleanly), and host theme/backdrop/accent observation. */
typedef struct PulseWorkbenchV2 {
    uint32_t size, version;
    void* context;
    PulseStatus(PULSE_CALL *show_snack)(void* context, const PulseSnackRequest* request,
        PulseWorkbenchActionFn on_action, void* user, PulseNotificationId* out_id);
    PulseStatus(PULSE_CALL *show_notice)(void* context, const PulseNoticeRequest* request,
        PulseWorkbenchActionFn on_action, void* user, PulseNotificationId* out_id);
    PulseStatus(PULSE_CALL *reveal_path)(void* context, PulseStringView path, uint32_t mode);
    PulseStatus(PULSE_CALL *show_text_diff)(void* context, PulseStringView left_path, PulseStringView right_path);
    PulseStatus(PULSE_CALL *set_clipboard_text)(void* context, PulseStringView text);
    PulseStatus(PULSE_CALL *open_search)(void* context, PulseStringView query);
    PulseStatus(PULSE_CALL *reveal_path_from_rect)(void* context, PulseStringView path, uint32_t mode,
        const PulseScreenRect* origin_rect);
    PulseStatus(PULSE_CALL *query_appearance)(void* context, PulseHostAppearance* out_appearance);
    PulseStatus(PULSE_CALL *subscribe_appearance)(void* context, PulseWorkbenchAppearanceFn callback,
        void* user, PulseAppearanceSubscription* out_subscription);
    PulseStatus(PULSE_CALL *unsubscribe_appearance)(void* context, PulseAppearanceSubscription subscription);
} PulseWorkbenchV2;

enum {
    PULSE_LOCALE_FOLLOW_SYSTEM = 1u
};

typedef struct PulseHostLocale {
    uint32_t size;           /* sizeof(PulseHostLocale) */
    uint32_t flags;          /* bit 0 = PULSE_LOCALE_FOLLOW_SYSTEM */
    char language_tag[32];   /* NUL-terminated BCP-47 tag, e.g. "zh-CN", "zh-TW", "en-US" */
    char preference_tag[32]; /* NUL-terminated host preference: "system" or explicit BCP-47 tag */
} PulseHostLocale;

typedef void(PULSE_CALL *PulseWorkbenchLocaleFn)(void* user, const PulseHostLocale* locale);

/* Append-only V3 prefix extension of PulseWorkbenchV2: adds host UI language
   query and change notification so plugins match Pulse's effective locale
   instead of OS display language. */
typedef struct PulseWorkbenchV3 {
    uint32_t size, version;
    void* context;
    PulseStatus(PULSE_CALL *show_snack)(void* context, const PulseSnackRequest* request,
        PulseWorkbenchActionFn on_action, void* user, PulseNotificationId* out_id);
    PulseStatus(PULSE_CALL *show_notice)(void* context, const PulseNoticeRequest* request,
        PulseWorkbenchActionFn on_action, void* user, PulseNotificationId* out_id);
    PulseStatus(PULSE_CALL *reveal_path)(void* context, PulseStringView path, uint32_t mode);
    PulseStatus(PULSE_CALL *show_text_diff)(void* context, PulseStringView left_path, PulseStringView right_path);
    PulseStatus(PULSE_CALL *set_clipboard_text)(void* context, PulseStringView text);
    PulseStatus(PULSE_CALL *open_search)(void* context, PulseStringView query);
    PulseStatus(PULSE_CALL *reveal_path_from_rect)(void* context, PulseStringView path, uint32_t mode,
        const PulseScreenRect* origin_rect);
    PulseStatus(PULSE_CALL *query_appearance)(void* context, PulseHostAppearance* out_appearance);
    PulseStatus(PULSE_CALL *subscribe_appearance)(void* context, PulseWorkbenchAppearanceFn callback,
        void* user, PulseAppearanceSubscription* out_subscription);
    PulseStatus(PULSE_CALL *unsubscribe_appearance)(void* context, PulseAppearanceSubscription subscription);
    PulseStatus(PULSE_CALL *query_locale)(void* context, PulseHostLocale* out_locale);
    PulseStatus(PULSE_CALL *subscribe_locale)(void* context, PulseWorkbenchLocaleFn callback,
        void* user, PulseLocaleSubscription* out_subscription);
    PulseStatus(PULSE_CALL *unsubscribe_locale)(void* context, PulseLocaleSubscription subscription);
} PulseWorkbenchV3;
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
#endif

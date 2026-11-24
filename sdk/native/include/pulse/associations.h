#ifndef PULSE_NATIVE_ASSOCIATIONS_H
#define PULSE_NATIVE_ASSOCIATIONS_H
#include "abi.h"
#ifdef __cplusplus
extern "C" {
#endif
#define PULSE_ASSOCIATIONS_INTERFACE_NAME "pulse.associations"
#define PULSE_ASSOCIATIONS_VERSION_1 1u
#pragma pack(push, 8)
enum {
    PULSE_ASSOC_OWNER_UNREGISTERED = 0u,
    PULSE_ASSOC_OWNER_PULSE = 1u,
    PULSE_ASSOC_OWNER_FREE = 2u,
    PULSE_ASSOC_OWNER_OTHER = 3u,
    PULSE_ASSOC_OWNER_UNKNOWN = 4u
};

enum {
    PULSE_ASSOC_ACTION_COMMAND = 0u,
    PULSE_ASSOC_ACTION_PREVIEW = 1u
};

enum {
    PULSE_ASSOC_STATUS_FLAG_NONE = 0u,
    PULSE_ASSOC_STATUS_FLAG_WINDOWS_DEFAULT = 1u
};

typedef uint64_t PulseAssociationRequestId;

typedef struct PulseAssociationEntry {
    uint32_t size;
    uint32_t action;              /* PULSE_ASSOC_ACTION_* */
    PulseStringView handler_id;   /* Must match a contributes.fileHandlers[].id in manifest.json. */
    PulseStringView extension;    /* Lowercase dotted extension declared by handler_id, e.g. ".7z". */
    PulseStringView display_name; /* UTF-8 localized Windows file type description (FriendlyTypeName). */
    PulseStringView verb_label;   /* UTF-8 shell open verb label, e.g. "Open in Pulse". */
    PulseStringView icon_resource;/* Flat package resource filename or "<file>,<index>"; empty = host icon. */
} PulseAssociationEntry;

typedef struct PulseAssociationStatus {
    uint32_t size;
    uint32_t owner;               /* PULSE_ASSOC_OWNER_* */
    uint32_t flags;               /* PULSE_ASSOC_STATUS_FLAG_* */
    uint32_t reserved;
    PulseStringView extension;
    PulseStringView handler_id;
    PulseStringView current_app;  /* Friendly app name / ProgID when owner is OTHER. */
} PulseAssociationStatus;

typedef void(PULSE_CALL *PulseAssociationsStatusFn)(
    void* user,
    PulseAssociationRequestId request,
    PulseStatus status,
    const PulseAssociationStatus* items,
    uint32_t item_count);

typedef void(PULSE_CALL *PulseAssociationsCompleteFn)(
    void* user,
    PulseAssociationRequestId request,
    PulseStatus status);

/* UI-thread entry points returning asynchronous results via session pump.
   Every extension operated on MUST be declared in manifest contributes.fileHandlers
   and cannot target blocked executable/script extensions. Windows UserChoice is never
   overwritten silently: unclaimed extensions claim HKCU defaults with reversible backup,
   and claimed extensions register OpenWithProgids + Default Apps Capabilities.
   `apply` reconciles the package's full active association set (unlisted previously
   recorded extensions are removed; count == 0 removes all). `remove` removes specific
   listed extensions (count == 0 removes all). On package disable or uninstall, the host
   automatically restores backed-up defaults. */
typedef struct PulseAssociationsV1 {
    uint32_t size;
    uint32_t version;
    void* context;
    PulseStatus(PULSE_CALL *query)(void* context, const PulseStringView* extensions, uint32_t count,
        PulseAssociationsStatusFn callback, void* user, PulseAssociationRequestId* out_request);
    PulseStatus(PULSE_CALL *apply)(void* context, const PulseAssociationEntry* entries, uint32_t count,
        PulseAssociationsCompleteFn callback, void* user, PulseAssociationRequestId* out_request);
    PulseStatus(PULSE_CALL *remove)(void* context, const PulseStringView* extensions, uint32_t count,
        PulseAssociationsCompleteFn callback, void* user, PulseAssociationRequestId* out_request);
    PulseStatus(PULSE_CALL *cancel)(void* context, PulseAssociationRequestId request);
    PulseStatus(PULSE_CALL *open_chooser)(void* context, PulseStringView extension);
    PulseStatus(PULSE_CALL *open_default_apps)(void* context);
} PulseAssociationsV1;
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
#endif

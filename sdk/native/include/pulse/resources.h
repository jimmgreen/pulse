#ifndef PULSE_NATIVE_RESOURCES_H
#define PULSE_NATIVE_RESOURCES_H
#include "abi.h"
#ifdef __cplusplus
extern "C" {
#endif
#define PULSE_RESOURCES_INTERFACE_NAME "pulse.resources"
#define PULSE_RESOURCES_VERSION_1 1u
/* QueryInterface copies this table into caller-owned memory. Calls and callbacks
   run on the owner UI thread; accepted calls complete after the call returns.
   Input strings are copied before return. All reply strings/arrays are borrowed
   only during the callback; copy with your own allocator, never free host data.
   No exception may cross the ABI. At most 64 requests may be pending.
   cancel_request and attachment Close suppress future callbacks and cancel
   pending work where possible; committed settings are not rolled back.
   Resource declarations files.readCurrentDirectory, settings.own and audit.own
   describe installation capabilities; they do not gate individual calls.
   Opaque entry handles come from pulse.files list; paths are never accepted.
   Settings are package-owned; audit entries are limited to this owner/principal.
   Table/context remains valid until attachment destruction. */
#pragma pack(push, 8)
typedef uint64_t PulseResourcesRequestId;
enum {
    PULSE_RESOURCES_STAT = 1,
    PULSE_RESOURCES_SETTINGS_GET = 2,
    PULSE_RESOURCES_SETTINGS_SET = 3,
    PULSE_RESOURCES_AUDIT_LIST = 4
};
enum { PULSE_RESOURCE_FILE_DIRECTORY = 1u, PULSE_RESOURCE_FILE_REPARSE_POINT = 2u };
typedef struct PulseResourceFileStat {
    PulseStringView name;
    uint64_t size_bytes;
    uint64_t modified_filetime;
    uint32_t attributes;
    uint32_t flags;
} PulseResourceFileStat;
typedef struct PulseSetting {
    PulseStringView key;
    PulseStringView value;
} PulseSetting;
typedef struct PulseSettings {
    uint64_t version;
    const PulseSetting *values;
    uint32_t count;
    uint32_t reserved;
} PulseSettings;
typedef struct PulseAuditEntry {
    uint64_t sequence;
    uint64_t monotonic_ms;
    PulseStringView operation;
    PulseStringView outcome;
} PulseAuditEntry;
typedef struct PulseAuditPage {
    const PulseAuditEntry *entries;
    uint32_t count;
    uint32_t reserved;
} PulseAuditPage;
typedef struct PulseResourcesReply {
    uint32_t size;
    uint32_t operation;
    PulseResourcesRequestId request_id;
    PulseStatus status;
    uint32_t reserved;
    PulseStringView error; /* Stable machine code, including version_conflict. */
    PulseResourceFileStat file_stat;
    PulseSettings settings;
    PulseAuditPage audit;
} PulseResourcesReply;
typedef void(PULSE_CALL *PulseResourcesCompletionFn)(void *user, const PulseResourcesReply *reply);
typedef struct PulseResourcesV1 {
    uint32_t size;
    uint32_t version;
    void *context;
    PulseStatus(PULSE_CALL *stat)(void *context, PulseStringView entry, PulseResourcesCompletionFn completion,
                                  void *user, PulseResourcesRequestId *request_id);
    PulseStatus(PULSE_CALL *settings_get)(void *context, PulseResourcesCompletionFn completion, void *user,
                                          PulseResourcesRequestId *request_id);
    /* Keys: 1..64 ASCII letters/digits/._-; values: 0..1024 UTF-8 bytes.
       delete_value=1 deletes key and requires empty value; 0 stores value.
       expected_version is required (0..2^53-1); conflicts never overwrite.
       Success returns the full current settings and incremented version. */
    PulseStatus(PULSE_CALL *settings_set)(void *context, PulseStringView key, PulseStringView value,
                                          uint32_t delete_value, uint64_t expected_version,
                                          PulseResourcesCompletionFn completion, void *user,
                                          PulseResourcesRequestId *request_id);
    PulseStatus(PULSE_CALL *audit_list)(void *context, uint32_t limit, PulseResourcesCompletionFn completion,
                                        void *user, PulseResourcesRequestId *request_id);
    PulseStatus(PULSE_CALL *cancel_request)(void *context, PulseResourcesRequestId request_id);
} PulseResourcesV1;
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
#endif

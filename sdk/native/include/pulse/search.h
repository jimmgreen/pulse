#ifndef PULSE_NATIVE_SEARCH_H
#define PULSE_NATIVE_SEARCH_H
#include "abi.h"
#ifdef __cplusplus
extern "C" {
#endif
#define PULSE_SEARCH_INTERFACE_NAME "pulse.search"
#define PULSE_SEARCH_VERSION_1 1u
#pragma pack(push, 8)
typedef uint64_t PulseSearchRequestId;

enum {
    PULSE_SEARCH_FLAG_NONE = 0u,
    PULSE_SEARCH_FLAG_PINYIN = 1u,
    PULSE_SEARCH_FLAG_FOLDERS_ONLY = 2u,
    PULSE_SEARCH_FLAG_FILES_ONLY = 4u,
    PULSE_SEARCH_ALL_FLAGS = 7u
};

enum {
    PULSE_SEARCH_HIT_NONE = 0u,
    PULSE_SEARCH_HIT_FOLDER = 1u
};

typedef struct PulseSearchRequest {
    uint32_t size;
    uint32_t flags;             /* PULSE_SEARCH_FLAG_* */
    uint32_t max_results;       /* 1..50 (0 defaults to 20). */
    uint32_t reserved;
    PulseStringView query;      /* UTF-8, 1..512 bytes. */
    PulseStringView kind_token; /* Optional UTF-8 kind filter (e.g. "image", "doc", "installer"), 0..64 bytes. */
    PulseStringView scope_directory; /* Optional absolute folder scope, 0..32768 bytes. */
} PulseSearchRequest;

typedef struct PulseSearchHit {
    PulseStringView path;
    PulseStringView name;
    uint64_t size_bytes;
    int64_t modified_unix_ms;
    uint32_t flags;
    uint32_t reserved;
} PulseSearchHit;

typedef struct PulseSearchReply {
    uint32_t size;
    PulseStatus status;
    PulseSearchRequestId request_id;
    uint64_t total_matches;
    const PulseSearchHit* hits;
    uint32_t hit_count;
    uint32_t index_connected;
} PulseSearchReply;

typedef void(PULSE_CALL *PulseSearchReplyFn)(void* user, const PulseSearchReply* reply);

/* UI-thread bounded asynchronous search over PulseIndex (with generation cancellation).
   Inputs are copied synchronously; callback runs at most once on the UI thread unless
   cancelled or session closes first. Borrowed reply slices expire at callback return. */
typedef struct PulseSearchV1 {
    uint32_t size, version;
    void* context;
    PulseStatus(PULSE_CALL *query)(void* context, const PulseSearchRequest* request,
        PulseSearchReplyFn callback, void* user, PulseSearchRequestId* out_request_id);
    PulseStatus(PULSE_CALL *cancel)(void* context, PulseSearchRequestId request_id);
} PulseSearchV1;
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
#endif

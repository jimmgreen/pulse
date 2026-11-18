#ifndef PULSE_NATIVE_PREVIEW_H
#define PULSE_NATIVE_PREVIEW_H
#include "abi.h"
#include "ui_model.h"
#ifdef __cplusplus
extern "C" {
#endif
#define PULSE_PREVIEW_INTERFACE_NAME "pulse.preview"
#define PULSE_PREVIEW_VERSION_1 1u
#pragma pack(push, 8)
typedef uint64_t PulsePreviewProviderId;
enum { PULSE_PREVIEW_ARCHIVE = 1u, PULSE_PREVIEW_BITMAP = 2u, PULSE_PREVIEW_UI_MODEL = 3u };
enum { PULSE_PREVIEW_ENTRY_DIRECTORY = 1u, PULSE_PREVIEW_ENTRY_ENCRYPTED = 2u };
typedef struct PulsePreviewArchiveEntry {
    PulseStringView path;
    uint64_t size_bytes, packed_bytes, modified_ticks; /* Windows FILETIME ticks. */
    uint32_t flags;
} PulsePreviewArchiveEntry;
typedef struct PulsePreviewResult {
    uint32_t size, kind;
    PulseStringView format;
    uint32_t incomplete;
    const PulsePreviewArchiveEntry* entries;
    uint32_t entry_count;
    uint32_t width, height, stride;
    const uint8_t* pixels;
    uint32_t byte_count;
    const PulseUiModelTree* model;
} PulsePreviewResult;
typedef struct PulsePreviewRequest {
    uint32_t size;
    PulseStringView path;
    uint64_t modified, size_bytes;
    uint32_t pixels;
    void* context;
    uint32_t(PULSE_CALL *cancelled)(void*);
    PulseStatus(PULSE_CALL *publish)(void*, const PulsePreviewResult*);
} PulsePreviewRequest;
typedef PulseStatus(PULSE_CALL *PulsePreviewFn)(void*, const PulsePreviewRequest*);
typedef struct PulsePreviewProvider {
    uint32_t size;
    PulseStringView extension; /* Lowercase dotted ASCII extension, 2..32 bytes. */
    PulsePreviewFn callback;
    void* user;
} PulsePreviewProvider;
typedef struct PulsePreviewV1 {
    uint32_t size, version;
    void* context;
    PulseStatus(PULSE_CALL *register_provider)(void*, const PulsePreviewProvider*, PulsePreviewProviderId*);
    PulseStatus(PULSE_CALL *unregister_provider)(void*, PulsePreviewProviderId);
} PulsePreviewV1;
/* Register/unregister on owner UI thread during entry, <=16 providers per instance.
   Provider runs on a host worker, <=2 concurrent calls per instance: synchronize
   shared userdata. Poll cancelled and return promptly; never call UI APIs there.
   publish synchronously copies borrowed data on that same worker; at most one
   successful result per call. Callback return is final; do not retain request or
   publish/cancel context. OK without a result fails. Cancellation discards output.
   Archive: <=4096 entries, <=4 MiB UTF-8 paths, format 1..128 bytes with no controls, paths <=32768.
   Bitmap: positive dimensions <=4096, stride >= width*4, byte_count=stride*height
   <=16 MiB; premultiplied BGRA. UI_MODEL synchronously copies and validates model; only
   local state/actions are supported. Declared native callback commands return
   UNAVAILABLE; data sources/context bindings retain model validation rules.
   unregister immediately retires the provider and cancels its work. It returns
   STATE while a provider callback is still running; keep userdata alive and retry
   on the UI thread until OK, or keep it through destroy. Retired providers cannot
   start new requests. Stop cancels but host retains DLL until every provider returns. No exceptions
   cross ABI. All strings are UTF-8; never free memory from the other side. */
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
#endif

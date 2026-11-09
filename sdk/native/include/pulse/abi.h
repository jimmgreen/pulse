#ifndef PULSE_NATIVE_ABI_H
#define PULSE_NATIVE_ABI_H
#include <stdint.h>
#if defined(_WIN32)
#define PULSE_CALL __cdecl
#else
#define PULSE_CALL
#endif
#ifdef __cplusplus
extern "C" {
#endif
#pragma pack(push, 8)
#define PULSE_ABI_VERSION_1 1u
/* SDK preview. Until SDK 1.0 any interface may change from one preview to the
   next, and the host loads only plugins built against its own preview. Every
   plugin writes PULSE_PLUGIN_SDK_PREVIEW() once at file scope; a plugin
   without it, or built against another preview, is refused before its entry
   point runs (PULSE_ERROR_SDK_PREVIEW). */
#define PULSE_SDK_PREVIEW 3u
typedef int32_t PulseStatus;
enum { PULSE_OK = 0, PULSE_ERROR_ARGUMENT = 1, PULSE_ERROR_ABI = 2,
       PULSE_ERROR_UNAVAILABLE = 3, PULSE_ERROR_INITIALIZATION = 4,
       PULSE_ERROR_STATE = 5, PULSE_ERROR_LOAD = 6, PULSE_ERROR_ENTRY = 7,
       PULSE_ERROR_PERMISSION = 8, PULSE_ERROR_SDK_PREVIEW = 9 };
/* UTF-8, borrowed for the duration of the call; length excludes any terminator. */
typedef struct PulseStringView { const char* data; uint32_t length; } PulseStringView;
typedef PulseStatus (PULSE_CALL *PulseQueryInterfaceFn)(void* context,
    PulseStringView name, uint32_t version, void* out_table, uint32_t out_size);
typedef struct PulseHostV1 {
    uint32_t size;
    uint32_t version;
    void* context;
    PulseQueryInterfaceFn query_interface;
} PulseHostV1;
typedef struct PulsePluginV1 {
    uint32_t size;
    uint32_t version;
    void* instance;
    PulseStatus (PULSE_CALL *activate)(void* instance);
    void (PULSE_CALL *deactivate)(void* instance);
    void (PULSE_CALL *destroy)(void* instance);
} PulsePluginV1;
/* Calls use the host UI thread, except explicitly any-thread interfaces such
   as pulse.dispatch post/cancel. deactivate must also undo partial activation
   after activate returns failure. destroy must synchronously stop plugin-owned
   threads and callbacks; arbitrary live DLL unload is not supported. The host
   never frees plugin allocations, and the plugin never frees host allocations.
   Native plugins are trusted code with the host process's OS authority. */
/* Host initializes size/version. On failure, any non-null instance must have a
   valid destroy callback. No exception may cross this boundary. Tables are
   copied by value; pointed-to host context lives through destroy. */
typedef PulseStatus (PULSE_CALL *PulsePluginEntryV1Fn)(const PulseHostV1* host,
    PulsePluginV1* plugin);
/* Exported as PulsePluginSdkPreview by PULSE_PLUGIN_SDK_PREVIEW(). */
typedef uint32_t (PULSE_CALL *PulsePluginSdkPreviewFn)(void);
#pragma pack(pop)
#ifdef __cplusplus
}
#define PULSE_PLUGIN_SDK_PREVIEW() \
    extern "C" __declspec(dllexport) uint32_t PULSE_CALL PulsePluginSdkPreview(void) { return PULSE_SDK_PREVIEW; }
#else
#define PULSE_PLUGIN_SDK_PREVIEW() \
    __declspec(dllexport) uint32_t PULSE_CALL PulsePluginSdkPreview(void) { return PULSE_SDK_PREVIEW; }
#endif
#endif

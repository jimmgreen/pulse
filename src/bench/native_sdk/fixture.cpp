#include <pulse/abi.h>
#include <new>
#ifndef FIXTURE_MODE
#define FIXTURE_MODE 0
#endif
#if FIXTURE_MODE == 4
extern "C" __declspec(dllimport) int PulseNativePrivateDependencyValue();
#endif
// Mode 5 is a plugin that never declared its SDK preview.
#if FIXTURE_MODE == 6
extern "C" __declspec(dllexport) uint32_t PULSE_CALL PulsePluginSdkPreview() { return PULSE_SDK_PREVIEW - 1; }
#elif FIXTURE_MODE != 5
PULSE_PLUGIN_SDK_PREVIEW()
#endif
#if FIXTURE_MODE != 2
namespace {
struct Counters { uint32_t size, version; int32_t* values; };
struct Instance { int32_t* counters; };
PulseStatus PULSE_CALL Activate(void* p) { ++static_cast<Instance*>(p)->counters[0]; return PULSE_OK; }
void PULSE_CALL Deactivate(void* p) { ++static_cast<Instance*>(p)->counters[1]; }
void PULSE_CALL Destroy(void* p) { auto* state = static_cast<Instance*>(p); ++state->counters[2]; delete state; }
}
#endif
#if FIXTURE_MODE == 2
extern "C" __declspec(dllexport) int MissingEntry() { return 0; }
#else
extern "C" __declspec(dllexport) PulseStatus PULSE_CALL PulsePluginEntryV1(const PulseHostV1* host, PulsePluginV1* out) {
#if FIXTURE_MODE == 4
    if (PulseNativePrivateDependencyValue() != 42) return PULSE_ERROR_INITIALIZATION;
#endif
    Counters counters{};
    if (host->query_interface(host->context, {"test.counters", 13}, 1, &counters, sizeof(counters)) != PULSE_OK) return PULSE_ERROR_INITIALIZATION;
    auto* p = new (std::nothrow) Instance{counters.values};
    if (!p) return PULSE_ERROR_INITIALIZATION;
    *out = {sizeof(*out), FIXTURE_MODE == 1 ? 999u : 1u, p, Activate, Deactivate, Destroy};
    return FIXTURE_MODE == 3 ? PULSE_ERROR_INITIALIZATION : PULSE_OK;
}
#endif

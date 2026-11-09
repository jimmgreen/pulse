#include <pulse/abi.h>
#include <windows.h>

PULSE_PLUGIN_SDK_PREVIEW()

extern "C" __declspec(dllexport) __declspec(noinline) void PULSE_CALL PulseCrashClick() {
    *static_cast<volatile int*>(nullptr) = 42;
}

namespace {
int instance = 0;
PulseStatus PULSE_CALL Activate(void*) {
#if CRASH_ON_ACTIVATE
    PulseCrashClick();
#endif
    return PULSE_OK;
}
void PULSE_CALL Finish(void*) {}
}

extern "C" __declspec(dllexport) PulseStatus PULSE_CALL PulsePluginEntryV1(
    const PulseHostV1*, PulsePluginV1* out) {
    *out = {sizeof(*out), PULSE_ABI_VERSION_1, &instance, Activate, Finish, Finish};
    return PULSE_OK;
}

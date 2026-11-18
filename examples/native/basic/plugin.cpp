#include <pulse/sdk.hpp>
#include <new>
namespace {
struct Plugin { pulse::sdk::Host host; bool active = false; explicit Plugin(const PulseHostV1& h) : host(h) {} };
PulseStatus PULSE_CALL Activate(void* p) { static_cast<Plugin*>(p)->active = true; return PULSE_OK; }
void PULSE_CALL Deactivate(void* p) { static_cast<Plugin*>(p)->active = false; }
void PULSE_CALL Destroy(void* p) { delete static_cast<Plugin*>(p); }
}
PULSE_PLUGIN_SDK_PREVIEW()
extern "C" __declspec(dllexport) PulseStatus PULSE_CALL PulsePluginEntryV1(const PulseHostV1* host, PulsePluginV1* out) {
    if (!host || !out || host->size < sizeof(*host) || out->size < sizeof(*out) ||
        host->version != PULSE_ABI_VERSION_1 || out->version != PULSE_ABI_VERSION_1) return PULSE_ERROR_ABI;
    auto* instance = new (std::nothrow) Plugin(*host);
    if (!instance) return PULSE_ERROR_INITIALIZATION;
    *out = {sizeof(*out), PULSE_ABI_VERSION_1, instance, Activate, Deactivate, Destroy};
    return PULSE_OK;
}

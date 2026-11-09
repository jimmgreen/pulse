#ifndef PULSE_NATIVE_SETTINGS_PAGES_H
#define PULSE_NATIVE_SETTINGS_PAGES_H
#include "ui_model.h"
#ifdef __cplusplus
extern "C" {
#endif
#define PULSE_SETTINGS_PAGES_INTERFACE_NAME "pulse.settings.pages"
#define PULSE_SETTINGS_PAGES_VERSION_1 1u
#pragma pack(push, 8)
typedef struct PulseSettingsPagesV1 {
    uint32_t size, version;
    void* context;
    PulseStatus(PULSE_CALL *create)(void*, PulseStringView title, const PulseUiModelTree*, uint64_t* page);
    PulseStatus(PULSE_CALL *query)(void*, uint64_t page, PulseStringView interface_name, uint32_t version, void* out, uint32_t size);
    PulseStatus(PULSE_CALL *remove)(void*, uint64_t page);
} PulseSettingsPagesV1;
/* UI thread only. One embedded settings page per package session. Title and model
   are copied synchronously. Page IDs are normal panel IDs. query exposes
   pulse.ui.model and pulse.ui.events on this dedicated page, independent of the
   main plugin panel. Mounting outside the host settings surface is unavailable.
   remove/stop invalidate IDs and subscriptions; callback userdata must remain
   valid until current callbacks return. Tables remain host-owned through destroy.
   Existing resource settings_get/settings_set provide persistence. */
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
#endif

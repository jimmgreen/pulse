#ifndef PULSE_NATIVE_SETTINGS_PAGES_H
#define PULSE_NATIVE_SETTINGS_PAGES_H
#include "ui_model.h"
#ifdef __cplusplus
extern "C" {
#endif
#define PULSE_SETTINGS_PAGES_INTERFACE_NAME "pulse.settings.pages"
#define PULSE_SETTINGS_PAGES_VERSION_1 1u
#define PULSE_SETTINGS_PAGES_VERSION_2 2u
#pragma pack(push, 8)
enum {
    PULSE_SETTINGS_STATUS_NONE = 0u,
    PULSE_SETTINGS_STATUS_SAVING = 1u,
    PULSE_SETTINGS_STATUS_SAVED = 2u,
    PULSE_SETTINGS_STATUS_WARNING = 3u,
    PULSE_SETTINGS_STATUS_ERROR = 4u
};
typedef struct PulseSettingsTabDesc {
    uint32_t size, flags;
    PulseStringView id, title, glyph;
    int32_t order;
    uint32_t reserved;
} PulseSettingsTabDesc;
typedef struct PulseSettingsPagesV1 {
    uint32_t size, version;
    void* context;
    PulseStatus(PULSE_CALL *create)(void*, PulseStringView title, const PulseUiModelTree*, uint64_t* page);
    PulseStatus(PULSE_CALL *query)(void*, uint64_t page, PulseStringView interface_name, uint32_t version, void* out, uint32_t size);
    PulseStatus(PULSE_CALL *remove)(void*, uint64_t page);
} PulseSettingsPagesV1;
typedef struct PulseSettingsPagesV2 {
    uint32_t size, version;
    void* context;
    PulseStatus(PULSE_CALL *create)(void*, PulseStringView title, const PulseUiModelTree*, uint64_t* page);
    PulseStatus(PULSE_CALL *query)(void*, uint64_t page, PulseStringView interface_name, uint32_t version, void* out, uint32_t size);
    PulseStatus(PULSE_CALL *remove)(void*, uint64_t page);
    PulseStatus(PULSE_CALL *create_tab)(void*, const PulseSettingsTabDesc* desc, const PulseUiModelTree*, uint64_t* page);
    PulseStatus(PULSE_CALL *set_status)(void*, uint64_t page, uint32_t kind, PulseStringView text);
    PulseStatus(PULSE_CALL *set_side_preview)(void*, uint64_t page, const PulseUiModelTree* preview, uint32_t width_dip);
} PulseSettingsPagesV2;
/* UI thread only. Embedded settings page(s) per package session (1 in V1, up to 6 tabs
   in V2). Title/descriptor and model are copied synchronously. Page IDs are normal panel IDs.
   query exposes pulse.ui.model, pulse.ui.events, and pulse.ui.data on each dedicated page,
   independent of the main plugin panel. Mounting outside the host settings surface is unavailable.
   set_status shows an auto-save/status pill in the settings header; set_side_preview attaches
   or clears a live read-only side preview panel. remove/stop invalidate IDs and subscriptions;
   callback userdata must remain valid until current callbacks return. Tables remain host-owned
   through destroy. Existing resource settings_get/settings_set provide persistence. */
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
#endif

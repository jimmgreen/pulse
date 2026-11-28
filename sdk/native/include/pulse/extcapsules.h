#ifndef PULSE_NATIVE_EXTCAPSULES_H
#define PULSE_NATIVE_EXTCAPSULES_H
#include "abi.h"
#ifdef __cplusplus
extern "C" {
#endif
#define PULSE_EXTCAPSULES_INTERFACE_NAME "pulse.extcapsules"
#define PULSE_EXTCAPSULES_VERSION_1 1u
#pragma pack(push, 8)

enum {
    PULSE_EXT_CAPSULE_FLAG_UPPERCASE = 1u << 0, /* Render "PSD" instead of "psd" */
    PULSE_EXT_CAPSULE_FLAG_STRIP_DOT = 1u << 1  /* Render "psd" instead of ".psd" */
};

/* One extension's inline filename capsule colors and text formatting.
   Colors use 0x00RRGGBB (opaque) or 0xAARRGGBB (alpha 0x00 treated as 0xFF for bg/text;
   border == 0 disables the outline). */
typedef struct PulseExtCapsuleRule {
    uint32_t size; /* sizeof(PulseExtCapsuleRule) */
    uint32_t flags; /* PULSE_EXT_CAPSULE_FLAG_* */
    PulseStringView extension; /* ASCII without leading dot, e.g. "psd", 1..24 bytes */
    uint32_t light_bg;
    uint32_t light_text;
    uint32_t light_border;
    uint32_t dark_bg;
    uint32_t dark_text;
    uint32_t dark_border;
} PulseExtCapsuleRule;

/* Shared box-model parameters for a package's capsules. Zero fields use host defaults. */
typedef struct PulseExtCapsuleStyle {
    uint32_t size; /* sizeof(PulseExtCapsuleStyle) */
    uint32_t corner_radius_milli; /* 0 = 4000 (4.0 DIP); else 0..12000 */
    uint32_t padding_x_milli; /* 0 = 5000 (5.0 DIP); else 1000..16000 */
    uint32_t padding_y_milli; /* 0 = 1000 (1.0 DIP); else 0..8000 */
    uint32_t border_width_milli; /* 0 = 1000 (1.0 DIP); else 0..3000 */
    uint32_t gap_milli; /* 0 = 4000 (4.0 DIP); else 0..16000 */
    int32_t font_size_delta_milli; /* 0 = -1000 (-1.0 DIP); range -4000..2000 */
    uint32_t font_weight; /* 0 = 600 (SemiBold); else 100..900 */
} PulseExtCapsuleStyle;

/* Declarative inline filename extension capsules for all file views (icon grids,
   details, list, tiles, content). All functions run on the owner UI thread.
   set_rules replaces the calling package's rule table atomically (up to 256 rules;
   style may be NULL for defaults). When multiple packages register the same
   extension, the first registered package wins. Closing the session removes every
   rule registered by that package. */
typedef struct PulseExtCapsulesV1 {
    uint32_t size, version;
    void* context;
    PulseStatus(PULSE_CALL *set_rules)(
        void* context,
        const PulseExtCapsuleStyle* style,
        const PulseExtCapsuleRule* rules,
        uint32_t rule_count);
    PulseStatus(PULSE_CALL *clear_rules)(void* context);
} PulseExtCapsulesV1;
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
#endif

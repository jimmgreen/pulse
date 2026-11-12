#ifndef PULSE_NATIVE_ICONS_H
#define PULSE_NATIVE_ICONS_H
#include "abi.h"
#ifdef __cplusplus
extern "C" {
#endif
#define PULSE_ICONS_INTERFACE_NAME "pulse.icons"
#define PULSE_ICONS_VERSION_1 1u
#define PULSE_ICON_BUILTIN_MAX 67u
#define PULSE_ICON_CUSTOM_MIN 0x10000u
#pragma pack(push, 8)
typedef uint32_t PulseIconId;
enum {
    PULSE_ICON_DEFAULT = 0u,
    PULSE_ICON_ARCHIVE = 1u,
    PULSE_ICON_EXTRACT = 2u,
    PULSE_ICON_SEARCH = 3u,
    PULSE_ICON_BOX = 4u,
    PULSE_ICON_EXTRACT_TO = 5u,
    PULSE_ICON_ADD_ARCHIVE = 6u,
    PULSE_ICON_TEST = 7u,
    PULSE_ICON_HASH = 8u,
    PULSE_ICON_GEAR = 9u,
    PULSE_ICON_FOLDER = 10u,
    PULSE_ICON_FILE = 11u,
    PULSE_ICON_CHEVRON_RIGHT = 12u,
    PULSE_ICON_CHEVRON_DOWN = 13u,
    PULSE_ICON_CHEVRON_UP = 14u,
    PULSE_ICON_MORE = 15u,
    PULSE_ICON_CHECK = 16u,
    PULSE_ICON_SHIELD = 17u,
    PULSE_ICON_BADGE = 18u,
    PULSE_ICON_LOCK = 19u,
    PULSE_ICON_EYE = 20u,
    PULSE_ICON_COPY = 21u,
    PULSE_ICON_CUT = 22u,
    PULSE_ICON_RENAME = 23u,
    PULSE_ICON_TRASH = 24u,
    PULSE_ICON_PROPERTIES = 25u,
    PULSE_ICON_OPEN = 26u,
    PULSE_ICON_OPEN_WITH = 27u,
    PULSE_ICON_PATH = 28u,
    PULSE_ICON_UNDO = 29u,
    PULSE_ICON_CLOSE = 30u,
    PULSE_ICON_PLUS = 31u,
    PULSE_ICON_MINUS = 32u,
    PULSE_ICON_SQUARE = 33u,
    PULSE_ICON_BACK = 34u,
    PULSE_ICON_FORWARD = 35u,
    PULSE_ICON_UP = 36u,
    PULSE_ICON_REFRESH = 37u,
    PULSE_ICON_IMAGE = 38u,
    PULSE_ICON_CAMERA = 39u,
    PULSE_ICON_FILM = 40u,
    PULSE_ICON_TEXT_SEARCH = 41u,
    PULSE_ICON_SPARKLE = 42u,
    PULSE_ICON_PUZZLE = 43u,
    PULSE_ICON_PALETTE = 44u,
    PULSE_ICON_LIST_VIEW = 45u,
    PULSE_ICON_TABS = 46u,
    PULSE_ICON_POWER = 47u,
    PULSE_ICON_DUPLICATE = 48u,
    PULSE_ICON_CUBE = 49u,
    PULSE_ICON_LINES = 50u,
    PULSE_ICON_KEYBOARD = 51u,
    PULSE_ICON_INFO = 52u,
    PULSE_ICON_WARN = 53u,
    PULSE_ICON_SIDEBAR = 54u,
    PULSE_ICON_PANES = 55u,
    PULSE_ICON_CONTRAST = 56u,
    PULSE_ICON_DOWNLOAD = 57u,
    PULSE_ICON_CLOUD_OFF = 58u,
    PULSE_ICON_DRAG = 59u,
    PULSE_ICON_SORT = 60u,
    PULSE_ICON_FILTER = 61u,
    PULSE_ICON_NEW_FOLDER = 62u,
    PULSE_ICON_STAR = 63u,
    PULSE_ICON_PC = 64u,
    PULSE_ICON_DISK = 65u,
    PULSE_ICON_CHEVRON_LEFT = 66u,
    PULSE_ICON_KEY = 67u
};
enum {
    PULSE_ICON_DRAW_STROKE = 1u,
    PULSE_ICON_DRAW_FILL = 2u,
    PULSE_ICON_DRAW_CLOSED = 4u
};
enum {
    PULSE_ICON_COLOR_INHERIT = 0u,
    PULSE_ICON_COLOR_ACCENT = 1u,
    PULSE_ICON_COLOR_SUCCESS = 2u,
    PULSE_ICON_COLOR_WARNING = 3u,
    PULSE_ICON_COLOR_DANGER = 4u,
    PULSE_ICON_COLOR_RGB = 5u
};
typedef struct PulseIconLayer {
    uint32_t size;
    uint32_t draw_flags; /* Non-zero combination of PULSE_ICON_DRAW_* flags. */
    PulseStringView path_data; /* SVG path commands M L H V C S A Z, 1..2048 bytes. */
    uint32_t color_role; /* PULSE_ICON_COLOR_*; RGB uses 0xAARRGGBB or 0x00RRGGBB in argb. */
    uint32_t argb;
    uint32_t stroke_width_milli; /* 0 = host default (~1350 milli-DIP); else 250..8000. */
    uint32_t opacity_milli; /* 0 = 1000 (opaque); else 1..1000. */
} PulseIconLayer;
typedef struct PulseIconDescriptor {
    uint32_t size;
    uint32_t view_box_milli; /* 0 = 16000 (16x16 grid); else 4000..1024000. */
    PulseStringView name; /* Optional package-local ASCII [A-Za-z0-9._-], 0..64 bytes. */
    const PulseIconLayer* layers; /* 1..32 layers; total path bytes <= 8192. */
    uint32_t layer_count;
    uint32_t reserved;
} PulseIconDescriptor;
typedef struct PulseIconsV1 {
    uint32_t size;
    uint32_t version;
    void* context;
    PulseStatus(PULSE_CALL *register_icon)(void*, const PulseIconDescriptor*, PulseIconId*);
    PulseStatus(PULSE_CALL *update_icon)(void*, PulseIconId, const PulseIconDescriptor*);
    PulseStatus(PULSE_CALL *unregister_icon)(void*, PulseIconId);
    PulseStatus(PULSE_CALL *resolve_builtin)(void*, PulseStringView, PulseIconId*);
} PulseIconsV1;
/* UI thread only; <=64 dynamic icons per plugin session. Returned PulseIconId values
   (>= PULSE_ICON_CUSTOM_MIN) work in pulse.commands, pulse.dialogs, toolbar pins,
   and UiTree glyphs ("icon:#<id>" or "icon:<package>/<name>"). */
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
#endif

#ifndef PULSE_NATIVE_UI_MODEL_H
#define PULSE_NATIVE_UI_MODEL_H
#include "ui.h"
#ifdef __cplusplus
extern "C" {
#endif
#pragma pack(push, 8)
#define PULSE_UI_MODEL_INTERFACE "pulse.ui.model"
#define PULSE_UI_MODEL_VERSION 1u
/* General-purpose controls backed by the existing renderer. All borrowed arrays/strings are
   copied synchronously. Optional fields use present=0/1; absent preserves host defaults.
   UTF-8 byte budget 65536; nodes 128/depth8; menus128/depth4; total data rows4096/depth8.
   This C ABI uses the same property semantics as the existing UiTree contract.
   Structure submission uses a temporary in-memory DOM for shared validation, never
   JSON text serialization/parsing. High-frequency set_states remains typed directly. */
typedef struct PulseUiModelString { uint32_t present; PulseStringView value; } PulseUiModelString;
typedef struct PulseUiModelBool { uint32_t present; uint32_t value; } PulseUiModelBool;
typedef struct PulseUiModelInt { uint32_t present; int64_t value; } PulseUiModelInt;
typedef struct PulseUiModelStrings { uint32_t present; const PulseStringView* data; uint32_t count; } PulseUiModelStrings;
enum {
    PULSE_UI_MODEL_STACK = 0,
    PULSE_UI_MODEL_GRID = 1,
    PULSE_UI_MODEL_GROUP = 2,
    PULSE_UI_MODEL_SCROLL_VIEW = 3,
    PULSE_UI_MODEL_TEXT = 4,
    PULSE_UI_MODEL_BUTTON = 5,
    PULSE_UI_MODEL_TEXT_INPUT = 6,
    PULSE_UI_MODEL_CHECKBOX = 7,
    PULSE_UI_MODEL_SELECT = 8,
    PULSE_UI_MODEL_PROGRESS = 9,
    PULSE_UI_MODEL_DIVIDER = 10,
    PULSE_UI_MODEL_TABS = 11,
    PULSE_UI_MODEL_TAB = 12,
    PULSE_UI_MODEL_SWITCH = 14,
    PULSE_UI_MODEL_RADIO = 15,
    PULSE_UI_MODEL_SLIDER = 16,
    PULSE_UI_MODEL_SEGMENTED = 17,
    PULSE_UI_MODEL_BADGE = 18,
    PULSE_UI_MODEL_TAG = 19,
    PULSE_UI_MODEL_INFO_BAR = 20,
    PULSE_UI_MODEL_SPLIT_BUTTON = 21,
    PULSE_UI_MODEL_MENU_BUTTON = 22,
    PULSE_UI_MODEL_EMPTY_STATE = 25,
    PULSE_UI_MODEL_VIRTUAL_LIST = 26,
    PULSE_UI_MODEL_TREE = 27,
    PULSE_UI_MODEL_DATA_TABLE = 28,
    PULSE_UI_MODEL_SPLIT_PANE = 31,
    /* Reserved holes are invalid types; COUNT is the numeric range upper bound. */
    PULSE_UI_MODEL_COUNT = 41
};
typedef struct PulseUiModelMenuItem PulseUiModelMenuItem;
typedef struct PulseUiModelMenus { uint32_t present; const PulseUiModelMenuItem* data; uint32_t count; } PulseUiModelMenus;
struct PulseUiModelMenuItem {
    uint32_t size;
    PulseUiModelString id, text, command, glyph, shortcut;
    PulseUiModelBool enabled, checked, separator;
    PulseUiModelMenus children;
};
typedef struct PulseUiModelRow PulseUiModelRow;
typedef struct PulseUiModelRows { uint32_t present; const PulseUiModelRow* data; uint32_t count; } PulseUiModelRows;
struct PulseUiModelRow {
    uint32_t size;
    PulseStringView id;
    PulseUiModelString text, glyph, command;
    PulseUiModelStrings cells;
    PulseUiModelRows children;
    PulseUiModelBool expanded, enabled;
};
typedef struct PulseUiModelColumn { uint32_t size; PulseStringView id, text; PulseUiModelInt width; } PulseUiModelColumn;
typedef struct PulseUiModelColumns { uint32_t present; const PulseUiModelColumn* data; uint32_t count; } PulseUiModelColumns;
typedef struct PulseUiModelValidation {
    uint32_t size;
    PulseUiModelBool required;
    PulseUiModelInt min_length, max_length;
    PulseStringView message;
} PulseUiModelValidation;
/* Reserved actual UiTree provider shape. Non-null data_source is UNAVAILABLE until
   owner-scoped provider/resolver integration is implemented; no fake static fallback. */
typedef struct PulseUiModelDataSource {
    uint32_t size;
    PulseStringView source;
    PulseUiModelInt page_size;
    PulseUiModelString filter_state, sort_state, status_state, error_state, progress_state;
} PulseUiModelDataSource;
typedef struct PulseUiModelNode PulseUiModelNode;
typedef struct PulseUiModelNodes { uint32_t present; const PulseUiModelNode* data; uint32_t count; } PulseUiModelNodes;
struct PulseUiModelNode {
    uint32_t size, type;
    PulseStringView id;
    PulseUiModelString text;
    PulseUiModelString command;
    PulseUiModelString binding;
    PulseUiModelString state;
    PulseUiModelString orientation;
    PulseUiModelString placeholder;
    PulseUiModelString glyph;
    PulseUiModelString keycap;
    PulseUiModelString detail;
    PulseUiModelString message;
    PulseUiModelString color;
    PulseUiModelString close_command;
    PulseUiModelString enabled_binding;
    PulseUiModelString visible_binding;
    PulseUiModelString role;
    PulseUiModelString alignment;
    PulseUiModelString badge_text;
    PulseUiModelString status;
    PulseUiModelString trailing_glyph;
    PulseUiModelString summary;
    PulseUiModelString filter_text;
    PulseUiModelString filter_command;
    PulseUiModelString sort_command;
    PulseUiModelString modified;
    PulseUiModelString file_type;
    PulseUiModelString file_size;
    PulseUiModelString helper;
    PulseUiModelString count_label;
    PulseUiModelString action_text;
    PulseUiModelString center_text;
    PulseUiModelString right_text;
    PulseUiModelString kind;
    PulseUiModelBool checked;
    PulseUiModelBool enabled;
    PulseUiModelBool visible;
    PulseUiModelBool icon_only;
    PulseUiModelBool bordered;
    PulseUiModelBool selected;
    PulseUiModelBool current;
    PulseUiModelBool separator;
    PulseUiModelBool dot;
    PulseUiModelBool move_intent;
    PulseUiModelBool expanded;
    PulseUiModelBool cut;
    PulseUiModelBool missing;
    PulseUiModelBool ready;
    PulseUiModelInt value;
    PulseUiModelInt columns;
    PulseUiModelInt gap;
    PulseUiModelInt min_height;
    PulseUiModelInt min_width;
    PulseUiModelInt max_width;
    PulseUiModelInt max_height;
    PulseUiModelInt flex;
    PulseUiModelInt stops;
    PulseUiModelInt item_count;
    PulseUiModelInt capacity;
    PulseUiModelInt free_bytes;
    PulseUiModelInt total_bytes;
    PulseUiModelNodes children;
    PulseUiModelStrings options, tag_colors;
    PulseUiModelMenus menu, actions;
    PulseUiModelRows rows;
    PulseUiModelColumns data_columns; /* DataTable columns; integer columns belongs to Grid. */
    const PulseUiModelValidation* validation;
    const PulseUiModelDataSource* data_source;
};
typedef struct PulseUiModelDataOperation { uint32_t size; PulseStringView target, operation; } PulseUiModelDataOperation;
typedef struct PulseUiModelAction {
    uint32_t size;
    PulseStringView id;
    const PulseUiState* updates;
    uint32_t update_count;
    PulseUiModelStrings validate;
    const PulseUiModelDataOperation* data_operation; /* Non-null returns UNAVAILABLE. */
} PulseUiModelAction;
typedef struct PulseUiModelTree {
    uint32_t size;
    const PulseUiModelNode* root;
    const PulseUiState* states;
    uint32_t state_count;
    const PulseUiModelAction* actions;
    uint32_t action_count;
    const PulseStringView* commands; /* Declared native callback IDs, <=128; separate from local actions. */
    uint32_t command_count;
} PulseUiModelTree;
typedef struct PulseUiModelV1 {
    uint32_t size, version;
    void* context;
    PulseStatus (PULSE_CALL *create)(void*, PulseStringView title, const PulseUiModelTree*, uint64_t* panel);
    PulseStatus (PULSE_CALL *update)(void*, uint64_t panel, const PulseUiModelTree*);
    PulseStatus (PULSE_CALL *subscribe_command)(void*, uint64_t panel, PulseStringView command,
        PulseUiClickFn callback, void* user, uint64_t* subscription);
    PulseStatus (PULSE_CALL *set_states)(void*, uint64_t panel, const PulseUiState*, uint32_t count);
    /* needed excludes terminator. Null buffer/capacity 0 queries size. No partial copy. */
    PulseStatus (PULSE_CALL *get_state)(void*, uint64_t panel, PulseStringView key, char* buffer,
        uint32_t capacity, uint32_t* needed);
    PulseStatus (PULSE_CALL *unsubscribe)(void*, uint64_t subscription);
    PulseStatus (PULSE_CALL *close)(void*, uint64_t panel);
    /* Callback user pointers belong to the plugin; unsubscribe/close invalidate them synchronously.
       Panel/subscription IDs are never reused. No exception may cross this ABI.
       Updates validate atomically, retain existing state keys and registrations by command ID.
       callback's string argument is command ID (including menu/close/filter/sort/row),
       not node ID. Local actions mutate state and never invoke native callbacks.
       context.* bindings are UNAVAILABLE until an owner resolver is connected. */
} PulseUiModelV1;
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
#endif

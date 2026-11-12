#ifndef PULSE_NATIVE_COMMANDS_H
#define PULSE_NATIVE_COMMANDS_H
#include "context.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#define PULSE_COMMANDS_INTERFACE_NAME "pulse.commands"
#define PULSE_COMMANDS_VERSION_1 1u
#define PULSE_COMMANDS_VERSION_2 2u
#define PULSE_COMMAND_ALL_FLAGS 63u
#pragma pack(push, 8)
typedef uint64_t PulseCommandId;
enum { PULSE_COMMAND_FILES = 1u, PULSE_COMMAND_DIRECTORIES = 2u, PULSE_COMMAND_BACKGROUND = 4u };
enum { PULSE_COMMAND_CTRL = 1u, PULSE_COMMAND_SHIFT = 2u, PULSE_COMMAND_ALT = 4u };
enum { PULSE_COMMAND_ICON_DEFAULT = 0u, PULSE_COMMAND_ICON_ARCHIVE = 1u, PULSE_COMMAND_ICON_EXTRACT = 2u };
typedef void(PULSE_CALL *PulseCommandFn)(void*, PulseCommandId, const PulseContextSelectionSnapshot*);
typedef struct PulseCommandDescriptor {
    uint32_t size;
    PulseStringView id; /* Package-local ASCII [A-Za-z0-9._-], 1..64 bytes. */
    PulseStringView title; /* UTF-8, 1..128 bytes; copied during registration. */
    uint32_t icon; /* Built-in PULSE_ICON_* (0..67) or custom PulseIconId (>= PULSE_ICON_CUSTOM_MIN). */
    uint32_t kinds; /* Allowed entry kinds (FILES | DIRECTORIES | BACKGROUND). */
    uint32_t min_selection; /* 0 allowed when PULSE_COMMAND_BACKGROUND is set. */
    uint32_t max_selection; /* 0..4096; must be >= min_selection. */
    const PulseStringView* extensions; /* Lowercase dotted extensions; <=32, each <=32 bytes. */
    uint32_t extension_count; /* 0: unrestricted. Otherwise only matching files. */
    uint32_t exclude_extensions; /* 0 include list, 1 exclude list. */
    uint32_t virtual_key; /* 0 or A..Z / 0..9 / F1..F24. No process-global hotkey. */
    uint32_t modifiers; /* CTRL/SHIFT/ALT; a letter/digit requires CTRL or ALT. */
    PulseCommandFn invoke;
    void* user;
    /* Optional, read only when size covers them (older descriptors end at user). */
    PulseStringView detail; /* UTF-8, 0..160 bytes: a second line under the title in the
                               context menu, saying what the command will produce.
                               "{stem}" = the single selected entry's name without its last
                               extension (the line is hidden for 2+ entries);
                               "{base}" = "{stem}" for one entry, else the selection folder's name. */
    PulseStringView group; /* UTF-8, 0..32 bytes: commands of one package sharing a group are
                              listed together in the context menu under this caption. */
    uint32_t flags; /* Combination of PULSE_COMMAND_* presentation/visibility flags. */
    /* Optional V3 fields, read only when size >= sizeof(PulseCommandDescriptor). */
    PulseStringView icon_name; /* UTF-8, 0..96 bytes: built-in icon name or package custom icon id; overrides icon when non-empty. */
    uint32_t group_icon; /* Optional icon ID for submenu header when PULSE_COMMAND_GROUP_SUBMENU is set. */
    uint32_t reserved_v3;
} PulseCommandDescriptor;
enum {
    PULSE_COMMAND_GROUP_SUBMENU = 1u,
    PULSE_COMMAND_PRIMARY = 2u,
    PULSE_COMMAND_SEPARATOR_BEFORE = 4u,
    PULSE_COMMAND_SEPARATOR_AFTER = 8u,
    PULSE_COMMAND_HIDDEN = 16u,
    PULSE_COMMAND_CHECKED = 32u
};
/* Descriptors from SDKs before detail/group/flags end here. */
#define PULSE_COMMAND_DESCRIPTOR_SIZE_V1 ((uint32_t)offsetof(PulseCommandDescriptor, detail))
/* Descriptors from SDKs before icon_name/group_icon end here. */
#define PULSE_COMMAND_DESCRIPTOR_SIZE_V2 ((uint32_t)offsetof(PulseCommandDescriptor, icon_name))
typedef struct PulseCommandsV1 {
    uint32_t size;
    uint32_t version;
    void* context;
    PulseStatus(PULSE_CALL *register_command)(void*, const PulseCommandDescriptor*, PulseCommandId*);
    PulseStatus(PULSE_CALL *unregister_command)(void*, PulseCommandId);
} PulseCommandsV1;
typedef struct PulseCommandsV2 {
    uint32_t size;
    uint32_t version;
    void* context;
    PulseStatus(PULSE_CALL *register_command)(void*, const PulseCommandDescriptor*, PulseCommandId*);
    PulseStatus(PULSE_CALL *unregister_command)(void*, PulseCommandId);
    PulseStatus(PULSE_CALL *update_command)(void*, PulseCommandId, const PulseCommandDescriptor*);
    PulseStatus(PULSE_CALL *set_command_flags)(void*, PulseCommandId, uint32_t);
} PulseCommandsV2;
/* UI thread only; <=32 commands per attachment. Register during plugin entry so
   menus can discover commands without activating/opening a plugin panel. Entry
   must return promptly. IDs are never reused; stop invalidates menu callbacks.
   Host rechecks conditions at invocation, captures loaded selection through
   pulse.context and borrows the snapshot through callback return. Copy before
   starting worker work. Callbacks must return promptly; no exceptions cross ABI.
   Duplicate IDs or shortcuts in the same owner are rejected. Built-in shortcuts
   take precedence; text editors do not dispatch plugin shortcuts. */
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
#endif

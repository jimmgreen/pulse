#ifndef PULSE_NATIVE_COMMANDS_H
#define PULSE_NATIVE_COMMANDS_H
#include "context.h"
#ifdef __cplusplus
extern "C" {
#endif
#define PULSE_COMMANDS_INTERFACE_NAME "pulse.commands"
#define PULSE_COMMANDS_VERSION_1 1u
#pragma pack(push, 8)
typedef uint64_t PulseCommandId;
enum { PULSE_COMMAND_FILES = 1u, PULSE_COMMAND_DIRECTORIES = 2u };
enum { PULSE_COMMAND_CTRL = 1u, PULSE_COMMAND_SHIFT = 2u, PULSE_COMMAND_ALT = 4u };
enum { PULSE_COMMAND_ICON_DEFAULT = 0u, PULSE_COMMAND_ICON_ARCHIVE = 1u, PULSE_COMMAND_ICON_EXTRACT = 2u };
typedef void(PULSE_CALL *PulseCommandFn)(void*, PulseCommandId, const PulseContextSelectionSnapshot*);
typedef struct PulseCommandDescriptor {
    uint32_t size;
    PulseStringView id; /* Package-local ASCII [A-Za-z0-9._-], 1..64 bytes. */
    PulseStringView title; /* UTF-8, 1..128 bytes; copied during registration. */
    uint32_t icon;
    uint32_t kinds; /* Allowed entry kinds. Every selected entry must match. */
    uint32_t min_selection;
    uint32_t max_selection; /* 0..4096; must be >= min_selection. */
    const PulseStringView* extensions; /* Lowercase dotted extensions; <=32, each <=32 bytes. */
    uint32_t extension_count; /* 0: unrestricted. Otherwise only matching files. */
    uint32_t exclude_extensions; /* 0 include list, 1 exclude list. */
    uint32_t virtual_key; /* 0 or A..Z / 0..9 / F1..F24. No process-global hotkey. */
    uint32_t modifiers; /* CTRL/SHIFT/ALT; a letter/digit requires CTRL or ALT. */
    PulseCommandFn invoke;
    void* user;
} PulseCommandDescriptor;
typedef struct PulseCommandsV1 {
    uint32_t size;
    uint32_t version;
    void* context;
    PulseStatus(PULSE_CALL *register_command)(void*, const PulseCommandDescriptor*, PulseCommandId*);
    PulseStatus(PULSE_CALL *unregister_command)(void*, PulseCommandId);
} PulseCommandsV1;
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

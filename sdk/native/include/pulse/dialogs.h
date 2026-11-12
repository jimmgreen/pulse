#ifndef PULSE_NATIVE_DIALOGS_H
#define PULSE_NATIVE_DIALOGS_H
#include "ui_model.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#define PULSE_DIALOGS_INTERFACE_NAME "pulse.dialogs"
#define PULSE_DIALOGS_VERSION_1 1u
#define PULSE_DIALOGS_VERSION_2 2u
#define PULSE_DIALOG_NO_BUTTON 0xFFFFFFFFu
#define PULSE_DIALOG_ICON_EXT_FLAG 0x40000000u
#pragma pack(push, 8)
enum { PULSE_DIALOG_BUTTON_STANDARD = 0, PULSE_DIALOG_BUTTON_PRIMARY = 1, PULSE_DIALOG_BUTTON_DANGER = 2 };
typedef struct PulseDialogButton {
    uint32_t size;
    uint32_t kind;
    PulseStringView id;
    PulseStringView text;
} PulseDialogButton;
typedef struct PulseDialogRequest {
    uint32_t size;
    uint32_t width;
    uint32_t height;
    uint32_t default_button;
    uint32_t cancel_button;
    uint32_t button_count;
    PulseStringView title;
    PulseStringView message;
    const PulseDialogButton* buttons;
    const PulseUiModelTree* model;
    /* Optional, read only when size covers them (older requests end at model). */
    PulseStringView heading; /* UTF-8, 0..128 bytes: a large line above the message;
                                title then only names the window. */
    uint32_t icon; /* PULSE_DIALOG_ICON_*: drawn before the heading. */
    uint32_t subject_kind; /* PULSE_DIALOG_SUBJECT_*: a card naming what the dialog is about. */
    PulseStringView subject; /* UTF-8, 0..260 bytes: the card's name line. */
    PulseStringView subject_detail; /* UTF-8, 0..256 bytes: the card's second line. */
    PulseStringView footnote; /* UTF-8, 0..256 bytes: a hint left of the buttons. */
    uint32_t footnote_icon; /* PULSE_DIALOG_ICON_* before the footnote. */
} PulseDialogRequest;
#define PULSE_DIALOG_REQUEST_SIZE_V1 ((uint32_t)offsetof(PulseDialogRequest, heading))
enum { PULSE_DIALOG_ICON_NONE = 0u, PULSE_DIALOG_ICON_KEY = 1u, PULSE_DIALOG_ICON_ARCHIVE = 2u,
       PULSE_DIALOG_ICON_CHECKSUM = 3u, PULSE_DIALOG_ICON_INFO = 4u, PULSE_DIALOG_ICON_WARNING = 5u,
       PULSE_DIALOG_ICON_FOLDER = 6u, PULSE_DIALOG_ICON_LOCK = 7u };
enum { PULSE_DIALOG_SUBJECT_NONE = 0u, PULSE_DIALOG_SUBJECT_FILE = 1u, PULSE_DIALOG_SUBJECT_FOLDER = 2u,
       PULSE_DIALOG_SUBJECT_ARCHIVE = 3u };
typedef void (PULSE_CALL *PulseDialogStateChanged)(void* user, uint64_t dialog, const PulseUiState* changes, uint32_t count);
typedef void (PULSE_CALL *PulseDialogClosed)(void* user, uint64_t dialog, PulseStatus status, PulseStringView button,
    const PulseUiState* states, uint32_t count);
typedef void (PULSE_CALL *PulseDialogCommandFn)(void* user, uint64_t dialog, PulseStringView command);
typedef struct PulseDialogsV1 {
    uint32_t size, version;
    void* context;
    PulseStatus(PULSE_CALL *show)(void*, const PulseDialogRequest*, PulseDialogStateChanged, PulseDialogClosed,
        void* user, uint64_t* dialog);
    PulseStatus(PULSE_CALL *set_states)(void*, uint64_t dialog, const PulseUiState* states, uint32_t count);
    PulseStatus(PULSE_CALL *close)(void*, uint64_t dialog);
} PulseDialogsV1;
typedef struct PulseDialogsV2 {
    uint32_t size, version;
    void* context;
    PulseStatus(PULSE_CALL *show)(void*, const PulseDialogRequest*, PulseDialogStateChanged, PulseDialogClosed,
        void* user, uint64_t* dialog);
    PulseStatus(PULSE_CALL *set_states)(void*, uint64_t dialog, const PulseUiState* states, uint32_t count);
    PulseStatus(PULSE_CALL *close)(void*, uint64_t dialog);
    PulseStatus(PULSE_CALL *subscribe_command)(void*, uint64_t dialog, PulseStringView command,
        PulseDialogCommandFn callback, void* user, uint64_t* subscription);
    PulseStatus(PULSE_CALL *unsubscribe_command)(void*, uint64_t subscription);
    PulseStatus(PULSE_CALL *update_model)(void*, uint64_t dialog, const PulseUiModelTree* model);
    PulseStatus(PULSE_CALL *get_state)(void*, uint64_t dialog, PulseStringView key, char* buffer,
        uint32_t capacity, uint32_t* needed);
} PulseDialogsV2;
/* A host-drawn modal dialog over the owner window: title, optional message, an
   optional UI-model form and 1-3 footer buttons. UI thread only; show returns
   at once and the owner window stays disabled via PickerOwnerGuard until the
   dialog closes. One dialog per package session; a second show returns STATE.
   width: DIP, 0 = 480, clamped 360-720. height: form area DIP, 0 = 240 with a
   model and 0 without; clamped to 640. Title 1-128 UTF-8 bytes, message up to
   2048, button ids 1-64 and texts 1-32. default_button answers Enter;
   cancel_button answers Esc and the close button (PULSE_DIALOG_NO_BUTTON: they
   close with status STATE and an empty button id). The model is copied and
   validated like pulse.ui.model; text inputs with kind "password" mask their
   text. V1 rejects declared model commands with UNAVAILABLE; V2 enables
   declared model commands via subscribe_command/unsubscribe_command, dynamic
   tree replacement via update_model (retaining matching state values and
   command subscriptions), and synchronous state reads via get_state. Nested
   pulse.pickers calls while a dialog is open automatically parent to the dialog
   window. state_changed (optional) reports form edits; set_states may update
   the form from it or later. closed runs exactly once with every declared
   state, status OK and the pressed button id, unless the session stops first:
   then no callback runs. All borrowed values are valid only during the
   callback. Keep userdata until closed returns or the session stops.
   close(dialog) answers like the cancel button. */
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
#endif

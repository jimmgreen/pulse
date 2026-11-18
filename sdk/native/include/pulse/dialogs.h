#ifndef PULSE_NATIVE_DIALOGS_H
#define PULSE_NATIVE_DIALOGS_H
#include "ui_model.h"
#ifdef __cplusplus
extern "C" {
#endif
#define PULSE_DIALOGS_INTERFACE_NAME "pulse.dialogs"
#define PULSE_DIALOGS_VERSION_1 1u
#define PULSE_DIALOG_NO_BUTTON 0xFFFFFFFFu
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
} PulseDialogRequest;
typedef void (PULSE_CALL *PulseDialogStateChanged)(void* user, uint64_t dialog, const PulseUiState* changes, uint32_t count);
typedef void (PULSE_CALL *PulseDialogClosed)(void* user, uint64_t dialog, PulseStatus status, PulseStringView button,
    const PulseUiState* states, uint32_t count);
typedef struct PulseDialogsV1 {
    uint32_t size, version;
    void* context;
    PulseStatus(PULSE_CALL *show)(void*, const PulseDialogRequest*, PulseDialogStateChanged, PulseDialogClosed,
        void* user, uint64_t* dialog);
    PulseStatus(PULSE_CALL *set_states)(void*, uint64_t dialog, const PulseUiState* states, uint32_t count);
    PulseStatus(PULSE_CALL *close)(void*, uint64_t dialog);
} PulseDialogsV1;
/* A host-drawn modal dialog over the owner window: title, optional message, an
   optional UI-model form and 1-3 footer buttons. UI thread only; show returns
   at once and the owner window stays disabled until the dialog closes. One
   dialog per package session; a second show returns STATE.
   width: DIP, 0 = 480, clamped 360-720. height: form area DIP, 0 = 240 with a
   model and 0 without; clamped to 640. Title 1-128 UTF-8 bytes, message up to
   2048, button ids 1-64 and texts 1-32. default_button answers Enter;
   cancel_button answers Esc and the close button (PULSE_DIALOG_NO_BUTTON: they
   close with status STATE and an empty button id). The model is copied and
   validated like pulse.ui.model; text inputs with kind "password" mask their
   text. Model commands are unavailable: use states and the footer buttons.
   state_changed (optional) reports form edits; set_states may update the form
   from it or later. closed runs exactly once with every declared state, status
   OK and the pressed button id, unless the session stops first: then no callback
   runs. All borrowed values are valid only during the callback. Keep userdata
   until closed returns or the session stops. close(dialog) answers like the
   cancel button. */
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
#endif

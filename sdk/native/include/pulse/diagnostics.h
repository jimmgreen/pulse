#ifndef PULSE_NATIVE_DIAGNOSTICS_H
#define PULSE_NATIVE_DIAGNOSTICS_H
#include "abi.h"
#ifdef __cplusplus
extern "C" {
#endif
#define PULSE_DIAGNOSTICS_INTERFACE_NAME "pulse.diagnostics"
#define PULSE_DIAGNOSTICS_VERSION_1 1u
#pragma pack(push, 8)
/* Per-session structured error record for native host interfaces (pulse.ui.model,
   pulse.dialogs, pulse.settings.pages, pulse.pickers, pulse.workbench, etc.).
   UI thread only. Failed calls overwrite the session's last error and advance
   the monotonic sequence counter; successful calls leave the previous failure
   intact until clear_last_error() is called. When no error has been recorded
   (or after clear_last_error), status is PULSE_OK, sequence is 0, and string
   views are empty. All PulseStringView fields in PulseDiagnosticInfo are
   borrowed only for the duration of PulseDiagnosticFn; copy before returning.
   No C++ exception may cross this ABI. */
typedef struct PulseDiagnosticInfo {
    uint32_t size;
    PulseStatus status;
    uint64_t sequence;
    PulseStringView interface_name;
    PulseStringView operation;
    PulseStringView subject;
    PulseStringView field;
    PulseStringView message;
} PulseDiagnosticInfo;

typedef void(PULSE_CALL *PulseDiagnosticFn)(void* user, const PulseDiagnosticInfo* info);

typedef struct PulseDiagnosticsV1 {
    uint32_t size, version;
    void* context;
    PulseStatus(PULSE_CALL *get_last_error)(void* context, PulseDiagnosticFn callback, void* user);
    PulseStatus(PULSE_CALL *clear_last_error)(void* context);
} PulseDiagnosticsV1;
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
#endif

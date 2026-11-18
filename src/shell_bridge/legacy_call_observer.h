#pragma once
#include <windows.h>
#include <commdlg.h>
#include <cstddef>

namespace pulse::shell_bridge {
struct LegacyCallObservation {
    DWORD structure_size = 0, flags = 0;
    bool complete = false, hook = false, template_name = false, custom_data = false;
};
// Passive diagnostics only: no dereferencing caller strings, hooks or templates.
// Restore entry LastError before the original call, and leave its result intact.
template<class Ofn, class Original, class Observe>
BOOL ObserveLegacyCall(Ofn* input, Original original, Observe&& observe) {
    const DWORD entry_error = GetLastError();
    LegacyCallObservation value;
    if (input) {
        value.structure_size = input->lStructSize;
        if (value.structure_size >= offsetof(Ofn, lpTemplateName) + sizeof(input->lpTemplateName) &&
            value.structure_size <= sizeof(Ofn)) {
            value.complete = true; value.flags = input->Flags;
            value.hook = input->lpfnHook != nullptr; value.template_name = input->lpTemplateName != nullptr;
            value.custom_data = input->lCustData != 0;
        }
    }
    try { observe(value); } catch (...) {}
    SetLastError(entry_error);
    return original(input);
}
}

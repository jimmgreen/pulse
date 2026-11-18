#pragma once
#include <windows.h>
#include "../app/picker_appearance.h"

// Internal DLL boundary only; never exported or passed to a caller CRT.
namespace pulse::picker_inprocess {
struct Environment {
    HMODULE module = nullptr;
    HANDLE activation = INVALID_HANDLE_VALUE; // Borrowed until ReleaseEnvironment.
    LANGID ui_language = 0;
    pulse::app::PickerAppearanceSnapshot appearance;
};
HRESULT AcquireEnvironment(Environment& output);
HRESULT ReleaseEnvironment();
}

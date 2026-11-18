#pragma once
#include <windows.h>
#include <commdlg.h>
#include <shobjidl.h>
#include <functional>
namespace pulse::shell_bridge {
HRESULT CreateSystemLegacyDialog(bool save, IUnknown** output);
using LegacyDialogFactory = std::function<HRESULT(bool, IFileDialog**)>;
struct LegacyComDiagnostic {
    const char* reason = nullptr; // Fixed internal label, never caller text.
    DWORD structure_size = 0, flags = 0, flags_ex = 0, presence = 0;
    DWORD max_file = 0, max_title = 0, max_custom_filter = 0, filter_index = 0;
    HRESULT error = S_OK;
};
// Presence bits: file, file-title, filter, custom-filter, initial-dir, title,
// default-extension, hook, template, reserved-pointer, reserved-DWORD, custom-data.
using LegacyDiagnostic = std::function<void(const LegacyComDiagnostic&)>;
// False is a pre-display bypass. Once Show is called the operation is consumed.
bool RunLegacyComDialog(OPENFILENAMEW* input, bool save, const LegacyDialogFactory& create, BOOL& result, const LegacyDiagnostic& diagnostic = {});
bool RunLegacyComDialog(OPENFILENAMEA* input, bool save, const LegacyDialogFactory& create, BOOL& result, const LegacyDiagnostic& diagnostic = {});
}

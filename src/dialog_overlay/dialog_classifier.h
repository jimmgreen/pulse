#pragma once

#include "../common/dialog_bridge_state.h"
#include <windows.h>
#include <cstdint>
#include <string>

namespace pulse::dialog_overlay {

enum class NativeDialogKind : uint32_t {
    NotAFileDialog = 0,
    AlreadyTakenOverByPulse,
    VistaFileDialog,       // Modern #32770 hosting DUIViewWndClassName / DirectUIHWND
    LegacyFileDialog,      // Win32 GetOpenFileName/GetSaveFileName #32770 with SHELLDLL_DefView
    BrowseForFolderDialog, // SHBrowseForFolder #32770 with SysTreeView32
    BrokeredPickerHost,    // UWP / MSIX PickerHost.exe dialog
};

struct NativeDialogInfo {
    NativeDialogKind kind = NativeDialogKind::NotAFileDialog;
    HWND dialog_hwnd = nullptr;
    HWND filename_edit_hwnd = nullptr;
    HWND ok_button_hwnd = nullptr;
    HWND cancel_button_hwnd = nullptr;
    DWORD owner_pid = 0;
    bool has_custom_controls = false;
    bool mode_b_eligible = false; // Strictly true ONLY for pure standard dialogs with zero custom controls.
    std::wstring window_class;
    std::wstring title;
};

// Non-invasively inspects `hwnd` and its child control tree to determine whether it is
// a native Windows file dialog, whether Layer 1/2 already owns it (via `ledger`), and
// whether Mode B (zero-injection proxy) is safe or Mode A (companion bar) must be used.
NativeDialogInfo ClassifyNativeDialog(HWND hwnd,
                                      const dialog_bridge_state::ActiveDialogLedger* ledger = nullptr) noexcept;

// Drives a native #32770 file dialog by writing `path_or_folder` into its filename Edit
// control (id 1148 / 1152) and optionally clicking IDOK (1) to navigate or accept.
bool DriveNativeDialogNavigation(const NativeDialogInfo& info, const std::wstring& path_or_folder,
                                 bool click_ok = true) noexcept;

// Sends IDCANCEL (2) cleanly to a native #32770 dialog.
bool CancelNativeDialog(const NativeDialogInfo& info) noexcept;

} // namespace pulse::dialog_overlay

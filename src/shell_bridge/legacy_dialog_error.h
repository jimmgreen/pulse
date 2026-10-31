#pragma once
#include <windows.h>
#include <commdlg.h>

namespace pulse::shell_bridge::legacy_dialog_error {
// A/W explicit adapters share the last common-dialog outcome on this thread.
// Native error sources are never cached or queried until the caller asks.
void UseNative() noexcept;
void SetBridge(DWORD error) noexcept;
DWORD Query(decltype(&CommDlgExtendedError) native_error);
}

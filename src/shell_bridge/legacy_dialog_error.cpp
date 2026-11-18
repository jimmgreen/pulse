#include "legacy_dialog_error.h"

namespace pulse::shell_bridge::legacy_dialog_error {
namespace {
thread_local bool bridge_active = false;
thread_local DWORD bridge_error = 0;
}
void UseNative() noexcept { bridge_active = false; }
void SetBridge(DWORD error) noexcept {
    bridge_error = error;
    bridge_active = true;
}
DWORD Query(decltype(&CommDlgExtendedError) native_error) {
    return bridge_active ? bridge_error : native_error();
}
}

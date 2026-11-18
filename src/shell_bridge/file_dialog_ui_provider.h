#pragma once
#include "file_dialog_inprocess_session.h"
#include <string_view>

namespace pulse::shell_bridge {
// Controlled proof loader only. Derives PulsePickerUiProof.dll from the actual
// pulse.exe directory, never the caller EXE/CWD. Explicit absolute local drive
// path required; UNC, reparse ancestors and already loaded same-name modules are
// rejected. The caller should cache the returned provider outside loader lock.
// Default refuses before any file access/LoadLibrary. No runtime gate is enabled.
// module_lifetime retains guarded directory/file identities and the module until
// the last provider/session reference ends; a DLL's own process PIN is not undone.
// Must not be called or destroyed from DllMain. Output is reset on every failure.
HRESULT LoadFileDialogUiProvider(std::wstring_view absolute_pulse_executable,
    std::shared_ptr<const FileDialogInprocessProvider>& output,
    bool allow_unverified_proof = false) noexcept;
}

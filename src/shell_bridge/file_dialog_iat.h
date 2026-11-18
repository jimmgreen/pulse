#pragma once

#include "file_dialog_activation.h"
#include <cstddef>
#include <span>

namespace pulse::shell_bridge {
// Status belongs to this request; flags snapshot the adapter's global state,
// including on rejected requests and exceptions. installed records slot
// ownership last observed under the installation lock, not the requested module.
// Uncoordinated foreign writes are detected by the next slot comparison; these
// flags are not a continuous external-memory monitor. Concurrent operations may
// change the snapshot immediately after return.
struct FileDialogIatResult {
    HRESULT status = E_FAIL;
    bool installed = false;
    bool protection_restored = true;
    bool owner_pinned = false;
};

// Controlled x86/x64 experiment, exclusively the explicitly supplied main EXE.
// PE32/PE32+ parsing accepts only the compiling process architecture.
// Link this module into that EXE; no DLL targets, DllMain, loader callbacks,
// automatic module scanning, delay imports or GetProcAddress interception.
// Callers must serialize other writers to the same page's protection/IAT slot.
// Supplied services must own their captured state (not borrow stack references),
// and permit concurrent invocation/destruction on the calling COM apartments.
FileDialogIatResult InstallSelfFileDialogIat(HMODULE owned_main_exe,
    const FileDialogBridgeServices& services, bool enabled) noexcept;
// DLL-only explicit path: pins the actual dispatcher module before publishing
// its address to the main EXE IAT. Pinning cannot be reversed until process exit,
// including when the later slot write fails. Pre-PIN candidates are locally
// owned and freed on failure; queries and uninitialized uninstall allocate no
// state. Never call from DllMain.
FileDialogIatResult InstallPinnedModuleFileDialogIat(HMODULE owned_main_exe,
    const FileDialogBridgeServices& services, bool enabled) noexcept;
FileDialogIatResult QueryFileDialogIatState() noexcept;
FileDialogIatResult UninstallSelfFileDialogIat() noexcept;

// Uninstall disables new routing before restoring the slot; it is NOT a drain
// barrier. In-flight calls retain their immutable configuration. The dispatcher
// and captured original remain alive until process exit, including for threads
// that loaded the thunk address before uninstall. A later install can route such
// a delayed call using the new configuration. No executable memory is generated.
// On protection restoration failure, status fails and protection_restored=false;
// install attempts slot rollback. Uninstall can retry outstanding restoration.

namespace detail {
// Bounded parser of a loaded-image layout, also used by malformed-image tests.
// Returns exactly one aligned named CoCreateInstance static import slot RVA.
HRESULT InspectFileDialogIatImage(std::span<const std::byte> image, size_t* slot_rva) noexcept;
}
}

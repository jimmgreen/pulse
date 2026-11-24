#pragma once
#include "dialog_auto_transaction.h"
#include <string>
#include <vector>

namespace pulse::dialog_auto {
struct InstalledDialogDll {
    Architecture architecture = Architecture::X64;
    std::wstring dll_path;  // the installed root's current bundle DLL
};
struct RebindResult {
    unsigned rebound = 0;   // registrations moved to the current bundle
    unsigned skipped = 0;   // disabled, already current, foreign or conflicting
    DWORD error = ERROR_SUCCESS;
    DWORD rollback_error = ERROR_SUCCESS;
};
// The per-user registration stores an absolute bundle path. After an upgrade it
// still names the old version, and once that version is cleaned up every
// non-injected CoCreateInstance(FileOpenDialog) fails with 0x8007007E. Moves
// an enabled registration owned by this installed root (or orphaned because
// its DLL is gone) to the current bundle. Foreign, disabled and conflicting
// registrations are left untouched; enabled state is never turned on.
RebindResult RebindStaleRegistration(const std::wstring& install_root,
    const std::vector<InstalledDialogDll>& current, const RegistrationApi& api = {}) noexcept;
// DialogBridge digests that enabled registrations of either architecture name
// under install_root: COM loads those DLLs, so version cleanup must keep them.
std::vector<std::wstring> RegisteredBundleDigests(const std::wstring& install_root,
    const RegistrationApi& api = {}) noexcept;
}

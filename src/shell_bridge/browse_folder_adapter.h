#pragma once
#include <windows.h>
#include <shlobj.h>
#include <functional>
#include "legacy_com_provider.h"

namespace pulse::shell_bridge {
struct BrowseFolderDiagnostic {
    const char* reason = nullptr;
    UINT flags = 0, presence = 0;
    HRESULT error = S_OK;
};
using BrowseFolderObserver = std::function<void(const BrowseFolderDiagnostic&)>;
// False permits the original SHBrowseForFolder call before any dialog is shown.
// True consumes cancellation, acceptance and every post-Show failure. The caller
// owns a successful absolute PIDL and releases it with CoTaskMemFree.
bool RunBrowseFolderDialog(BROWSEINFOW* input, const LegacyDialogFactory& create,
    PIDLIST_ABSOLUTE& result, const BrowseFolderObserver& observer = {});
bool RunBrowseFolderDialog(BROWSEINFOA* input, const LegacyDialogFactory& create,
    PIDLIST_ABSOLUTE& result, const BrowseFolderObserver& observer = {});
}

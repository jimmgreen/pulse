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
struct BrowseFolderPolicy {
    BFFCALLBACK callback = nullptr;
    LPARAM callback_data = 0;
    bool restricted = false, hide_new_folder = false;
    bool* use_original = nullptr;
};
using BrowseFolderFactory = std::function<HRESULT(const BrowseFolderPolicy&, IFileDialog**)>;
// False permits the original SHBrowseForFolder call before any dialog is shown.
// True consumes cancellation, acceptance and every post-Show failure. The caller
// owns a successful absolute PIDL and releases it with CoTaskMemFree.
bool RunBrowseFolderDialog(BROWSEINFOW* input, const BrowseFolderFactory& create,
    PIDLIST_ABSOLUTE& result, const BrowseFolderObserver& observer = {});
bool RunBrowseFolderDialog(BROWSEINFOA* input, const BrowseFolderFactory& create,
    PIDLIST_ABSOLUTE& result, const BrowseFolderObserver& observer = {});
}

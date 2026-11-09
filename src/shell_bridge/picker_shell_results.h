#pragma once

#include "../ipc/picker_protocol.h"
#include <windows.h>
#include <shobjidl.h>
#include <string>
#include <vector>

namespace pulse::shell_bridge {

// Caller initializes COM on its thread. Results are real Shell objects with
// normal COM ownership. These calls may read filesystem/Shell state; never write.
// Save permits a nonexistent leaf in an existing directory, without creating it.
// Explicit allow_missing supports caller-validated Open or no-test-create Save
// with missing local parents via Shell bind metadata. Folder validation stays
// strict; default Save still requires an existing parent. No files are created.
// System GetDisplayName may choose an equivalent 8.3 path for long paths; callers
// must not treat a different spelling as a different file or rewrite Shell objects.
HRESULT CreatePickerShellItem(ipc::picker::Operation operation, const std::wstring& path,
                              IShellItem** item, bool allow_missing = false, uint32_t native_options = 0) noexcept;
HRESULT CreatePickerShellItemArray(ipc::picker::Operation operation,
                                   const std::vector<std::wstring>& paths,
                                   IShellItemArray** items, bool allow_missing = false, uint32_t native_options = 0) noexcept;

} // namespace pulse::shell_bridge

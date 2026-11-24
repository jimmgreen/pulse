#pragma once

#include "vfs_adapter.h"
#include <windows.h>

namespace pulse::extensions::native {

VfsBackend MakeVfsBackend(HWND owner);

// While set (UI thread), a provider's open_path request is stored in *target
// instead of navigating: MountArchiveQuietly. nullptr ends the capture.
void CaptureVfsOpens(std::wstring* target);

} // namespace pulse::extensions::native
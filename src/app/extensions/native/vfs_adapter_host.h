#pragma once

#include "vfs_adapter.h"
#include <windows.h>

namespace pulse::extensions::native {

VfsBackend MakeVfsBackend(HWND owner);

} // namespace pulse::extensions::native

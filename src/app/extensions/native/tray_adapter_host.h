#pragma once
#include "tray_adapter.h"

namespace pulse::extensions::native {

TrayBackend MakeTrayBackend(HWND owner);

} // namespace pulse::extensions::native

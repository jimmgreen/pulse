#pragma once
#include "dialogs_adapter.h"

namespace pulse::extensions::native {

DialogsBackend MakeDialogsBackend(HWND owner, bool dark, D2D1_COLOR_F accent);

} // namespace pulse::extensions::native

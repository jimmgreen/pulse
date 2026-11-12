#pragma once
#include "workbench_adapter.h"

namespace pulse::extensions::native {

WorkbenchBackend MakeWorkbenchBackend(HWND owner);
UINT ExtensionWorkbenchActionMessage() noexcept;
bool HandleWorkbenchActionMessage(HWND owner, UINT message, WPARAM wparam) noexcept;

} // namespace pulse::extensions::native

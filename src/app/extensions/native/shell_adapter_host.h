#pragma once
#include "shell_adapter.h"
#include <vector>

namespace pulse::ops { struct ShellMenuItem; }

namespace pulse::extensions::native {

ShellBackend MakeShellBackend(HWND owner);
bool DeliverShellAdapterMenuItems(uint32_t token, const std::vector<ops::ShellMenuItem>& items, bool partial);

} // namespace pulse::extensions::native

#pragma once
#include "pickers_adapter.h"

namespace pulse::extensions::native {

PickersBackend MakePickersBackend(HWND owner, std::function<HWND()> active_modal_owner);

} // namespace pulse::extensions::native

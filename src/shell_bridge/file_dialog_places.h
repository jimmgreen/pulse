#pragma once
#include "../picker_inprocess/picker_ui_api_v2.h"
#include <string>
#include <shobjidl.h>
static_assert(PULSE_PICKER_UI_V2_PLACE_TOP == FDAP_TOP);
static_assert(PULSE_PICKER_UI_V2_PLACE_BOTTOM == FDAP_BOTTOM);
namespace pulse::shell_bridge {
struct FileDialogPlace {
    std::wstring label, path;
    uint32_t location = 0;
};
}

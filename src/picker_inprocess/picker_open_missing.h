#pragma once
#include "picker_ui_api_v2.h"
#include <shobjidl.h>

namespace pulse::picker_inprocess {
inline constexpr bool MissingOpenOptionsSupported(uint32_t open, uint32_t native, bool folder, bool save) {
    const bool allow = (open & PULSE_PICKER_UI_V2_OPEN_ALLOW_MISSING) != 0;
    const bool validated = (open & PULSE_PICKER_UI_V2_OPEN_MISSING_VALIDATED) != 0;
    const bool skip = (open & PULSE_PICKER_UI_V2_OPEN_SKIP_ACCESS_VALIDATION) != 0;
    const bool create = (native & FOS_CREATEPROMPT) != 0;
    if (validated && (!allow || skip || create || folder || save)) return false;
    if (allow && (folder || save || (!skip && !create && !validated))) return false;
    return !(open & PULSE_PICKER_UI_V2_OPEN_REQUIRE_EXISTING_PARENT) || allow;
}
inline constexpr uint32_t RemotePickerFeatures(uint32_t features) {
    // The remote wire keeps its existing 0xff Open-options contract.
    return features & ~PULSE_PICKER_UI_V2_FEATURE_OPEN_MISSING_VALIDATED;
}
}

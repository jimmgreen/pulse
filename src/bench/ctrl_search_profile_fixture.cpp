#include "../app/app_state.h"
#include "../app/hot_entry_probe.h"
#include "../app/hot_entry_data_dir.h"

bool PrepareCtrlSearchProfile() {
    if (!GetEnvironmentVariableW(L"PULSE_TEST_LIVE_SHELL_ENTRY", nullptr, 0) ||
        !GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", nullptr, 0)) return false;
    auto state = std::make_unique<pulse::AppState>();
    return pulse::PrepareHotEntryProbe(*state) && !pulse::VerifiedHotEntryDataDirectory().empty();
}

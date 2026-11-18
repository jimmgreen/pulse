#include "command_contributions.h"
#include "../../app_state.h"
#include "../../toolbar_tools.h"
#include "../../../ui/toolbar_layout.h"
#include <algorithm>

namespace pulse::extensions::native {
bool IsCommandPinned(AppState& state, int command) {
    const auto id = CommandIdentity(state, command);
    return !id.empty() && std::find(state.appPrefs.toolbar_tools.begin(), state.appPrefs.toolbar_tools.end(), id) != state.appPrefs.toolbar_tools.end();
}

bool ToggleCommandPin(AppState& state, int command) {
    const auto id = CommandIdentity(state, command);
    if (!app::IsNativeToolbarTool(id)) return false;
    auto tools = state.appPrefs.toolbar_tools;
    const auto found = std::find(tools.begin(), tools.end(), id);
    if (found != tools.end()) tools.erase(found);
    else {
        if (app::ToolbarToolsOnly(tools).size() >= std::min(app::kMaxToolbarTools, static_cast<size_t>(ui::kToolbarCommandPins))) return false;
        tools.push_back(id);
    }
    state.appPrefs.toolbar_tools = app::NormalizeToolbarTools(tools);
    if (!state.isolatedTest) state.appPrefs.Save();
    InvalidateRect(state.hwnd, nullptr, FALSE);
    return true;
}
}

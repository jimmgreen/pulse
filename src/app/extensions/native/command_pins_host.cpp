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

void ApplyToolbarSuggestions(AppState& state, const std::vector<CommandInfo>& commands) {
    constexpr size_t kPerPackage = 2;
    auto& prefs = state.appPrefs;
    bool changed = false;
    for (const auto& command : commands) {
        if ((command.flags & PULSE_COMMAND_TOOLBAR) == 0 || (command.flags & PULSE_COMMAND_HIDDEN) != 0) continue;
        std::wstring id(command.identity.begin(), command.identity.end());   // ASCII "native:<pkg>:<cmd>"
        if (!app::IsNativeToolbarTool(id)) continue;
        if (std::find(prefs.toolbar_suggested.begin(), prefs.toolbar_suggested.end(), id) != prefs.toolbar_suggested.end()) continue;
        const std::wstring prefix = L"native:" + std::wstring(command.package.begin(), command.package.end()) + L":";
        const size_t used = static_cast<size_t>(std::count_if(prefs.toolbar_suggested.begin(), prefs.toolbar_suggested.end(),
            [&](const std::wstring& seen) { return seen.starts_with(prefix); }));
        if (used >= kPerPackage) continue;
        prefs.toolbar_suggested.push_back(id);
        changed = true;
        if (std::find(prefs.toolbar_tools.begin(), prefs.toolbar_tools.end(), id) != prefs.toolbar_tools.end()) continue;
        if (app::ToolbarToolsOnly(prefs.toolbar_tools).size() >= std::min(app::kMaxToolbarTools, static_cast<size_t>(ui::kToolbarCommandPins))) continue;
        auto tools = prefs.toolbar_tools;
        tools.push_back(id);
        prefs.toolbar_tools = app::NormalizeToolbarTools(tools);
    }
    if (!changed) return;
    if (!state.isolatedTest) prefs.Save();
    InvalidateRect(state.hwnd, nullptr, FALSE);
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
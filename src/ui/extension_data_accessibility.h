#pragma once
#include "../app/extensions/ui_tree.h"
#include <windows.h>
#include <oleacc.h>
#include <functional>
#include <optional>
#include <string>
#include <vector>
namespace pulse::ui {
struct DataAccessibleRow {
    std::string id;
    std::wstring name;
    RECT screen_bounds{};
    bool enabled = true, selected = false, focused = false, offscreen = false;
    bool has_children = false, expanded = true;
};
struct DataAccessibleSnapshot {
    std::wstring name;
    std::wstring default_action_name;
    extensions::UiNodeType type = extensions::UiNodeType::VirtualList;
    RECT screen_bounds{};
    bool enabled = true, focused = false;
    std::vector<DataAccessibleRow> rows;
};
struct DataAccessibleCallbacks {
    // Capture a weak owner and generation. Null means permanently disconnected.
    std::function<std::optional<DataAccessibleSnapshot>()> snapshot;
    std::function<bool(const std::string&)> select;
    // Must use the same host authorization path as keyboard/mouse activation.
    std::function<bool(const std::string&)> activate;
};
HRESULT CreateExtensionDataAccessible(HWND window, DataAccessibleCallbacks callbacks, IAccessible** result);
}

#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace pulse::json { class Value; }
namespace pulse::extensions {
enum class UiNodeType { Stack, Grid, Group, ScrollView, Text, Button, TextInput, Checkbox, Select, Progress, Divider, Tabs, Tab, List, Switch, Radio, Slider, Segmented, Badge, Tag, InfoBar, SplitButton, MenuButton, SidebarItem, Breadcrumb, EmptyState, VirtualList, Tree, DataTable, TrayCard, CommandSearch, SplitPane, SidebarSection, DriveItem, PaneHeader, FileRow, StagingTray, StagingItem, StatusBar, Tooltip, TitleBarButton, Count };
struct UiMenuItem {
    std::string id, text, command, glyph, shortcut;
    bool enabled = true, checked = false, separator = false;
    std::vector<UiMenuItem> children;

    bool operator==(const UiMenuItem&) const = default;
};
struct UiDataRow {
    std::string id, text, glyph, command;
    std::vector<std::string> cells;
    std::vector<UiDataRow> children;
    bool expanded = true, enabled = true;

    bool operator==(const UiDataRow&) const = default;
};
struct UiDataColumn { std::string id, text; int width = 160;
    bool operator==(const UiDataColumn&) const = default;
};
struct UiDataSource {
    std::string source = "context.directoryEntries";
    int page_size = 64;
    std::string filter_state, sort_state, status_state, error_state, progress_state;

    bool operator==(const UiDataSource&) const = default;
};
struct UiValidation {
    bool required = false;
    int min_length = 0, max_length = 1024;
    std::string message;

    bool operator==(const UiValidation&) const = default;
};
struct UiNode {
    UiNodeType type = UiNodeType::Stack;
    std::string id, text, command, binding, state;
    std::vector<UiNode> children;
    std::vector<std::string> options;
    bool checked = false, enabled = true, visible = true;
    int value = 0, columns = 1, gap = 12, min_height = 0;
    // DIP constraints; zero maximum is unbounded. Flex grows only in a direct stack parent.
    int min_width = 0, max_width = 0, max_height = 0, flex = 0;
    std::string button_kind = "standard";
    std::string placeholder;
    std::string input_kind = "text";  // TextInput: text / password
    std::string progress_kind = "bar";
    std::string progress_state = "normal";
    std::string glyph, keycap, detail, message, color, close_command;
    std::string enabled_binding, visible_binding;
    std::string text_role = "body", alignment = "left";
    std::string badge_kind = "neutral", info_kind = "informational";
    std::string badge_text;
    bool icon_only = false, bordered = true, selected = false, current = false, separator = true;
    int stops = 2;
    std::string trailing_glyph, summary, filter_text, filter_command, sort_command;
    std::string modified, file_type, file_size, status_text, helper, count_label, action_text;
    std::string center_text, right_text, title_bar_role = "custom";
    bool dot = false, move_intent = false, expanded = true, cut = false, missing = false, ready = true;
    int item_count = 0, capacity = 0;
    int64_t free_bytes = -1, total_bytes = 0;
    std::vector<std::string> tag_colors;
    std::vector<UiMenuItem> menu, actions;
    std::vector<UiDataRow> rows;
    std::vector<UiDataColumn> data_columns;
    std::optional<UiDataSource> data_source;
    std::optional<UiValidation> validation;
    std::string orientation = "vertical";

    bool operator==(const UiNode&) const = default;
};
struct UiStateEntry { std::string key, value;
    bool operator==(const UiStateEntry&) const = default;
};
struct UiDataOperation { std::string target, operation;
    bool operator==(const UiDataOperation&) const = default;
};
struct UiStateAction { std::string id; std::vector<UiStateEntry> updates; std::optional<UiDataOperation> data_operation; std::vector<std::string> validate;
    bool operator==(const UiStateAction&) const = default;
};
struct UiTree { UiNode root; std::vector<UiStateEntry> state; std::vector<UiStateAction> actions; int version = 1;
    bool operator==(const UiTree&) const = default;
};
bool ValidateUiInput(const UiNode& node, std::string_view value);
bool ValidateUiStateValue(const UiTree& tree, std::string_view key, std::string_view value);
std::string_view UiNodeTypeName(UiNodeType type);
bool ParseUiTree(const json::Value& value, const std::vector<std::string>& commands, UiTree& tree, std::string& error);
}

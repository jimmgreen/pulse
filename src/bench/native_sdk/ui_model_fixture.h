#pragma once
#include "../../app/extensions/native/ui_model.h"
#include <array>
#include <cstring>
namespace native_ui_fixture {
// Independent frozen v1 expectations: never derive these from the private UiNodeType order.
inline constexpr const char *kExpectedTypeNames[] = {"stack", "grid", "group", "scrollView", "text", "button", "textInput", "checkbox", "select", "progress", "divider", "tabs", "tab", "list", "switch", "radio", "slider", "segmented", "badge", "tag", "infoBar", "splitButton", "menuButton", "sidebarItem", "breadcrumb", "emptyState", "virtualList", "tree", "dataTable", "", "commandSearch", "splitPane", "sidebarSection", "", "", "fileRow", "", "", "statusBar", "", "", "canvas", "optionCard", "settingsRow", "image", "menuPreview"};
static_assert(sizeof(kExpectedTypeNames) / sizeof(kExpectedTypeNames[0]) == PULSE_UI_MODEL_COUNT);
inline constexpr std::array<uint32_t, 39> kSupportedTypes{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 30, 31, 32, 35, 38, 41, 42, 43, 44, 45};
// Host-shell controls that stay internal: trayCard, driveItem, paneHeader, stagingTray, stagingItem, tooltip, titleBarButton.
inline constexpr std::array<uint32_t, 7> kRemovedTypes{29, 33, 34, 36, 37, 39, 40};
inline uint32_t NextType(uint32_t type) {
    for (size_t i = 0; i < kSupportedTypes.size(); ++i)
        if (kSupportedTypes[i] == type) return kSupportedTypes[(i + 1) % kSupportedTypes.size()];
    return kSupportedTypes.front();
}
inline PulseStringView S(const char *text) { return {text, static_cast<uint32_t>(std::strlen(text))}; }
inline PulseUiModelString T(const char *text) { return {1, S(text)}; }
inline PulseUiModelNode Node(uint32_t type, const char *id) {
    PulseUiModelNode node{};
    node.size = sizeof(node);
    node.type = type;
    node.id = S(id);
    return node;
}
// Owned borrowed-data fixture shared by model and actual renderer tests.
struct Model {
    PulseUiModelNode root{}, wrapper{}, inline_switch{};
    std::array<PulseUiModelNode, 2> children{};
    std::array<PulseStringView, 3> options{S("First"), S("Second"), S("Third")};
    std::array<PulseStringView, 2> canvas_ops{S("viewbox 120 64"), S("rect 4 4 112 56 6 accent none 1.5")};
    std::array<PulseStringView, 6> commands{S("run"), S("menu"), S("close"), S("filter"), S("sort"), S("row")};
    std::array<PulseUiState, 5> states{{{S("text"), S("Native input")},
                                        {S("flag"), S("true")},
                                        {S("index"), S("0")},
                                        {S("ratio"), S("50")},
                                        {S("row"), S("first")}}};
    PulseUiModelMenuItem menu{}, preview_item{};
    std::array<PulseUiModelRow, 2> rows{};
    std::array<PulseStringView, 2> cells{S("First native row"), S("Second native row")};
    PulseUiModelColumn column{};
    PulseUiModelTree tree{};
    explicit Model(uint32_t type) {
        root = Node(type, "sample");
        children = {Node(PULSE_UI_MODEL_TEXT, "left"), Node(PULSE_UI_MODEL_TEXT, "right")};
        children[0].text = T("Existing Pulse controls, created from C++");
        children[1].text = T("Second pane content");
        inline_switch = Node(PULSE_UI_MODEL_SWITCH, "inline-switch");
        inline_switch.state = T("flag");
        const bool container = type == PULSE_UI_MODEL_STACK || type == PULSE_UI_MODEL_GRID ||
                               type == PULSE_UI_MODEL_GROUP || type == PULSE_UI_MODEL_SCROLL_VIEW ||
                               type == PULSE_UI_MODEL_TABS || type == PULSE_UI_MODEL_TAB ||
                               type == PULSE_UI_MODEL_SPLIT_PANE || type == PULSE_UI_MODEL_SETTINGS_ROW ||
                               type == PULSE_UI_MODEL_SIDEBAR_SECTION;
        if ((!container && type != PULSE_UI_MODEL_DIVIDER) || type == PULSE_UI_MODEL_GROUP ||
            type == PULSE_UI_MODEL_TAB || type == PULSE_UI_MODEL_SETTINGS_ROW ||
            type == PULSE_UI_MODEL_SIDEBAR_SECTION)
            root.text = T("Native model sample");
        if (container)
            root.children = {1, children.data(), 1};
        if (type == PULSE_UI_MODEL_GRID)
            root.columns = {1, 2};
        if (type == PULSE_UI_MODEL_BUTTON || type == PULSE_UI_MODEL_SPLIT_BUTTON)
            root.command = T("run");
        if (type == PULSE_UI_MODEL_BUTTON)
            root.kind = T("primary");
        if (type == PULSE_UI_MODEL_TEXT_INPUT) {
            root.state = T("text");
            root.placeholder = T("Type a value");
        }
        if (type == PULSE_UI_MODEL_CHECKBOX || type == PULSE_UI_MODEL_SWITCH || type == PULSE_UI_MODEL_RADIO)
            root.state = T("flag");
        if (type == PULSE_UI_MODEL_SELECT || type == PULSE_UI_MODEL_SEGMENTED || type == PULSE_UI_MODEL_LIST) {
            root.options = {1, options.data(), 3};
            root.state = T("index");
        }
        if (type == PULSE_UI_MODEL_SLIDER) {
            root.state = T("index");
            root.stops = {1, 4};
        }
        if (type == PULSE_UI_MODEL_PROGRESS) {
            root.value = {1, 75};
            root.kind = T("bar");
        }
        if (type == PULSE_UI_MODEL_BADGE) {
            root.kind = T("success");
            root.dot = {1, 1};
        }
        if (type == PULSE_UI_MODEL_TAG)
            root.color = T("#4488CC");
        if (type == PULSE_UI_MODEL_INFO_BAR || type == PULSE_UI_MODEL_EMPTY_STATE)
            root.message = T("Native typed model uses the existing Fluent painter.");
        if (type == PULSE_UI_MODEL_INFO_BAR) {
            root.close_command = T("close");
            root.kind = T("informational");
        }
        menu.size = sizeof(menu);
        menu.id = T("entry");
        menu.text = T("Native menu callback");
        menu.command = T("menu");
        if (type == PULSE_UI_MODEL_MENU_BUTTON || type == PULSE_UI_MODEL_SPLIT_BUTTON)
            root.menu = {1, &menu, 1};
        if (type == PULSE_UI_MODEL_TABS) {
            children[0] = Node(PULSE_UI_MODEL_TAB, "tab");
            children[0].text = T("Native tab");
            children[0].close_command = T("close");
            children[0].children = {1, &children[1], 1};
            root.state = T("index");
        }
        if (type == PULSE_UI_MODEL_SPLIT_PANE) {
            root.children.count = 2;
            root.state = T("ratio");
        }
        if (type == PULSE_UI_MODEL_CANVAS) {
            root.options = {1, canvas_ops.data(), static_cast<uint32_t>(canvas_ops.size())};
            root.min_height = {1, 64};
            root.bordered = {1, 1};
        }
        if (type == PULSE_UI_MODEL_OPTION_CARD) {
            root.detail = T("Card explanation line");
            root.state = T("flag");
            root.kind = T("radio");
            root.glyph = T("icon:box");
            root.bordered = {1, 1};
        }
        if (type == PULSE_UI_MODEL_SETTINGS_ROW) {
            root.detail = T("Settings row helper description");
            root.bordered = {1, 1};
            root.children = {1, &inline_switch, 1};
        }
        if (type == PULSE_UI_MODEL_IMAGE) {
            root.glyph = T("icon:sparkle");
            root.kind = T("tile");
            root.color = T("#3377AA");
            root.min_height = {1, 64};
        }
        if (type == PULSE_UI_MODEL_SIDEBAR_ITEM) {
            root.text = T("Downloads");
            root.glyph = T("icon:download");
            root.detail = T("12 items");
            root.badge_text = T("3");
            root.selected = {1, 1};
            root.command = T("run");
        }
        if (type == PULSE_UI_MODEL_BREADCRUMB) {
            root.text = T("Projects");
            root.current = {1, 1};
            root.command = T("run");
        }
        if (type == PULSE_UI_MODEL_COMMAND_SEARCH) {
            root.text = T("Search commands");
            root.placeholder = T("Type to filter");
            root.keycap = T("Ctrl+K");
            root.state = T("text");
        }
        if (type == PULSE_UI_MODEL_SIDEBAR_SECTION) {
            root.glyph = T("icon:folder");
            root.state = T("flag");
        }
        if (type == PULSE_UI_MODEL_FILE_ROW) {
            root.text = T("report-2026.xlsx");
            root.glyph = T("icon:file");
            root.modified = T("2026-11-20 14:05");
            root.file_type = T("Spreadsheet");
            root.file_size = T("48 KB");
            root.command = T("run");
        }
        if (type == PULSE_UI_MODEL_STATUS_BAR) {
            root.text = T("128 items");
            root.center_text = T("3 selected");
            root.right_text = T("Ready");
            root.ready = {1, 1};
        }
        preview_item.size = sizeof(preview_item);
        preview_item.id = T("preview-item");
        preview_item.text = T("Smart extract");
        preview_item.glyph = T("icon:extract");
        preview_item.shortcut = T("Ctrl+E");
        if (type == PULSE_UI_MODEL_MENU_PREVIEW) {
            root.detail = T("Context menu preview");
            root.bordered = {1, 1};
            root.menu = {1, &preview_item, 1};
        }






        for (size_t i = 0; i < rows.size(); ++i) {
            rows[i].size = sizeof(rows[i]);
            rows[i].id = S(i ? "second" : "first");
            rows[i].text = {1, cells[i]};
            rows[i].command = T("row");
        }
        column.size = sizeof(column);
        column.id = S("name");
        column.text = S("Name");
        column.width = {1, 300};
        if (type == PULSE_UI_MODEL_VIRTUAL_LIST || type == PULSE_UI_MODEL_TREE || type == PULSE_UI_MODEL_DATA_TABLE) {
            root.rows = {1, rows.data(), 2};
            root.state = T("row");
        }
        if (type == PULSE_UI_MODEL_DATA_TABLE) {
            root.data_columns = {1, &column, 1};
            for (size_t i = 0; i < rows.size(); ++i)
                rows[i].cells = {1, &cells[i], 1};
        }
        tree.size = sizeof(tree);
        tree.root = &root;
        tree.states = states.data();
        tree.state_count = static_cast<uint32_t>(states.size());
        tree.commands = commands.data();
        tree.command_count = static_cast<uint32_t>(commands.size());
        if (type == PULSE_UI_MODEL_TAB) {
            wrapper = Node(PULSE_UI_MODEL_TABS, "tabs");
            wrapper.children = {1, &root, 1};
            tree.root = &wrapper;
        }
    }
    Model(const Model &) = delete;
    Model &operator=(const Model &) = delete;
};
} // namespace native_ui_fixture

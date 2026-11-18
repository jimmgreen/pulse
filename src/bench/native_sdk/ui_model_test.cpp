#include "../../app/extensions/native/ui_model.h"
#include "ui_model_fixture.h"
#include <array>
#include <cstdio>
#include <cstring>

namespace {
PulseStringView S(const char *value) { return {value, static_cast<uint32_t>(std::strlen(value))}; }
PulseUiModelString T(const char *value) { return {1, S(value)}; }
PulseUiModelNode Node(uint32_t type, const char *id) {
    PulseUiModelNode node{};
    node.size = sizeof(node);
    node.type = type;
    node.id = S(id);
    return node;
}
} // namespace
int main() {
    using namespace pulse::extensions;
    int passed = 0, failed = 0;
    auto check = [&](bool ok, const char *name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        ok ? ++passed : ++failed;
    };
    auto go = S("go");
    UiTree output;
    std::set<std::string> commands;
    std::string error;
    for (const uint32_t type : native_ui_fixture::kSupportedTypes) {
        auto root = Node(type, "root");
        auto child = Node(PULSE_UI_MODEL_TAB, "tab");
        child.text = T("Tab");
        std::array<PulseUiModelNode, 2> pair{Node(PULSE_UI_MODEL_TEXT, "a"), Node(PULSE_UI_MODEL_TEXT, "b")};
        auto wrapper = Node(PULSE_UI_MODEL_TABS, "tabs");
        const bool container = type == PULSE_UI_MODEL_STACK || type == PULSE_UI_MODEL_GRID ||
                               type == PULSE_UI_MODEL_GROUP || type == PULSE_UI_MODEL_SCROLL_VIEW ||
                               type == PULSE_UI_MODEL_TABS || type == PULSE_UI_MODEL_TAB ||
                               type == PULSE_UI_MODEL_SPLIT_PANE;
        if ((!container && type != PULSE_UI_MODEL_DIVIDER) || type == PULSE_UI_MODEL_GROUP ||
            type == PULSE_UI_MODEL_TAB)
            root.text = T("Example");
        if (type == PULSE_UI_MODEL_BUTTON || type == PULSE_UI_MODEL_SPLIT_BUTTON)
            root.command = T("go");
        auto option = S("Option");
        if (type == PULSE_UI_MODEL_SELECT || type == PULSE_UI_MODEL_SEGMENTED)
            root.options = {1, &option, 1};
        PulseUiModelMenuItem menu{};
        menu.size = sizeof(menu);
        menu.id = T("entry");
        menu.text = T("Run");
        menu.command = T("go");
        if (type == PULSE_UI_MODEL_MENU_BUTTON || type == PULSE_UI_MODEL_SPLIT_BUTTON)
            root.menu = {1, &menu, 1};
        if (type == PULSE_UI_MODEL_TABS)
            root.children = {1, &child, 1};
        if (type == PULSE_UI_MODEL_SPLIT_PANE)
            root.children = {1, pair.data(), 2};
        if (type == PULSE_UI_MODEL_VIRTUAL_LIST || type == PULSE_UI_MODEL_TREE || type == PULSE_UI_MODEL_DATA_TABLE)
            root.rows = {1, nullptr, 0};
        PulseUiModelColumn column{};
        column.size = sizeof(column);
        column.id = S("name");
        column.text = S("Name");
        if (type == PULSE_UI_MODEL_DATA_TABLE)
            root.data_columns = {1, &column, 1};
        PulseUiModelTree model{};
        model.size = sizeof(model);
        model.root = &root;
        model.commands = &go;
        model.command_count = 1;
        if (type == PULSE_UI_MODEL_TAB) {
            wrapper.children = {1, &root, 1};
            model.root = &wrapper;
        }
        const auto status = native::ConvertUiModel(&model, output, commands, error);
        const bool mapped =
            status == PULSE_OK &&
            UiNodeTypeName(type == PULSE_UI_MODEL_TAB ? output.root.children.front().type : output.root.type) ==
                native_ui_fixture::kExpectedTypeNames[type];
        const auto label = std::string("existing model mapping: ") + native_ui_fixture::kExpectedTypeNames[type];
        check(mapped, label.c_str());
        if (!mapped)
            std::printf("  %s\n", error.c_str());
    }
    for (const uint32_t type : native_ui_fixture::kSupportedTypes) {
        native_ui_fixture::Model fixture(type);
        const auto result = native::ConvertUiModel(&fixture.tree, output, commands, error);
        const auto label =
            std::string("populated renderer fixture: ") + std::string(native_ui_fixture::kExpectedTypeNames[type]);
        check(result == PULSE_OK, label.c_str());
        if (result != PULSE_OK)
            std::printf("  %s\n", error.c_str());
    }
    auto root = Node(PULSE_UI_MODEL_TEXT, "root");
    root.text = T("Copied");
    PulseUiModelTree model{};
    model.size = sizeof(model);
    model.root = &root;
    check(native::ConvertUiModel(&model, output, commands, error) == PULSE_OK, "baseline typed model");
    const auto retained = output;
    const auto retained_commands = commands;
    for (const auto type : native_ui_fixture::kRemovedTypes) {
        root.type = type;
        const auto label = "removed public model value rejected: " + std::to_string(type);
        check(native::ConvertUiModel(&model, output, commands, error) == PULSE_ERROR_ARGUMENT &&
              output == retained && commands == retained_commands, label.c_str());
    }
    root.type = PULSE_UI_MODEL_TEXT;
    root.placeholder = T("unsupported on text");
    check(native::ConvertUiModel(&model, output, commands, error) == PULSE_ERROR_ARGUMENT && output == retained,
          "node-specific field rejection is transactional");
    root.placeholder = {};
    const char bad[] = {'\xC0', '\x80'};
    root.text = {1, {bad, 2}};
    check(native::ConvertUiModel(&model, output, commands, error) == PULSE_ERROR_ARGUMENT,
          "invalid UTF-8 rejected before DOM");
    root.text = T("Copied");
    root.binding = T("context.directory");
    check(native::ConvertUiModel(&model, output, commands, error) == PULSE_ERROR_UNAVAILABLE,
          "unconnected context resolver is unavailable");
    root.binding = {};
    PulseUiModelDataSource source{};
    root.data_source = &source;
    check(native::ConvertUiModel(&model, output, commands, error) == PULSE_ERROR_UNAVAILABLE,
          "unconnected provider is unavailable");
    root = Node(PULSE_UI_MODEL_STACK, "root");
    root.children = {1, &root, 1};
    check(native::ConvertUiModel(&model, output, commands, error) == PULSE_ERROR_ARGUMENT,
          "node pointer cycle rejected");
    root = Node(PULSE_UI_MODEL_MENU_BUTTON, "root");
    PulseUiModelMenuItem menu{};
    menu.size = sizeof(menu);
    menu.id = T("menu");
    menu.text = T("Menu");
    menu.children = {1, &menu, 1};
    root.menu = {1, &menu, 1};
    check(native::ConvertUiModel(&model, output, commands, error) == PULSE_ERROR_ARGUMENT,
          "menu pointer cycle rejected");
    root = Node(PULSE_UI_MODEL_TREE, "root");
    PulseUiModelRow row{};
    row.size = sizeof(row);
    row.id = S("row");
    row.children = {1, &row, 1};
    root.rows = {1, &row, 1};
    check(native::ConvertUiModel(&model, output, commands, error) == PULSE_ERROR_ARGUMENT,
          "row pointer cycle rejected");
    root = Node(PULSE_UI_MODEL_STACK, "root");
    std::array<PulseUiModelNode, 128> many{};
    std::array<std::string, 128> ids{};
    for (size_t i = 0; i < many.size(); ++i) {
        ids[i] = "node" + std::to_string(i);
        many[i] = Node(PULSE_UI_MODEL_TEXT, ids[i].c_str());
    }
    root.children = {1, many.data(), 127};
    check(native::ConvertUiModel(&model, output, commands, error) == PULSE_OK, "exact 128 nodes accepted");
    root.children.count = 128;
    check(native::ConvertUiModel(&model, output, commands, error) == PULSE_ERROR_ARGUMENT, "129 nodes rejected");
    std::array<PulseUiModelNode, 9> chain{};
    for (size_t i = 0; i < chain.size(); ++i) {
        chain[i] = Node(PULSE_UI_MODEL_STACK, ids[i].c_str());
        if (i + 1 < chain.size())
            chain[i].children = {1, &chain[i + 1], 1};
    }
    chain[7].children = {};
    model.root = chain.data();
    check(native::ConvertUiModel(&model, output, commands, error) == PULSE_OK, "exact depth eight accepted");
    chain[7].children = {1, &chain[8], 1};
    check(native::ConvertUiModel(&model, output, commands, error) == PULSE_ERROR_ARGUMENT, "depth nine rejected");
    model.root = &root;
    root = Node(PULSE_UI_MODEL_TEXT_INPUT, "root");
    root.text = T("Name");
    root.state = T("name");
    PulseUiState state{S("name"), S("initial")};
    model.states = &state;
    model.state_count = 1;
    PulseUiModelValidation validation{};
    validation.size = sizeof(validation);
    validation.required = {1, 1};
    validation.min_length = {1, 2};
    validation.max_length = {1, 20};
    validation.message = S("Enter name");
    root.validation = &validation;
    PulseUiState change{S("name"), S("changed")};
    auto validate = S("root");
    PulseUiModelAction action{};
    action.size = sizeof(action);
    action.id = S("save");
    action.updates = &change;
    action.update_count = 1;
    action.validate = {1, &validate, 1};
    model.actions = &action;
    model.action_count = 1;
    check(native::ConvertUiModel(&model, output, commands, error) == PULSE_OK && output.root.validation &&
              output.root.validation->min_length == 2 && output.actions[0].validate[0] == "root" &&
              output.actions[0].updates[0].value == "changed",
          "validation and local state action fields copied");
    PulseUiModelDataOperation operation{};
    action.data_operation = &operation;
    check(native::ConvertUiModel(&model, output, commands, error) == PULSE_ERROR_UNAVAILABLE,
          "unconnected dataOperation rejected");
    std::printf("%d PASS, %d FAIL\n", passed, failed);
    return failed ? 1 : 0;
}

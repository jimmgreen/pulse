#include "../../app/extensions/native/ui_model.h"
#include "ui_model_fixture.h"
#include "../../../sdk/native/include/pulse/model_builder.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

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
                               type == PULSE_UI_MODEL_SPLIT_PANE || type == PULSE_UI_MODEL_SETTINGS_ROW;
        if ((!container && type != PULSE_UI_MODEL_DIVIDER) || type == PULSE_UI_MODEL_GROUP ||
            type == PULSE_UI_MODEL_TAB || type == PULSE_UI_MODEL_SETTINGS_ROW)
            root.text = T("Example");
        if (type == PULSE_UI_MODEL_BUTTON || type == PULSE_UI_MODEL_SPLIT_BUTTON)
            root.command = T("go");
        auto option = S("Option");
        if (type == PULSE_UI_MODEL_SELECT || type == PULSE_UI_MODEL_SEGMENTED || type == PULSE_UI_MODEL_LIST)
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
    std::array<PulseUiModelNode, 384> many{};
    std::array<std::string, 384> ids{};
    for (size_t i = 0; i < many.size(); ++i) {
        ids[i] = "node" + std::to_string(i);
        many[i] = Node(PULSE_UI_MODEL_TEXT, ids[i].c_str());
    }
    root.children = {1, many.data(), 383};
    check(native::ConvertUiModel(&model, output, commands, error) == PULSE_OK, "exact 384 nodes accepted");
    root.children.count = 384;
    check(native::ConvertUiModel(&model, output, commands, error) == PULSE_ERROR_ARGUMENT, "385 nodes rejected");
    std::array<PulseUiModelNode, 13> chain{};
    for (size_t i = 0; i < chain.size(); ++i) {
        chain[i] = Node(PULSE_UI_MODEL_STACK, ids[i].c_str());
        if (i + 1 < chain.size())
            chain[i].children = {1, &chain[i + 1], 1};
    }
    chain[11].children = {};
    model.root = chain.data();
    check(native::ConvertUiModel(&model, output, commands, error) == PULSE_OK, "exact depth twelve accepted");
    chain[11].children = {1, &chain[12], 1};
    check(native::ConvertUiModel(&model, output, commands, error) == PULSE_ERROR_ARGUMENT, "depth thirteen rejected");
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
    {
        auto slider=Node(PULSE_UI_MODEL_SLIDER,"continuous");
        slider.kind=T("continuous");slider.state=T("amount");slider.command=T("commit");
        PulseStringView range[]={S("-1.5"),S("4.75")};slider.options={1,range,2};
        PulseUiState initial{S("amount"),S("1.23456789")};auto commit=S("commit");
        PulseUiModelTree tree{};tree.size=sizeof(tree);tree.root=&slider;tree.states=&initial;tree.state_count=1;tree.commands=&commit;tree.command_count=1;
        check(native::ConvertUiModel(&tree,output,commands,error)==PULSE_OK&&output.root.slider_kind=="continuous","continuous kind preserves ABI and real fractional state");
        for(const char* invalid:{"nan","inf","1e999","5","1.2x"}){initial.value=S(invalid);check(native::ConvertUiModel(&tree,output,commands,error)!=PULSE_OK,"continuous invalid numeric state rejected");}
        initial.value=S("1.2");range[1]=S("-2");check(native::ConvertUiModel(&tree,output,commands,error)!=PULSE_OK,"continuous reversed bounds rejected");range[1]=S("4.75");
        slider.stops={1,5};check(native::ConvertUiModel(&tree,output,commands,error)!=PULSE_OK,"continuous cannot masquerade as stop index");slider.stops={};
        slider.kind={};slider.command={};slider.options={};initial.value=S("1");
        check(native::ConvertUiModel(&tree,output,commands,error)==PULSE_OK&&output.root.slider_kind=="stepped","old no-kind slider retains discrete default");
        std::vector<std::string> keys;std::vector<PulseUiState> states;keys.reserve(129);states.reserve(129);
        for(int i=0;i<129;++i)keys.push_back("s"+std::to_string(i));
        for(const auto& key:keys)states.push_back({{key.data(),static_cast<uint32_t>(key.size())},S("0")});
        slider.state={};tree.states=states.data();tree.state_count=128;
        check(native::ConvertUiModel(&tree,output,commands,error)==PULSE_OK,"128 initial states accepted");tree.state_count=129;
        check(native::ConvertUiModel(&tree,output,commands,error)!=PULSE_OK,"129 initial states rejected");
    }
    {
        using pulse::sdk::ModelMenuItem;
        using pulse::sdk::ModelNode;
        using pulse::sdk::ModelTree;
        using N = PulseUiModelNode;
        std::string large(4096, 'x');
        std::string too_large(4097, 'x');
        auto ok_tree = ModelTree(
            ModelNode(PULSE_UI_MODEL_STACK, "settings-root")
                .Children({
                    ModelNode::OptionCard("opt-a", "Smart extract", "Single root extracted in-place", "flag", "radio", "icon:extract", "Default"),
                    ModelNode::OptionCard("opt-b", "Folder extract", "Always create folder", "flag2", "toggle"),
                    ModelNode::SettingsRow("row-a", "Context preview", "Dynamic bound detail",
                        ModelNode(PULSE_UI_MODEL_SWITCH, "row-sw").String(&N::state, "flag"), "icon:settings", true)
                        .String(&N::binding, "state.blob"),
                    ModelNode::Image("img-a", "icon:box", "7z tile", "tile", 72).String(&N::color, "#3A7A68"),
                    ModelNode::MenuPreview("menu-a", "Context Menu", {
                        ModelMenuItem("m1", "Extract").Glyph("icon:extract").Shortcut("Ctrl+E"),
                        ModelMenuItem::Separator("msep"),
                        ModelMenuItem("m2", "Submenu").Children({
                            ModelMenuItem("m2-1", "Test archive").Checked(true)
                        })
                    }, "Live menu layout"),
                    ModelNode::Chip("chip-z", ".7z", "flag", "#228855", "icon:box"),
                    ModelNode(PULSE_UI_MODEL_GRID, "grid-8").Int(&N::columns, 8)
                }))
            .State("flag", "true")
            .State("flag2", "false")
            .State("blob", large);
        check(native::ConvertUiModel(&ok_tree.Build(), output, commands, error) == PULSE_OK &&
              output.root.children.size() == 7 &&
              output.root.children[0].type == UiNodeType::OptionCard &&
              output.root.children[2].type == UiNodeType::SettingsRow &&
              output.root.children[3].type == UiNodeType::Image &&
              output.root.children[4].type == UiNodeType::MenuPreview &&
              output.root.children[4].menu.size() == 3 &&
              output.root.children[4].menu[1].separator &&
              output.root.children[5].type == UiNodeType::Tag &&
              output.root.children[6].columns == 8,
              "new P0-2 controls, builders, 8-column grid, and 4096-byte state convert cleanly");

        auto bad_state = ModelTree(ModelNode(PULSE_UI_MODEL_TEXT, "t").String(&N::binding, "state.blob")).State("blob", too_large);
        check(native::ConvertUiModel(&bad_state.Build(), output, commands, error) == PULSE_ERROR_ARGUMENT,
              "4097-byte state value rejected");

        auto bad_card_kind = ModelTree(ModelNode(PULSE_UI_MODEL_OPTION_CARD, "c").String(&N::state, "flag").String(&N::kind, "bogus"))
            .State("flag", "true");
        check(native::ConvertUiModel(&bad_card_kind.Build(), output, commands, error) == PULSE_ERROR_ARGUMENT,
              "optionCard invalid kind rejected");

        auto bad_card_state = ModelTree(ModelNode(PULSE_UI_MODEL_OPTION_CARD, "c").String(&N::state, "idx"))
            .State("idx", "0");
        check(native::ConvertUiModel(&bad_card_state.Build(), output, commands, error) == PULSE_ERROR_ARGUMENT,
              "optionCard requires boolean state");

        auto bad_chip_state = ModelTree(ModelNode(PULSE_UI_MODEL_TAG, "chip").String(&N::text, "zip").String(&N::state, "idx"))
            .State("idx", "1");
        check(native::ConvertUiModel(&bad_chip_state.Build(), output, commands, error) == PULSE_ERROR_ARGUMENT,
              "interactive chip/tag requires boolean state");

        auto bad_fit = ModelTree(ModelNode::Image("img", "icon:box", "alt", "stretch"));
        check(native::ConvertUiModel(&bad_fit.Build(), output, commands, error) == PULSE_ERROR_ARGUMENT,
              "image invalid fit rejected");

        auto bad_row_children = ModelTree(ModelNode(PULSE_UI_MODEL_SETTINGS_ROW, "r").Children({
            ModelNode(PULSE_UI_MODEL_TEXT, "c1").String(&N::text, "1"),
            ModelNode(PULSE_UI_MODEL_TEXT, "c2").String(&N::text, "2"),
            ModelNode(PULSE_UI_MODEL_TEXT, "c3").String(&N::text, "3"),
            ModelNode(PULSE_UI_MODEL_TEXT, "c4").String(&N::text, "4"),
            ModelNode(PULSE_UI_MODEL_TEXT, "c5").String(&N::text, "5")
        }));
        check(native::ConvertUiModel(&bad_row_children.Build(), output, commands, error) == PULSE_ERROR_ARGUMENT,
              "settingsRow >4 inline children rejected");

        auto bad_grid_9 = ModelTree(ModelNode(PULSE_UI_MODEL_GRID, "g9").Int(&N::columns, 9));
        check(native::ConvertUiModel(&bad_grid_9.Build(), output, commands, error) == PULSE_ERROR_ARGUMENT,
              "grid >8 columns rejected");
    }
    {
        // Presentation extras, typography and the re-opened general-purpose controls.
        using pulse::sdk::CanvasBuilder;
        using pulse::sdk::ModelMenuItem;
        using pulse::sdk::ModelNode;
        using pulse::sdk::ModelTree;
        auto rich = ModelTree(
            ModelNode::VStack("root", {
                ModelNode::Card("card", "Overview", {
                    ModelNode::Label("title", "Archive health", "title"),
                    ModelNode::Label("ok", "All volumes verified", "bodyStrong", "success"),
                    ModelNode::Label("hash", "sha256 3f1a", "code", "secondary"),
                    ModelNode::Button("go", "Verify", "go", "primary").Tooltip("Re-run verification"),
                }).Padding(20),
                ModelNode::HStack("crumbs", {ModelNode::Crumb("c1", "Home", "go"), ModelNode::Crumb("c2", "Docs", "go", true)})
                    .Padding(0),
                ModelNode::Section("sec", "Recent", "open", {
                    ModelNode::NavItem("nav", "Downloads", "go", "icon:download", true, "3"),
                    ModelNode::FileItem("file", "notes.txt", "2026-11-20", "Text", "2 KB", "go"),
                }),
                ModelNode::Search("find", "Find", "query", "Type to filter", "Ctrl+K"),
                ModelNode::ListBox("pick", "Format", "fmt", {"7z", "zip"}),
                CanvasBuilder("chart").ViewBox(100, 50).Rect(0, 0, 50, 50, 4, "accent").PointerState("pointer")
                    .Command("go").MinHeight(80).BuildNode(),
                ModelNode::StatusBar("status", "12 items", "1 selected", "Ready"),
            }))
            .State("open", "true").State("query", "").State("fmt", "1").State("pointer", "")
            .Command("go");
        const auto &built = rich.Build();
        const bool converted = native::ConvertUiModel(&built, output, commands, error) == PULSE_OK;
        check(converted && built.size == sizeof(PulseUiModelTree) && built.extra_count == 4,
              "builder emits extras table and full tree size when extras are used");
        if (!converted)
            std::printf("  %s\n", error.c_str());
        if (converted) {
            const auto &card = output.root.children[0];
            const auto &crumbs = output.root.children[1];
            check(card.padding == 20 && crumbs.padding == 0 && output.root.padding == -1,
                  "padding copied by node ID; unset stays default");
            check(card.children[0].text_role == "title" && card.children[1].text_role == "bodyStrong" &&
                      card.children[1].text_tone == "success" && card.children[2].text_role == "code" &&
                      card.children[2].text_tone == "secondary",
                  "text roles and tones copied");
            check(card.children[3].tooltip == "Re-run verification", "tooltip copied");
            check(output.root.children[5].pointer_state == "pointer", "canvas pointer state copied");
            check(crumbs.children[1].type == UiNodeType::Breadcrumb && crumbs.children[1].current &&
                      output.root.children[2].type == UiNodeType::SidebarSection &&
                      output.root.children[2].children[0].type == UiNodeType::SidebarItem &&
                      output.root.children[2].children[0].selected &&
                      output.root.children[2].children[1].type == UiNodeType::FileRow &&
                      output.root.children[3].type == UiNodeType::CommandSearch &&
                      output.root.children[4].type == UiNodeType::List &&
                      output.root.children[6].type == UiNodeType::StatusBar,
                  "re-opened controls map to the host renderer types");
        }
        auto plain = ModelTree(ModelNode::Label("plain", "No extras"));
        check(plain.Build().size == PULSE_UI_MODEL_TREE_SIZE_V1 &&
                  native::ConvertUiModel(&plain.Build(), output, commands, error) == PULSE_OK,
              "tree without extras is sent at the earlier V1 size and accepted");
        PulseUiModelTree odd = plain.Build();
        odd.size = PULSE_UI_MODEL_TREE_SIZE_V1 + 4;
        check(native::ConvertUiModel(&odd, output, commands, error) == PULSE_ERROR_ARGUMENT,
              "unknown tree size rejected");

        {
            auto preview = ModelTree(ModelNode::MenuPreview(
                "p", "", {ModelMenuItem("x", "Smart extract").Group("7-Zip").Primary().Detail("To backup"),
                          ModelMenuItem("c", "Compress").Group("7-Zip")})
                                         .Kind("context"));
            const bool ok = native::ConvertUiModel(&preview.Build(), output, commands, error) == PULSE_OK;
            const auto &items = output.root.menu;
            check(ok && output.root.menu_kind == "context" && items.size() == 2 && items[0].group == "7-Zip" &&
                      items[0].primary && items[0].detail == "To backup" && items[1].group == "7-Zip" &&
                      !items[1].primary && items[1].detail.empty(),
                  "menu item detail/group/primary and the context preview kind copied");
            // An array of records written by an earlier SDK: stepped by their own size, new fields absent.
            struct alignas(8) V1Record { unsigned char bytes[PULSE_UI_MODEL_MENU_ITEM_SIZE_V1]; };
            std::array<V1Record, 2> old_records{};
            std::array<PulseUiModelMenuItem, 2> filled{};
            const char *record_ids[] = {"old1", "old2"};
            for (size_t i = 0; i < 2; ++i) {
                filled[i].size = PULSE_UI_MODEL_MENU_ITEM_SIZE_V1;
                filled[i].id = T(record_ids[i]);
                filled[i].text = T("Old plugin");
                std::memcpy(old_records[i].bytes, &filled[i], PULSE_UI_MODEL_MENU_ITEM_SIZE_V1);
            }
            auto legacy = Node(PULSE_UI_MODEL_MENU_PREVIEW, "legacy");
            legacy.menu = {1, reinterpret_cast<const PulseUiModelMenuItem *>(old_records.data()), 2};
            PulseUiModelTree tree{};
            tree.size = sizeof(tree);
            tree.root = &legacy;
            check(native::ConvertUiModel(&tree, output, commands, error) == PULSE_OK && output.root.menu.size() == 2 &&
                      output.root.menu[1].id == "old2" && output.root.menu[0].group.empty() &&
                      output.root.menu_kind == "plain",
                  "menu item array at the earlier V1 size accepted");
            filled[1].size = sizeof(PulseUiModelMenuItem);
            std::memcpy(old_records[1].bytes, &filled[1], PULSE_UI_MODEL_MENU_ITEM_SIZE_V1);
            check(native::ConvertUiModel(&tree, output, commands, error) == PULSE_ERROR_ARGUMENT,
                  "mixed menu item record sizes rejected");
            filled[0].size = PULSE_UI_MODEL_MENU_ITEM_SIZE_V1 + 4;
            std::memcpy(old_records[0].bytes, &filled[0], PULSE_UI_MODEL_MENU_ITEM_SIZE_V1);
            check(native::ConvertUiModel(&tree, output, commands, error) == PULSE_ERROR_ARGUMENT,
                  "unknown menu item size rejected");
        }
        {
            auto art = CanvasBuilder("art").ViewBox(150, 56).MinHeight(64).BuildNode();
            auto tile = ModelNode::OptionCard("tile", "Smart extract", "One item lands directly", "mode");
            tile.Children({art});
            auto ok_tree = ModelTree(std::move(tile)).State("mode", "false");
            check(native::ConvertUiModel(&ok_tree.Build(), output, commands, error) == PULSE_OK &&
                      output.root.children.size() == 1 && output.root.children[0].type == UiNodeType::Canvas,
                  "option card takes one canvas illustration");
        }

        const auto rejected = [&](ModelTree tree, const char *name) {
            const bool ok = native::ConvertUiModel(&tree.Build(), output, commands, error) == PULSE_ERROR_ARGUMENT;
            check(ok, name);
        };
        rejected(ModelTree(ModelNode::Label("t", "x").Padding(8)), "padding on text rejected");
        rejected(ModelTree(ModelNode::VStack("s", {}).Padding(49)), "padding above 48 rejected");
        rejected(ModelTree(ModelNode::VStack("s", {}).Tooltip("x")), "tooltip on container rejected");
        rejected(ModelTree(ModelNode::Label("t", "x").Tooltip(std::string(257, 'a'))), "tooltip over 256 bytes rejected");
        rejected(ModelTree(ModelNode::Label("t", "x").PointerState("p")).State("p", ""), "pointer state outside canvas rejected");
        rejected(ModelTree(CanvasBuilder("c").ViewBox(10, 10).PointerState("missing").BuildNode()),
                 "undeclared pointer state rejected");
        rejected(ModelTree(ModelNode::Label("t", "x", "headline")), "unknown text role rejected");
        rejected(ModelTree(ModelNode::Label("t", "x", "body", "purple")), "unknown text tone rejected");
        const auto card_with = [](std::vector<ModelNode> children) {
            auto card = ModelNode::OptionCard("card", "Card", "", "mode");
            card.Children(std::move(children));
            return card;
        };
        rejected(ModelTree(card_with({CanvasBuilder("a").ViewBox(10, 10).State("mode").BuildNode()})).State("mode", "false"),
                 "option card canvas with its own state rejected");
        rejected(ModelTree(card_with({CanvasBuilder("a").ViewBox(10, 10).BuildNode(),
                                      CanvasBuilder("b").ViewBox(10, 10).BuildNode()}))
                     .State("mode", "false"),
                 "option card with two canvases rejected");
        rejected(ModelTree(card_with({ModelNode::Label("a", "x")})).State("mode", "false"),
                 "option card with a non-canvas child rejected");
        rejected(ModelTree(ModelNode::MenuPreview(
                     "p", "", {ModelMenuItem("a", "A").Children({ModelMenuItem("b", "B").Group("g")})})),
                 "nested menu group rejected");
        rejected(ModelTree(ModelNode::MenuPreview("p", "", {ModelMenuItem("a", "A")}).Kind("popup")),
                 "unknown menu preview kind rejected");

        auto base = ModelTree(ModelNode::Label("t", "x"));
        PulseUiModelTree manual = base.Build();
        PulseUiModelNodeExtra extras[2]{};
        extras[0].size = sizeof(extras[0]);
        extras[0].node = S("t");
        extras[0].tooltip = T("hint");
        extras[1] = extras[0];
        manual.size = sizeof(manual);
        manual.extras = extras;
        manual.extra_count = 1;
        check(native::ConvertUiModel(&manual, output, commands, error) == PULSE_OK && output.root.tooltip == "hint",
              "hand-built extras accepted");
        manual.extra_count = 2;
        check(native::ConvertUiModel(&manual, output, commands, error) == PULSE_ERROR_ARGUMENT,
              "duplicate extra node ID rejected");
        extras[1].node = S("nope");
        check(native::ConvertUiModel(&manual, output, commands, error) == PULSE_ERROR_ARGUMENT,
              "extra naming an absent node rejected");
        manual.extra_count = 1;
        extras[0].tooltip = {};
        check(native::ConvertUiModel(&manual, output, commands, error) == PULSE_ERROR_ARGUMENT,
              "empty extra rejected");
        extras[0].tooltip = T("hint");
        extras[0].size = sizeof(extras[0]) - 8;
        check(native::ConvertUiModel(&manual, output, commands, error) == PULSE_ERROR_ARGUMENT,
              "extra record size mismatch rejected");
        extras[0].size = sizeof(extras[0]);
        manual.extras = nullptr;
        check(native::ConvertUiModel(&manual, output, commands, error) == PULSE_ERROR_ARGUMENT,
              "extra count without array rejected");
    }
    std::printf("%d PASS, %d FAIL\n", passed, failed);
    return failed ? 1 : 0;
}

#pragma once
#include "sdk.hpp"
#include "icons.h"
#include "ui_model.h"
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace pulse::sdk {
// Owning author-side values. Build borrows storage until the next mutation/Build
// or destruction; host create/update copies synchronously. Rebuild after a move.
class ModelColumn {
    std::string id_, text_; int64_t width_;
    mutable PulseUiModelColumn built_{};
public:
    ModelColumn(std::string id, std::string text, int64_t width) : id_(std::move(id)), text_(std::move(text)), width_(width) {}
    const PulseUiModelColumn& Build() const {
        built_ = {}; built_.size = sizeof(built_); built_.id = View(id_); built_.text = View(text_); built_.width = {1, width_}; return built_;
    }
};
class ModelRow {
    std::string id_, text_; std::vector<std::string> cells_;
    std::vector<ModelRow> children_; bool cells_set_ = false, children_set_ = false;
    mutable std::vector<PulseStringView> cell_views_;
    mutable std::vector<PulseUiModelRow> child_views_;
    mutable PulseUiModelRow built_{};
public:
    ModelRow(std::string id, std::string text) : id_(std::move(id)), text_(std::move(text)) {}
    ModelRow& Cells(std::vector<std::string> values) { cells_ = std::move(values); cells_set_ = true; return *this; }
    ModelRow& Children(std::vector<ModelRow> values) { children_ = std::move(values); children_set_ = true; return *this; }
    const PulseUiModelRow& Build() const {
        built_ = {}; built_.size = sizeof(built_); built_.id = View(id_); built_.text = {1, View(text_)};
        cell_views_.clear(); for (const auto& c : cells_) cell_views_.push_back(View(c));
        child_views_.clear(); for (const auto& c : children_) child_views_.push_back(c.Build());
        if (cells_set_) built_.cells = {1, cell_views_.data(), static_cast<uint32_t>(cell_views_.size())};
        if (children_set_) built_.children = {1, child_views_.data(), static_cast<uint32_t>(child_views_.size())};
        return built_;
    }
};
class ModelMenuItem {
    std::string id_, text_, command_, glyph_, shortcut_, detail_, group_;
    bool primary_ = false;
    bool enabled_set_ = false, enabled_ = true;
    bool checked_set_ = false, checked_ = false;
    bool separator_set_ = false, separator_ = false;
    std::vector<ModelMenuItem> children_;
    bool children_set_ = false;
    mutable std::vector<PulseUiModelMenuItem> child_views_;
    mutable PulseUiModelMenuItem built_{};
public:
    ModelMenuItem(std::string id, std::string text, std::string command = {})
        : id_(std::move(id)), text_(std::move(text)), command_(std::move(command)) {}
    static ModelMenuItem Separator(std::string id) {
        ModelMenuItem item(std::move(id), {});
        item.separator_ = true;
        item.separator_set_ = true;
        return item;
    }
    ModelMenuItem& Glyph(std::string glyph) { glyph_ = std::move(glyph); return *this; }
    ModelMenuItem& Shortcut(std::string shortcut) { shortcut_ = std::move(shortcut); return *this; }
    ModelMenuItem& Enabled(bool value) { enabled_ = value; enabled_set_ = true; return *this; }
    ModelMenuItem& Checked(bool value) { checked_ = value; checked_set_ = true; return *this; }
    ModelMenuItem& Children(std::vector<ModelMenuItem> items) { children_ = std::move(items); children_set_ = true; return *this; }
    // Second caption line under the label (menuPreview rows grow to 40 DIP).
    ModelMenuItem& Detail(std::string detail) { detail_ = std::move(detail); return *this; }
    // Tinted item group: consecutive items with the same caption share one selection-gradient band.
    ModelMenuItem& Group(std::string caption) { group_ = std::move(caption); return *this; }
    // Semibold label for the group's main action.
    ModelMenuItem& Primary(bool value = true) { primary_ = value; return *this; }
    const PulseUiModelMenuItem& Build() const {
        built_ = {}; built_.size = sizeof(built_);
        built_.id = {1, View(id_)};
        if (!text_.empty()) built_.text = {1, View(text_)};
        if (!command_.empty()) built_.command = {1, View(command_)};
        if (!glyph_.empty()) built_.glyph = {1, View(glyph_)};
        if (!shortcut_.empty()) built_.shortcut = {1, View(shortcut_)};
        if (enabled_set_) built_.enabled = {1, enabled_ ? 1u : 0u};
        if (checked_set_) built_.checked = {1, checked_ ? 1u : 0u};
        if (separator_set_) built_.separator = {1, separator_ ? 1u : 0u};
        if (!detail_.empty()) built_.detail = {1, View(detail_)};
        if (!group_.empty()) built_.group = {1, View(group_)};
        if (primary_) built_.primary = {1, 1u};
        child_views_.clear(); for (const auto& c : children_) child_views_.push_back(c.Build());
        if (children_set_) built_.children = {1, child_views_.data(), static_cast<uint32_t>(child_views_.size())};
        return built_;
    }
};
class ModelTree;
class ModelNode {
    friend class ModelTree;
    uint32_t type_; std::string id_;
    int64_t padding_ = -1; std::string tooltip_, pointer_state_;
    std::vector<std::pair<PulseUiModelString PulseUiModelNode::*, std::string>> strings_;
    std::vector<std::pair<PulseUiModelInt PulseUiModelNode::*, int64_t>> ints_;
    std::vector<std::pair<PulseUiModelBool PulseUiModelNode::*, bool>> bools_;
    std::vector<ModelNode> children_; std::vector<std::string> options_;
    std::vector<ModelColumn> columns_; std::vector<ModelRow> rows_;
    std::vector<ModelMenuItem> menu_;
    bool children_set_ = false, options_set_ = false, columns_set_ = false, rows_set_ = false, menu_set_ = false;
    mutable PulseUiModelNode built_{};
    mutable std::vector<PulseUiModelNode> child_views_;
    mutable std::vector<PulseStringView> option_views_;
    mutable std::vector<PulseUiModelColumn> column_views_;
    mutable std::vector<PulseUiModelRow> row_views_;
    mutable std::vector<PulseUiModelMenuItem> menu_views_;
    template<class T, class V> static void Set(T& fields, typename T::value_type::first_type member, V value) {
        for (auto& field : fields) if (field.first == member) { field.second = std::move(value); return; }
        fields.emplace_back(member, std::move(value));
    }
public:
    ModelNode(uint32_t type, std::string id) : type_(type), id_(std::move(id)) {}
    ModelNode& String(PulseUiModelString PulseUiModelNode::* member, std::string value) { Set(strings_, member, std::move(value)); return *this; }
    ModelNode& Int(PulseUiModelInt PulseUiModelNode::* member, int64_t value) { Set(ints_, member, value); return *this; }
    ModelNode& Bool(PulseUiModelBool PulseUiModelNode::* member, bool value) { Set(bools_, member, value); return *this; }
    ModelNode& Children(std::vector<ModelNode> values) { children_ = std::move(values); children_set_ = true; return *this; }
    ModelNode& Options(std::vector<std::string> values) { options_ = std::move(values); options_set_ = true; return *this; }
    ModelNode& DataColumns(std::vector<ModelColumn> values) { columns_ = std::move(values); columns_set_ = true; return *this; }
    ModelNode& Rows(std::vector<ModelRow> values) { rows_ = std::move(values); rows_set_ = true; return *this; }
    ModelNode& Menu(std::vector<ModelMenuItem> values) { menu_ = std::move(values); menu_set_ = true; return *this; }
    // Presentation carried in PulseUiModelTree::extras (ModelTree collects them by node ID).
    ModelNode& Padding(int64_t dip) { padding_ = dip; return *this; }        // Stack/Grid/Group, 0..48
    ModelNode& Tooltip(std::string text) { tooltip_ = std::move(text); return *this; }  // non-container controls
    ModelNode& PointerState(std::string key) { pointer_state_ = std::move(key); return *this; }  // Canvas
    // Shorthand for the common fields; equivalent to String/Int/Bool with the matching member.
    ModelNode& Text(std::string value) { return String(&PulseUiModelNode::text, std::move(value)); }
    ModelNode& Command(std::string value) { return String(&PulseUiModelNode::command, std::move(value)); }
    ModelNode& State(std::string key) { return String(&PulseUiModelNode::state, std::move(key)); }
    ModelNode& Glyph(std::string value) { return String(&PulseUiModelNode::glyph, std::move(value)); }
    ModelNode& Kind(std::string value) { return String(&PulseUiModelNode::kind, std::move(value)); }
    ModelNode& Role(std::string value) { return String(&PulseUiModelNode::role, std::move(value)); }
    ModelNode& Detail(std::string value) { return String(&PulseUiModelNode::detail, std::move(value)); }
    ModelNode& Placeholder(std::string value) { return String(&PulseUiModelNode::placeholder, std::move(value)); }
    ModelNode& Badge(std::string value) { return String(&PulseUiModelNode::badge_text, std::move(value)); }
    ModelNode& Color(std::string value) { return String(&PulseUiModelNode::color, std::move(value)); }
    ModelNode& Alignment(std::string value) { return String(&PulseUiModelNode::alignment, std::move(value)); }
    ModelNode& Horizontal(bool value = true) { return String(&PulseUiModelNode::orientation, value ? "horizontal" : "vertical"); }
    ModelNode& Gap(int64_t dip) { return Int(&PulseUiModelNode::gap, dip); }
    ModelNode& Columns(int64_t count) { return Int(&PulseUiModelNode::columns, count); }
    ModelNode& Flex(int64_t weight) { return Int(&PulseUiModelNode::flex, weight); }
    ModelNode& MinWidth(int64_t dip) { return Int(&PulseUiModelNode::min_width, dip); }
    ModelNode& MaxWidth(int64_t dip) { return Int(&PulseUiModelNode::max_width, dip); }
    ModelNode& MinHeight(int64_t dip) { return Int(&PulseUiModelNode::min_height, dip); }
    ModelNode& Bordered(bool value = true) { return Bool(&PulseUiModelNode::bordered, value); }
    ModelNode& Selected(bool value = true) { return Bool(&PulseUiModelNode::selected, value); }
    ModelNode& Enabled(bool value) { return Bool(&PulseUiModelNode::enabled, value); }
    ModelNode& Visible(bool value) { return Bool(&PulseUiModelNode::visible, value); }
    // Typed constructors for the general-purpose controls.
    static ModelNode VStack(std::string id, std::vector<ModelNode> children, int64_t gap = 8) {
        ModelNode node(PULSE_UI_MODEL_STACK, std::move(id));
        node.Gap(gap).Children(std::move(children));
        return node;
    }
    static ModelNode HStack(std::string id, std::vector<ModelNode> children, int64_t gap = 8) {
        ModelNode node(PULSE_UI_MODEL_STACK, std::move(id));
        node.Horizontal().Gap(gap).Children(std::move(children));
        return node;
    }
    static ModelNode Card(std::string id, std::string title, std::vector<ModelNode> children, int64_t gap = 8) {
        ModelNode node(PULSE_UI_MODEL_GROUP, std::move(id));
        node.Bordered().Gap(gap).Children(std::move(children));
        if (!title.empty()) node.Text(std::move(title));
        return node;
    }
    // role: body, caption, section, bodyStrong, subtitle, title, code.
    // tone: default, secondary, accent, success, warning, danger.
    static ModelNode Label(std::string id, std::string text, std::string role = {}, std::string tone = {}) {
        ModelNode node(PULSE_UI_MODEL_TEXT, std::move(id));
        node.Text(std::move(text));
        if (!role.empty()) node.Role(std::move(role));
        if (!tone.empty()) node.Kind(std::move(tone));
        return node;
    }
    static ModelNode Button(std::string id, std::string text, std::string command, std::string kind = {}) {
        ModelNode node(PULSE_UI_MODEL_BUTTON, std::move(id));
        node.Text(std::move(text)).Command(std::move(command));
        if (!kind.empty()) node.Kind(std::move(kind));
        return node;
    }
    static ModelNode Toggle(std::string id, std::string label, std::string state_key) {
        ModelNode node(PULSE_UI_MODEL_SWITCH, std::move(id));
        node.Text(std::move(label)).State(std::move(state_key));
        return node;
    }
    static ModelNode Input(std::string id, std::string label, std::string state_key, std::string placeholder = {}) {
        ModelNode node(PULSE_UI_MODEL_TEXT_INPUT, std::move(id));
        node.Text(std::move(label)).State(std::move(state_key));
        if (!placeholder.empty()) node.Placeholder(std::move(placeholder));
        return node;
    }
    static ModelNode Search(std::string id, std::string label, std::string state_key, std::string placeholder = {},
                            std::string keycap = {}) {
        ModelNode node(PULSE_UI_MODEL_COMMAND_SEARCH, std::move(id));
        node.Text(std::move(label)).State(std::move(state_key));
        if (!placeholder.empty()) node.Placeholder(std::move(placeholder));
        if (!keycap.empty()) node.String(&PulseUiModelNode::keycap, std::move(keycap));
        return node;
    }
    static ModelNode Dropdown(std::string id, std::string label, std::string state_key, std::vector<std::string> options) {
        ModelNode node(PULSE_UI_MODEL_SELECT, std::move(id));
        node.Text(std::move(label)).State(std::move(state_key)).Options(std::move(options));
        return node;
    }
    static ModelNode ListBox(std::string id, std::string label, std::string state_key, std::vector<std::string> options) {
        ModelNode node(PULSE_UI_MODEL_LIST, std::move(id));
        node.Text(std::move(label)).State(std::move(state_key)).Options(std::move(options));
        return node;
    }
    static ModelNode NavItem(std::string id, std::string text, std::string command, std::string glyph = {},
                             bool selected = false, std::string badge = {}) {
        ModelNode node(PULSE_UI_MODEL_SIDEBAR_ITEM, std::move(id));
        node.Text(std::move(text)).Command(std::move(command));
        if (!glyph.empty()) node.Glyph(std::move(glyph));
        if (selected) node.Selected(true);
        if (!badge.empty()) node.Badge(std::move(badge));
        return node;
    }
    static ModelNode Crumb(std::string id, std::string text, std::string command, bool current = false) {
        ModelNode node(PULSE_UI_MODEL_BREADCRUMB, std::move(id));
        node.Text(std::move(text)).Command(std::move(command));
        if (current) node.Bool(&PulseUiModelNode::current, true).Bool(&PulseUiModelNode::separator, false);
        return node;
    }
    // Collapsible section: state_key holds "true" (expanded) or "false".
    static ModelNode Section(std::string id, std::string title, std::string state_key, std::vector<ModelNode> children,
                             std::string glyph = {}) {
        ModelNode node(PULSE_UI_MODEL_SIDEBAR_SECTION, std::move(id));
        node.Text(std::move(title)).State(std::move(state_key)).Children(std::move(children));
        if (!glyph.empty()) node.Glyph(std::move(glyph));
        return node;
    }
    static ModelNode FileItem(std::string id, std::string name, std::string modified = {}, std::string type = {},
                              std::string size = {}, std::string command = {}) {
        ModelNode node(PULSE_UI_MODEL_FILE_ROW, std::move(id));
        node.Text(std::move(name));
        if (!modified.empty()) node.String(&PulseUiModelNode::modified, std::move(modified));
        if (!type.empty()) node.String(&PulseUiModelNode::file_type, std::move(type));
        if (!size.empty()) node.String(&PulseUiModelNode::file_size, std::move(size));
        if (!command.empty()) node.Command(std::move(command));
        return node;
    }
    static ModelNode StatusBar(std::string id, std::string left, std::string center = {}, std::string right = {}) {
        ModelNode node(PULSE_UI_MODEL_STATUS_BAR, std::move(id));
        node.Text(std::move(left));
        if (!center.empty()) node.String(&PulseUiModelNode::center_text, std::move(center));
        if (!right.empty()) node.String(&PulseUiModelNode::right_text, std::move(right));
        return node;
    }
    static ModelNode PathRow(std::string id, std::string label, std::string state_key, std::string browse_command,
                             std::string button_text = "Browse...", std::string placeholder = {}) {
        ModelNode input(PULSE_UI_MODEL_TEXT_INPUT, id + ".input");
        input.String(&PulseUiModelNode::text, std::move(label))
             .String(&PulseUiModelNode::state, std::move(state_key))
             .Int(&PulseUiModelNode::flex, 1);
        if (!placeholder.empty()) input.String(&PulseUiModelNode::placeholder, std::move(placeholder));
        ModelNode browse(PULSE_UI_MODEL_BUTTON, id + ".browse");
        browse.String(&PulseUiModelNode::text, std::move(button_text))
              .String(&PulseUiModelNode::command, std::move(browse_command));
        std::vector<ModelNode> row_children;
        row_children.push_back(std::move(input));
        row_children.push_back(std::move(browse));
        ModelNode row(PULSE_UI_MODEL_STACK, std::move(id));
        row.String(&PulseUiModelNode::orientation, "horizontal")
           .Int(&PulseUiModelNode::gap, 8)
           .Children(std::move(row_children));
        return row;
    }
    static ModelNode LabeledField(std::string id, std::string label, ModelNode control, std::string helper = {}) {
        std::vector<ModelNode> items;
        ModelNode caption(PULSE_UI_MODEL_TEXT, id + ".label");
        caption.String(&PulseUiModelNode::text, std::move(label));
        items.push_back(std::move(caption));
        items.push_back(std::move(control));
        if (!helper.empty()) {
            ModelNode hint(PULSE_UI_MODEL_TEXT, id + ".helper");
            hint.String(&PulseUiModelNode::text, std::move(helper))
                .String(&PulseUiModelNode::role, "caption");
            items.push_back(std::move(hint));
        }
        ModelNode stack(PULSE_UI_MODEL_STACK, std::move(id));
        stack.Int(&PulseUiModelNode::gap, 4).Children(std::move(items));
        return stack;
    }
    static ModelNode OptionCard(std::string id, std::string title, std::string detail, std::string state_key,
                                std::string kind = "radio", std::string glyph = {}, std::string badge = {}) {
        ModelNode node(PULSE_UI_MODEL_OPTION_CARD, std::move(id));
        node.String(&PulseUiModelNode::text, std::move(title))
            .String(&PulseUiModelNode::state, std::move(state_key))
            .String(&PulseUiModelNode::kind, std::move(kind))
            .Bool(&PulseUiModelNode::bordered, true);
        if (!detail.empty()) node.String(&PulseUiModelNode::detail, std::move(detail));
        if (!glyph.empty()) node.String(&PulseUiModelNode::glyph, std::move(glyph));
        if (!badge.empty()) node.String(&PulseUiModelNode::badge_text, std::move(badge));
        return node;
    }
    static ModelNode SettingsRow(std::string id, std::string title, std::string detail, ModelNode control,
                                 std::string glyph = {}, bool bordered = false) {
        std::vector<ModelNode> ctrls;
        ctrls.push_back(std::move(control));
        ModelNode node(PULSE_UI_MODEL_SETTINGS_ROW, std::move(id));
        node.String(&PulseUiModelNode::text, std::move(title))
            .Bool(&PulseUiModelNode::bordered, bordered)
            .Children(std::move(ctrls));
        if (!detail.empty()) node.String(&PulseUiModelNode::detail, std::move(detail));
        if (!glyph.empty()) node.String(&PulseUiModelNode::glyph, std::move(glyph));
        return node;
    }
    static ModelNode Image(std::string id, std::string source, std::string alt_text = {},
                           std::string fit = "contain", int64_t min_height = 64) {
        ModelNode node(PULSE_UI_MODEL_IMAGE, std::move(id));
        node.String(&PulseUiModelNode::glyph, std::move(source))
            .String(&PulseUiModelNode::kind, std::move(fit))
            .Int(&PulseUiModelNode::min_height, min_height);
        if (!alt_text.empty()) node.String(&PulseUiModelNode::text, std::move(alt_text));
        return node;
    }
    static ModelNode MenuPreview(std::string id, std::string title, std::vector<ModelMenuItem> items,
                                 std::string detail = {}) {
        ModelNode node(PULSE_UI_MODEL_MENU_PREVIEW, std::move(id));
        if (!title.empty()) node.String(&PulseUiModelNode::text, std::move(title));
        if (!detail.empty()) node.String(&PulseUiModelNode::detail, std::move(detail));
        node.Bool(&PulseUiModelNode::bordered, true).Menu(std::move(items));
        return node;
    }
    static ModelNode Chip(std::string id, std::string label, std::string state_key,
                          std::string color = {}, std::string glyph = {}) {
        ModelNode node(PULSE_UI_MODEL_TAG, std::move(id));
        node.String(&PulseUiModelNode::text, std::move(label))
            .String(&PulseUiModelNode::state, std::move(state_key))
            .Bool(&PulseUiModelNode::bordered, true);
        if (!color.empty()) node.String(&PulseUiModelNode::color, std::move(color));
        if (!glyph.empty()) node.String(&PulseUiModelNode::glyph, std::move(glyph));
        return node;
    }
    const PulseUiModelNode& Build() const {
        built_ = {}; built_.size = sizeof(built_); built_.type = type_; built_.id = View(id_);
        for (const auto& [member, value] : strings_) built_.*member = {1, View(value)};
        for (const auto& [member, value] : ints_) built_.*member = {1, value};
        for (const auto& [member, value] : bools_) built_.*member = {1, value ? 1u : 0u};
        child_views_.clear(); for (const auto& child : children_) child_views_.push_back(child.Build());
        option_views_.clear(); for (const auto& option : options_) option_views_.push_back(View(option));
        column_views_.clear(); for (const auto& column : columns_) column_views_.push_back(column.Build());
        row_views_.clear(); for (const auto& row : rows_) row_views_.push_back(row.Build());
        menu_views_.clear(); for (const auto& item : menu_) menu_views_.push_back(item.Build());
        if (children_set_) built_.children = {1, child_views_.data(), static_cast<uint32_t>(child_views_.size())};
        if (options_set_) built_.options = {1, option_views_.data(), static_cast<uint32_t>(option_views_.size())};
        if (columns_set_) built_.data_columns = {1, column_views_.data(), static_cast<uint32_t>(column_views_.size())};
        if (rows_set_) built_.rows = {1, row_views_.data(), static_cast<uint32_t>(row_views_.size())};
        if (menu_set_) built_.menu = {1, menu_views_.data(), static_cast<uint32_t>(menu_views_.size())};
        return built_;
    }
};
class ModelTree {
    ModelNode root_; std::vector<std::pair<std::string, std::string>> states_; std::vector<std::string> commands_;
    mutable PulseUiModelTree built_{};
    mutable std::vector<PulseUiState> state_views_; mutable std::vector<PulseStringView> command_views_;
    mutable std::vector<PulseUiModelNodeExtra> extra_views_;
    void CollectExtras(const ModelNode& node) const {
        if (node.padding_ >= 0 || !node.tooltip_.empty() || !node.pointer_state_.empty()) {
            PulseUiModelNodeExtra extra{};
            extra.size = sizeof(extra);
            extra.node = View(node.id_);
            if (node.padding_ >= 0) extra.padding = {1, node.padding_};
            if (!node.tooltip_.empty()) extra.tooltip = {1, View(node.tooltip_)};
            if (!node.pointer_state_.empty()) extra.pointer_state = {1, View(node.pointer_state_)};
            extra_views_.push_back(extra);
        }
        for (const auto& child : node.children_) CollectExtras(child);
    }
public:
    explicit ModelTree(ModelNode root) : root_(std::move(root)) {}
    ModelTree& State(std::string key, std::string value) {
        for (auto& state : states_) if (state.first == key) { state.second = std::move(value); return *this; }
        states_.emplace_back(std::move(key), std::move(value)); return *this;
    }
    ModelTree& Command(std::string id) { commands_.push_back(std::move(id)); return *this; }
    const PulseUiModelTree& Build() const {
        built_ = {}; built_.size = sizeof(built_); built_.root = &root_.Build();
        state_views_.clear(); for (const auto& [key, value] : states_) state_views_.push_back({View(key), View(value)});
        command_views_.clear(); for (const auto& command : commands_) command_views_.push_back(View(command));
        built_.states = state_views_.data(); built_.state_count = static_cast<uint32_t>(state_views_.size());
        built_.commands = command_views_.data(); built_.command_count = static_cast<uint32_t>(command_views_.size());
        extra_views_.clear(); CollectExtras(root_);
        if (extra_views_.empty()) {
            built_.size = PULSE_UI_MODEL_TREE_SIZE_V1;  // accepted by hosts that predate extras
        } else {
            built_.extras = extra_views_.data(); built_.extra_count = static_cast<uint32_t>(extra_views_.size());
        }
        return built_;
    }
};
inline std::string IconGlyph(std::string_view name) { return std::string("icon:").append(name); }
inline std::string IconGlyph(uint32_t id) { return "icon:#" + std::to_string(id); }
class CanvasBuilder {
    std::string id_, state_, command_, color_, pointer_state_, tooltip_;
    int64_t min_height_ = 0;
    bool bordered_ = false, bordered_set_ = false;
    std::vector<std::string> cmds_;
    static std::string Fmt(double v) {
        char buf[32];
        const int n = std::snprintf(buf, sizeof(buf), "%.2f", v);
        return n > 0 ? std::string(buf, static_cast<size_t>(n)) : "0";
    }
    static std::string Quote(std::string_view text) {
        std::string out;
        out.reserve(text.size() + 2);
        out.push_back('"');
        for (const char ch : text) {
            if (ch == '"' || ch == '\\') out.push_back('\\');
            out.push_back(ch);
        }
        out.push_back('"');
        return out;
    }
public:
    explicit CanvasBuilder(std::string id) : id_(std::move(id)) {}
    CanvasBuilder& ViewBox(double w, double h) { cmds_.push_back("viewbox " + Fmt(w) + " " + Fmt(h)); return *this; }
    CanvasBuilder& Clear(std::string_view color) { cmds_.push_back("clear " + std::string(color)); return *this; }
    CanvasBuilder& Rect(double x, double y, double w, double h, double rx, std::string_view fill, std::string_view stroke = "none", double stroke_w = 1.0) {
        cmds_.push_back("rect " + Fmt(x) + " " + Fmt(y) + " " + Fmt(w) + " " + Fmt(h) + " " + Fmt(rx) +
                        " fill=" + std::string(fill) + " stroke=" + std::string(stroke) + " width=" + Fmt(stroke_w));
        return *this;
    }
    CanvasBuilder& Circle(double cx, double cy, double r, std::string_view fill, std::string_view stroke = "none", double stroke_w = 1.0) {
        cmds_.push_back("circle " + Fmt(cx) + " " + Fmt(cy) + " " + Fmt(r) +
                        " fill=" + std::string(fill) + " stroke=" + std::string(stroke) + " width=" + Fmt(stroke_w));
        return *this;
    }
    CanvasBuilder& Ellipse(double cx, double cy, double rx, double ry, std::string_view fill, std::string_view stroke = "none", double stroke_w = 1.0) {
        cmds_.push_back("ellipse " + Fmt(cx) + " " + Fmt(cy) + " " + Fmt(rx) + " " + Fmt(ry) +
                        " fill=" + std::string(fill) + " stroke=" + std::string(stroke) + " width=" + Fmt(stroke_w));
        return *this;
    }
    CanvasBuilder& Line(double x1, double y1, double x2, double y2, std::string_view stroke, double stroke_w = 1.25) {
        cmds_.push_back("line " + Fmt(x1) + " " + Fmt(y1) + " " + Fmt(x2) + " " + Fmt(y2) +
                        " stroke=" + std::string(stroke) + " width=" + Fmt(stroke_w));
        return *this;
    }
    CanvasBuilder& Path(std::string_view fill, std::string_view stroke, double stroke_w, std::string_view svg_d) {
        cmds_.push_back("path d=" + Quote(svg_d) + " fill=" + std::string(fill) +
                        " stroke=" + std::string(stroke) + " width=" + Fmt(stroke_w));
        return *this;
    }
    CanvasBuilder& Icon(std::string_view icon_name_or_hash, double x, double y, double size, std::string_view color = "text", double stroke_w = 1.35) {
        if (icon_name_or_hash.size() >= 5 && icon_name_or_hash.substr(0, 5) == "icon:") icon_name_or_hash.remove_prefix(5);
        cmds_.push_back("icon " + Fmt(x) + " " + Fmt(y) + " " + Fmt(size) + " " + std::string(icon_name_or_hash) +
                        " color=" + std::string(color) + " stroke=" + Fmt(stroke_w));
        return *this;
    }
    CanvasBuilder& Icon(PulseIconId icon_id, double x, double y, double size, std::string_view color = "text", double stroke_w = 1.35) {
        return Icon("#" + std::to_string(icon_id), x, y, size, color, stroke_w);
    }
    CanvasBuilder& Text(double x, double y, double w, double h, std::string_view color, double font_size, std::string_view align, std::string_view content) {
        const std::string_view role = font_size <= 12.5 ? "caption" : (font_size >= 15.5 ? "section" : "body");
        cmds_.push_back("text " + Fmt(x) + " " + Fmt(y) + " " + Quote(content) +
                        " w=" + Fmt(w) + " h=" + Fmt(h) + " color=" + std::string(color) +
                        " align=" + std::string(align) + " role=" + std::string(role));
        return *this;
    }
    // Text at an exact size in viewBox units, so it scales with the canvas (a label on an icon);
    // weight is "normal", "semibold" or "bold". The host draws it with the UI text renderer
    // (LumaText); hosts that predate size= draw it at the body size.
    CanvasBuilder& ScaledText(double x, double y, double w, double h, std::string_view color, double size,
                              std::string_view weight, std::string_view align, std::string_view content) {
        cmds_.push_back("text " + Fmt(x) + " " + Fmt(y) + " " + Quote(content) +
                        " w=" + Fmt(w) + " h=" + Fmt(h) + " color=" + std::string(color) +
                        " align=" + std::string(align) + " role=body size=" + Fmt(size) + " weight=" + std::string(weight));
        return *this;
    }
    CanvasBuilder& Text(double x, double y, std::string_view content, std::string_view color = "text",
                        std::string_view role = "body", std::string_view align = "left") {
        cmds_.push_back("text " + Fmt(x) + " " + Fmt(y) + " " + Quote(content) +
                        " color=" + std::string(color) + " role=" + std::string(role) + " align=" + std::string(align));
        return *this;
    }
    CanvasBuilder& Bordered(bool v = true) { bordered_ = v; bordered_set_ = true; return *this; }
    CanvasBuilder& Background(std::string color) { color_ = std::move(color); return *this; }
    CanvasBuilder& Command(std::string cmd) { command_ = std::move(cmd); return *this; }
    CanvasBuilder& State(std::string key) { state_ = std::move(key); return *this; }
    CanvasBuilder& MinHeight(int64_t dip) { min_height_ = dip; return *this; }
    // Declared state key that receives "move X Y" / "leave" / "click X Y" in viewBox units.
    CanvasBuilder& PointerState(std::string key) { pointer_state_ = std::move(key); return *this; }
    CanvasBuilder& Tooltip(std::string text) { tooltip_ = std::move(text); return *this; }
    ModelNode BuildNode() const {
        ModelNode node(PULSE_UI_MODEL_CANVAS, id_);
        if (!cmds_.empty()) node.Options(cmds_);
        if (bordered_set_) node.Bool(&PulseUiModelNode::bordered, bordered_);
        if (!color_.empty()) node.String(&PulseUiModelNode::color, color_);
        if (!command_.empty()) node.String(&PulseUiModelNode::command, command_);
        if (!state_.empty()) node.String(&PulseUiModelNode::state, state_);
        if (min_height_ > 0) node.Int(&PulseUiModelNode::min_height, min_height_);
        if (!pointer_state_.empty()) node.PointerState(pointer_state_);
        if (!tooltip_.empty()) node.Tooltip(tooltip_);
        return node;
    }
};
}
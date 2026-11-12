#pragma once
#include "sdk.hpp"
#include "ui_model.h"
#include <cstdio>
#include <string>
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
class ModelNode {
    uint32_t type_; std::string id_;
    std::vector<std::pair<PulseUiModelString PulseUiModelNode::*, std::string>> strings_;
    std::vector<std::pair<PulseUiModelInt PulseUiModelNode::*, int64_t>> ints_;
    std::vector<std::pair<PulseUiModelBool PulseUiModelNode::*, bool>> bools_;
    std::vector<ModelNode> children_; std::vector<std::string> options_;
    std::vector<ModelColumn> columns_; std::vector<ModelRow> rows_;
    bool children_set_ = false, options_set_ = false, columns_set_ = false, rows_set_ = false;
    mutable PulseUiModelNode built_{};
    mutable std::vector<PulseUiModelNode> child_views_;
    mutable std::vector<PulseStringView> option_views_;
    mutable std::vector<PulseUiModelColumn> column_views_;
    mutable std::vector<PulseUiModelRow> row_views_;
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
    const PulseUiModelNode& Build() const {
        built_ = {}; built_.size = sizeof(built_); built_.type = type_; built_.id = View(id_);
        for (const auto& [member, value] : strings_) built_.*member = {1, View(value)};
        for (const auto& [member, value] : ints_) built_.*member = {1, value};
        for (const auto& [member, value] : bools_) built_.*member = {1, value ? 1u : 0u};
        child_views_.clear(); for (const auto& child : children_) child_views_.push_back(child.Build());
        option_views_.clear(); for (const auto& option : options_) option_views_.push_back(View(option));
        column_views_.clear(); for (const auto& column : columns_) column_views_.push_back(column.Build());
        row_views_.clear(); for (const auto& row : rows_) row_views_.push_back(row.Build());
        if (children_set_) built_.children = {1, child_views_.data(), static_cast<uint32_t>(child_views_.size())};
        if (options_set_) built_.options = {1, option_views_.data(), static_cast<uint32_t>(option_views_.size())};
        if (columns_set_) built_.data_columns = {1, column_views_.data(), static_cast<uint32_t>(column_views_.size())};
        if (rows_set_) built_.rows = {1, row_views_.data(), static_cast<uint32_t>(row_views_.size())};
        return built_;
    }
};
class ModelTree {
    ModelNode root_; std::vector<std::pair<std::string, std::string>> states_; std::vector<std::string> commands_;
    mutable PulseUiModelTree built_{};
    mutable std::vector<PulseUiState> state_views_; mutable std::vector<PulseStringView> command_views_;
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
        return built_;
    }
};
inline std::string IconGlyph(std::string_view name) { return std::string("icon:").append(name); }
inline std::string IconGlyph(uint32_t id) { return "icon:#" + std::to_string(id); }
class CanvasBuilder {
    std::string id_, state_, command_, color_;
    int64_t min_height_ = 0;
    bool bordered_ = false, bordered_set_ = false;
    std::vector<std::string> cmds_;
    static std::string Fmt(double v) {
        char buf[32];
        const int n = std::snprintf(buf, sizeof(buf), "%.2f", v);
        return n > 0 ? std::string(buf, static_cast<size_t>(n)) : "0";
    }
public:
    explicit CanvasBuilder(std::string id) : id_(std::move(id)) {}
    CanvasBuilder& ViewBox(double w, double h) { cmds_.push_back("viewbox " + Fmt(w) + " " + Fmt(h)); return *this; }
    CanvasBuilder& Clear(std::string_view color) { cmds_.push_back("clear " + std::string(color)); return *this; }
    CanvasBuilder& Rect(double x, double y, double w, double h, double rx, std::string_view fill, std::string_view stroke = "none", double stroke_w = 1.0) {
        cmds_.push_back("rect " + Fmt(x) + " " + Fmt(y) + " " + Fmt(w) + " " + Fmt(h) + " " + Fmt(rx) + " " + std::string(fill) + " " + std::string(stroke) + " " + Fmt(stroke_w));
        return *this;
    }
    CanvasBuilder& Circle(double cx, double cy, double r, std::string_view fill, std::string_view stroke = "none", double stroke_w = 1.0) {
        cmds_.push_back("circle " + Fmt(cx) + " " + Fmt(cy) + " " + Fmt(r) + " " + std::string(fill) + " " + std::string(stroke) + " " + Fmt(stroke_w));
        return *this;
    }
    CanvasBuilder& Ellipse(double cx, double cy, double rx, double ry, std::string_view fill, std::string_view stroke = "none", double stroke_w = 1.0) {
        cmds_.push_back("ellipse " + Fmt(cx) + " " + Fmt(cy) + " " + Fmt(rx) + " " + Fmt(ry) + " " + std::string(fill) + " " + std::string(stroke) + " " + Fmt(stroke_w));
        return *this;
    }
    CanvasBuilder& Line(double x1, double y1, double x2, double y2, std::string_view stroke, double stroke_w = 1.25) {
        cmds_.push_back("line " + Fmt(x1) + " " + Fmt(y1) + " " + Fmt(x2) + " " + Fmt(y2) + " " + std::string(stroke) + " " + Fmt(stroke_w));
        return *this;
    }
    CanvasBuilder& Path(std::string_view fill, std::string_view stroke, double stroke_w, std::string_view svg_d) {
        cmds_.push_back("path " + std::string(fill) + " " + std::string(stroke) + " " + Fmt(stroke_w) + " | " + std::string(svg_d));
        return *this;
    }
    CanvasBuilder& Icon(std::string_view icon_name_or_hash, double x, double y, double size, std::string_view color = "text", double stroke_w = 1.35) {
        cmds_.push_back("icon " + std::string(icon_name_or_hash) + " " + Fmt(x) + " " + Fmt(y) + " " + Fmt(size) + " " + std::string(color) + " " + Fmt(stroke_w));
        return *this;
    }
    CanvasBuilder& Text(double x, double y, double w, double h, std::string_view color, double font_size, std::string_view align, std::string_view content) {
        cmds_.push_back("text " + Fmt(x) + " " + Fmt(y) + " " + Fmt(w) + " " + Fmt(h) + " " + std::string(color) + " " + Fmt(font_size) + " " + std::string(align) + " | " + std::string(content));
        return *this;
    }
    CanvasBuilder& Bordered(bool v = true) { bordered_ = v; bordered_set_ = true; return *this; }
    CanvasBuilder& Background(std::string color) { color_ = std::move(color); return *this; }
    CanvasBuilder& Command(std::string cmd) { command_ = std::move(cmd); return *this; }
    CanvasBuilder& State(std::string key) { state_ = std::move(key); return *this; }
    CanvasBuilder& MinHeight(int64_t dip) { min_height_ = dip; return *this; }
    ModelNode BuildNode() const {
        ModelNode node(PULSE_UI_MODEL_CANVAS, id_);
        if (!cmds_.empty()) node.Options(cmds_);
        if (bordered_set_) node.Bool(&PulseUiModelNode::bordered, bordered_);
        if (!color_.empty()) node.String(&PulseUiModelNode::color, color_);
        if (!command_.empty()) node.String(&PulseUiModelNode::command, command_);
        if (!state_.empty()) node.String(&PulseUiModelNode::state, state_);
        if (min_height_ > 0) node.Int(&PulseUiModelNode::min_height, min_height_);
        return node;
    }
};
}

#pragma once
#include "sdk.hpp"
#include "ui_model.h"
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
}

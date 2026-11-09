#include "ui_model.h"
#include "../../../common/json_value.h"
#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <windows.h>

namespace pulse::extensions::native {
namespace {
// Public ABI identities map to semantic names independently of the private enum order.
constexpr std::pair<uint32_t, std::string_view> kModelTypeNames[] = {
    {PULSE_UI_MODEL_STACK, "stack"},
    {PULSE_UI_MODEL_GRID, "grid"},
    {PULSE_UI_MODEL_GROUP, "group"},
    {PULSE_UI_MODEL_SCROLL_VIEW, "scrollView"},
    {PULSE_UI_MODEL_TEXT, "text"},
    {PULSE_UI_MODEL_BUTTON, "button"},
    {PULSE_UI_MODEL_TEXT_INPUT, "textInput"},
    {PULSE_UI_MODEL_CHECKBOX, "checkbox"},
    {PULSE_UI_MODEL_SELECT, "select"},
    {PULSE_UI_MODEL_PROGRESS, "progress"},
    {PULSE_UI_MODEL_DIVIDER, "divider"},
    {PULSE_UI_MODEL_TABS, "tabs"},
    {PULSE_UI_MODEL_TAB, "tab"},
    {PULSE_UI_MODEL_SWITCH, "switch"},
    {PULSE_UI_MODEL_RADIO, "radio"},
    {PULSE_UI_MODEL_SLIDER, "slider"},
    {PULSE_UI_MODEL_SEGMENTED, "segmented"},
    {PULSE_UI_MODEL_BADGE, "badge"},
    {PULSE_UI_MODEL_TAG, "tag"},
    {PULSE_UI_MODEL_INFO_BAR, "infoBar"},
    {PULSE_UI_MODEL_SPLIT_BUTTON, "splitButton"},
    {PULSE_UI_MODEL_MENU_BUTTON, "menuButton"},
    {PULSE_UI_MODEL_EMPTY_STATE, "emptyState"},
    {PULSE_UI_MODEL_VIRTUAL_LIST, "virtualList"},
    {PULSE_UI_MODEL_TREE, "tree"},
    {PULSE_UI_MODEL_DATA_TABLE, "dataTable"},
    {PULSE_UI_MODEL_SPLIT_PANE, "splitPane"},
};
static_assert(std::size(kModelTypeNames) == 27);
constexpr std::string_view ModelTypeName(uint32_t type) {
    for (const auto &entry : kModelTypeNames)
        if (entry.first == type)
            return entry.second;
    return {};
}
struct Rejected {
    PulseStatus status;
    std::string message;
};
struct Builder {
    size_t bytes = 0, nodes = 0, menus = 0, rows = 0;
    std::set<const void *> seen_nodes, seen_menus, seen_rows;
    [[noreturn]] void Fail(const std::string &message, PulseStatus status = PULSE_ERROR_ARGUMENT) {
        throw Rejected{status, message};
    }
    std::string Text(PulseStringView view) {
        if (view.length > 1024 || (view.length && !view.data) || view.length > 65536 - bytes)
            Fail("invalid string or total UTF-8 budget exceeded");
        std::string text(view.data ? view.data : "", view.length);
        if (text.find('\0') != std::string::npos ||
            (!text.empty() && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                                   static_cast<int>(text.size()), nullptr, 0)))
            Fail("invalid UTF-8 or embedded NUL");
        bytes += view.length;
        return text;
    }
    void Present(uint32_t present) {
        if (present > 1)
            Fail("presence must be zero or one");
    }
    void Field(json::Value &object, const char *name, PulseUiModelString value) {
        Present(value.present);
        if (value.present)
            object.set(name, Text(value.value));
        else if (value.value.data || value.value.length)
            Fail(std::string(name) + ": data on absent field");
    }
    void Field(json::Value &object, const char *name, PulseUiModelBool value) {
        Present(value.present);
        if (value.value > 1 || (!value.present && value.value))
            Fail(std::string(name) + ": invalid boolean");
        if (value.present)
            object.set(name, value.value != 0);
    }
    void Field(json::Value &object, const char *name, PulseUiModelInt value) {
        Present(value.present);
        if ((!value.present && value.value) || value.value < -(int64_t{1} << 53) || value.value > (int64_t{1} << 53))
            Fail(std::string(name) + ": invalid integer");
        if (value.present)
            object.set(name, value.value);
    }
    template <class T> bool Array(uint32_t present, const T *data, uint32_t count, uint32_t limit) {
        Present(present);
        if (count > limit || (count && !data) || (!present && (data || count)))
            Fail("invalid bounded array");
        return present != 0;
    }
    void Strings(json::Value &object, const char *name, PulseUiModelStrings input, uint32_t limit = 64) {
        if (!Array(input.present, input.data, input.count, limit))
            return;
        auto values = json::Value::Array();
        for (uint32_t i = 0; i < input.count; ++i)
            values.push(Text(input.data[i]));
        object.set(name, std::move(values));
    }
    json::Value States(const PulseUiState *entries, uint32_t count) {
        if (count > 32 || (count && !entries))
            Fail("state/update count exceeds 32");
        auto array = json::Value::Array();
        for (uint32_t i = 0; i < count; ++i) {
            auto value = json::Value::Object();
            value.set("key", Text(entries[i].key));
            value.set("value", Text(entries[i].value));
            array.push(std::move(value));
        }
        return array;
    }
    void Menu(json::Value &object, const char *name, PulseUiModelMenus input, size_t depth) {
        if (!Array(input.present, input.data, input.count, 128))
            return;
        auto array = json::Value::Array();
        for (uint32_t i = 0; i < input.count; ++i) {
            const auto &source = input.data[i];
            if (depth > 4 || ++menus > 128 || source.size != sizeof(source) || !seen_menus.insert(&source).second)
                Fail("menu depth/budget/cycle or invalid record size");
            auto value = json::Value::Object();
            Field(value, "id", source.id);
            Field(value, "text", source.text);
            Field(value, "command", source.command);
            Field(value, "glyph", source.glyph);
            Field(value, "shortcut", source.shortcut);
            Field(value, "enabled", source.enabled);
            Field(value, "checked", source.checked);
            Field(value, "separator", source.separator);
            Menu(value, "children", source.children, depth + 1);
            array.push(std::move(value));
        }
        object.set(name, std::move(array));
    }
    void Rows(json::Value &object, PulseUiModelRows input, size_t depth) {
        if (!Array(input.present, input.data, input.count, 4096))
            return;
        auto array = json::Value::Array();
        for (uint32_t i = 0; i < input.count; ++i) {
            const auto &source = input.data[i];
            if (depth > 8 || ++rows > 4096 || source.size != sizeof(source) || !seen_rows.insert(&source).second)
                Fail("row depth/budget/cycle or invalid record size");
            auto value = json::Value::Object();
            value.set("id", Text(source.id));
            Field(value, "text", source.text);
            Field(value, "glyph", source.glyph);
            Field(value, "command", source.command);
            Field(value, "enabled", source.enabled);
            Field(value, "expanded", source.expanded);
            Strings(value, "cells", source.cells, 16);
            if (source.children.present) {
                auto nested = json::Value::Object();
                Rows(nested, source.children, depth + 1);
                value.set("children", *nested.find("rows"));
            } else if (source.children.count || source.children.data)
                Fail("data on absent row children");
            array.push(std::move(value));
        }
        object.set("rows", std::move(array));
    }
    json::Value Node(const PulseUiModelNode &source, size_t depth) {
        if (depth > 8 || ++nodes > 128 || source.size != sizeof(source) || ModelTypeName(source.type).empty() ||
            !seen_nodes.insert(&source).second)
            Fail("node depth/budget/cycle or invalid record size/type");
        if (source.data_source)
            Fail("native UI data provider is not connected", PULSE_ERROR_UNAVAILABLE);
        auto value = json::Value::Object();
        value.set("id", Text(source.id));
        value.set("type", ModelTypeName(source.type));
        // Named scalar mappings follow the existing model/schema property names exactly.
        Field(value, "text", source.text);
        Field(value, "command", source.command);
        Field(value, "binding", source.binding);
        Field(value, "state", source.state);
        Field(value, "orientation", source.orientation);
        Field(value, "placeholder", source.placeholder);
        Field(value, "glyph", source.glyph);
        Field(value, "keycap", source.keycap);
        Field(value, "detail", source.detail);
        Field(value, "message", source.message);
        Field(value, "color", source.color);
        Field(value, "closeCommand", source.close_command);
        Field(value, "enabledBinding", source.enabled_binding);
        Field(value, "visibleBinding", source.visible_binding);
        Field(value, "role", source.role);
        Field(value, "alignment", source.alignment);
        Field(value, "badgeText", source.badge_text);
        Field(value, "status", source.status);
        Field(value, "trailingGlyph", source.trailing_glyph);
        Field(value, "summary", source.summary);
        Field(value, "filterText", source.filter_text);
        Field(value, "filterCommand", source.filter_command);
        Field(value, "sortCommand", source.sort_command);
        Field(value, "modified", source.modified);
        Field(value, "fileType", source.file_type);
        Field(value, "fileSize", source.file_size);
        Field(value, "helper", source.helper);
        Field(value, "countLabel", source.count_label);
        Field(value, "actionText", source.action_text);
        Field(value, "centerText", source.center_text);
        Field(value, "rightText", source.right_text);
        Field(value, "kind", source.kind);
        Field(value, "checked", source.checked);
        Field(value, "enabled", source.enabled);
        Field(value, "visible", source.visible);
        Field(value, "iconOnly", source.icon_only);
        Field(value, "bordered", source.bordered);
        Field(value, "selected", source.selected);
        Field(value, "current", source.current);
        Field(value, "separator", source.separator);
        Field(value, "dot", source.dot);
        Field(value, "moveIntent", source.move_intent);
        Field(value, "expanded", source.expanded);
        Field(value, "cut", source.cut);
        Field(value, "missing", source.missing);
        Field(value, "ready", source.ready);
        Field(value, "value", source.value);
        Field(value, "columns", source.columns);
        Field(value, "gap", source.gap);
        Field(value, "minHeight", source.min_height);
        Field(value, "minWidth", source.min_width);
        Field(value, "maxWidth", source.max_width);
        Field(value, "maxHeight", source.max_height);
        Field(value, "flex", source.flex);
        Field(value, "stops", source.stops);
        Field(value, "itemCount", source.item_count);
        Field(value, "capacity", source.capacity);
        Field(value, "freeBytes", source.free_bytes);
        Field(value, "totalBytes", source.total_bytes);
        if (const auto *binding = value.find("binding"); binding && binding->as_string().starts_with("context."))
            Fail("native UI context resolver is not connected", PULSE_ERROR_UNAVAILABLE);
        Strings(value, "options", source.options);
        Strings(value, "tagColors", source.tag_colors);
        Menu(value, "menu", source.menu, 1);
        Menu(value, "actions", source.actions, 1);
        Rows(value, source.rows, 1);
        if (Array(source.data_columns.present, source.data_columns.data, source.data_columns.count, 16)) {
            if (source.columns.present)
                Fail("integer and data columns are mutually exclusive");
            auto columns = json::Value::Array();
            for (uint32_t i = 0; i < source.data_columns.count; ++i) {
                const auto &column = source.data_columns.data[i];
                if (column.size != sizeof(column))
                    Fail("invalid column size");
                auto entry = json::Value::Object();
                entry.set("id", Text(column.id));
                entry.set("text", Text(column.text));
                Field(entry, "width", column.width);
                columns.push(std::move(entry));
            }
            value.set("columns", std::move(columns));
        }
        if (source.validation) {
            const auto &validation = *source.validation;
            if (validation.size != sizeof(validation))
                Fail("invalid validation size");
            auto entry = json::Value::Object();
            Field(entry, "required", validation.required);
            Field(entry, "minLength", validation.min_length);
            Field(entry, "maxLength", validation.max_length);
            entry.set("message", Text(validation.message));
            value.set("validation", std::move(entry));
        }
        if (Array(source.children.present, source.children.data, source.children.count, 128)) {
            auto children = json::Value::Array();
            for (uint32_t i = 0; i < source.children.count; ++i)
                children.push(Node(source.children.data[i], depth + 1));
            value.set("children", std::move(children));
        }
        return value;
    }
};
} // namespace
PulseStatus ConvertUiModel(const PulseUiModelTree *input, UiTree &output, std::set<std::string> &commands,
                           std::string &error) {
    error.clear();
    try {
        Builder builder;
        if (!input || input->size != sizeof(*input) || !input->root || input->command_count > 128 ||
            (input->command_count && !input->commands) || input->action_count > 32 ||
            (input->action_count && !input->actions))
            builder.Fail("invalid native UI tree");
        std::vector<std::string> command_ids;
        std::set<std::string> unique_commands;
        for (uint32_t i = 0; i < input->command_count; ++i) {
            auto id = builder.Text(input->commands[i]);
            if (id.empty() || id.size() > 64 ||
                !std::all_of(id.begin(), id.end(),
                             [](char c) {
                                 return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                                        c == '.' || c == '_' || c == '-';
                             }) ||
                !unique_commands.insert(id).second)
                builder.Fail("invalid or duplicate callback command ID");
            command_ids.push_back(std::move(id));
        }
        auto document = json::Value::Object();
        document.set("version", 1);
        document.set("state", builder.States(input->states, input->state_count));
        document.set("root", builder.Node(*input->root, 1));
        if (input->action_count) {
            auto actions = json::Value::Array();
            for (uint32_t i = 0; i < input->action_count; ++i) {
                const auto &source = input->actions[i];
                if (source.size != sizeof(source))
                    builder.Fail("invalid action size");
                if (source.data_operation)
                    builder.Fail("native UI data operations are not connected", PULSE_ERROR_UNAVAILABLE);
                auto action = json::Value::Object();
                action.set("id", builder.Text(source.id));
                action.set("updates", builder.States(source.updates, source.update_count));
                builder.Strings(action, "validate", source.validate, 128);
                actions.push(std::move(action));
            }
            document.set("actions", std::move(actions));
        }
        UiTree validated;
        if (!ParseUiTree(document, command_ids, validated, error))
            return PULSE_ERROR_ARGUMENT;
        output = std::move(validated);
        commands = std::move(unique_commands);
        return PULSE_OK;
    } catch (const Rejected &rejected) {
        error = rejected.message;
        return rejected.status;
    } catch (...) {
        error = "native UI conversion failed";
        return PULSE_ERROR_INITIALIZATION;
    }
}
} // namespace pulse::extensions::native

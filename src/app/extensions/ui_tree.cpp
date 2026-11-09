#include "ui_tree.h"
#include "../../common/json_value.h"
#include <algorithm>
#include <charconv>
#include <functional>
#include <set>
namespace pulse::extensions {
std::string_view UiNodeTypeName(UiNodeType type) {
    constexpr std::string_view names[] = {
        "stack",         "grid",        "group",          "scrollView",  "text",          "button",
        "textInput",     "checkbox",    "select",         "progress",    "divider",       "tabs",
        "tab",           "list",        "switch",         "radio",       "slider",        "segmented",
        "badge",         "tag",         "infoBar",        "splitButton", "menuButton",    "sidebarItem",
        "breadcrumb",    "emptyState",  "virtualList",    "tree",        "dataTable",     "trayCard",
        "commandSearch", "splitPane",   "sidebarSection", "driveItem",   "paneHeader",    "fileRow",
        "stagingTray",   "stagingItem", "statusBar",      "tooltip",     "titleBarButton"};
    static_assert(std::size(names) == static_cast<size_t>(UiNodeType::Count));
    return static_cast<size_t>(type) < std::size(names) ? names[static_cast<size_t>(type)]
                                                        : std::string_view{};
}
namespace {
bool Identifier(const std::string& s) {
    return !s.empty() && s.size() <= 64 && std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
               c == '_' || c == '-';
    });
}
bool Index(const std::string& text, size_t limit) {
    size_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() &&
           text == std::to_string(value) && value < limit;
}
bool Boolean(const std::string& text) { return text == "true" || text == "false"; }
} // namespace
bool ValidateUiInput(const UiNode& node, std::string_view value) {
    if (value.find('\0') != std::string_view::npos) {
        return false;
    }
    if (!node.validation) {
        return true;
    }
    json::Value checked;
    if (!json::Parse(json::Serialize(json::Value(std::string(value))), checked)) {
        return false;
    }
    const size_t length = static_cast<size_t>(
        std::count_if(value.begin(), value.end(), [](unsigned char c) { return (c & 0xC0) != 0x80; }));
    const auto& rule = *node.validation;
    return (!rule.required || length != 0) && length >= static_cast<size_t>(rule.min_length) &&
           length <= static_cast<size_t>(rule.max_length);
}
bool ValidateUiStateValue(const UiTree& tree, std::string_view key, std::string_view candidate) {
    if (candidate.size() > 1024 || candidate.find('\0') != std::string_view::npos ||
        std::none_of(tree.state.begin(), tree.state.end(),
                     [&](const UiStateEntry& entry) { return entry.key == key; })) {
        return false;
    }
    const std::string value(candidate);
    json::Value utf8_check;
    if (!json::Parse(json::Serialize(json::Value(value)), utf8_check)) {
        return false;
    }
    const std::string binding = "state." + std::string(key);
    std::function<bool(const UiNode&)> valid;
    valid = [&](const UiNode& node) {
        const auto type = node.type;
        if (node.data_source) {
            const auto& source = *node.data_source;
            if (source.sort_state == key && value != "nameAsc" && value != "nameDesc") {
                return false;
            }
            if (source.progress_state == key && !Index(value, 101)) {
                return false;
            }
        }
        if ((node.enabled_binding == binding || node.visible_binding == binding) && !Boolean(value)) {
            return false;
        }
        if (type == UiNodeType::Progress && node.binding == binding && !Index(value, 101)) {
            return false;
        }
        if (node.state == key) {
            const bool toggle = type == UiNodeType::Button &&
                                (node.button_kind == "toggle" || node.button_kind == "transparentToggle");
            if ((type == UiNodeType::Checkbox || type == UiNodeType::Switch || type == UiNodeType::Radio ||
                 type == UiNodeType::SidebarSection || type == UiNodeType::StagingTray || toggle) &&
                !Boolean(value)) {
                return false;
            }
            if ((type == UiNodeType::Select || type == UiNodeType::List || type == UiNodeType::Segmented) &&
                !Index(value, node.options.size())) {
                return false;
            }
            if (type == UiNodeType::Tabs && !Index(value, node.children.size())) {
                return false;
            }
            if (type == UiNodeType::Slider && !Index(value, static_cast<size_t>(node.stops))) {
                return false;
            }
            if (type == UiNodeType::SplitPane && !Index(value, 101)) {
                return false;
            }
            if ((type == UiNodeType::VirtualList || type == UiNodeType::Tree ||
                 type == UiNodeType::DataTable) &&
                !node.data_source && !value.empty()) {
                std::function<bool(const std::vector<UiDataRow>&)> contains;
                contains = [&](const std::vector<UiDataRow>& rows) {
                    for (const auto& row : rows) {
                        if (row.id == value) {
                            return row.enabled;
                        }
                        if (contains(row.children)) {
                            return true;
                        }
                    }
                    return false;
                };
                if (!contains(node.rows)) {
                    return false;
                }
            }
        }
        return std::all_of(node.children.begin(), node.children.end(), valid);
    };
    return valid(tree.root);
}
bool ParseUiTree(const json::Value& value, const std::vector<std::string>& commands, UiTree& output,
                 std::string& error) {
    UiTree tree;
    error.clear();
    const auto fail = [&](const std::string& path, const std::string& reason) {
        error = path + ": " + reason;
        return false;
    };
    const auto only = [&](const json::Value& v, const std::set<std::string>& fields,
                          const std::string& path) {
        if (!v.is_object()) {
            return fail(path, "expected object");
        }
        for (const auto& key : v.keys()) {
            if (!fields.contains(key)) {
                return fail(path + "." + key, "unsupported property");
            }
        }
        return true;
    };
    const auto text = [&](const json::Value& v, const char* key, std::string& out, const std::string& path,
                          size_t limit = 1024) {
        const auto* p = v.find(key);
        if (!p) {
            return true;
        }
        if (!p->is_string() || p->as_string().size() > limit ||
            p->as_string().find('\0') != std::string::npos) {
            return fail(path + "." + key, "expected bounded UTF-8 string without NUL");
        }
        out = p->as_string();
        return true;
    };
    const auto boolean = [&](const json::Value& v, const char* key, bool& out, const std::string& path) {
        const auto* p = v.find(key);
        if (!p) {
            return true;
        }
        if (!p->is_bool()) {
            return fail(path + "." + key, "expected boolean");
        }
        out = p->as_bool();
        return true;
    };
    const auto integer = [&](const json::Value& v, const char* key, int& out, int low, int high,
                             const std::string& path) {
        const auto* p = v.find(key);
        if (!p) {
            return true;
        }
        int64_t number = 0;
        if (!p->get_int(number, low, high)) {
            return fail(path + "." + key, "integer outside allowed range");
        }
        out = static_cast<int>(number);
        return true;
    };
    const auto enumeration = [&](const std::string& entry, std::initializer_list<std::string_view> choices,
                                 const std::string& path) {
        return std::find(choices.begin(), choices.end(), entry) != choices.end() ||
               fail(path, "unknown enum value");
    };
    std::set<std::string> allowed_commands(commands.begin(), commands.end());
    const auto command = [&](const std::string& id, const std::string& path) {
        return allowed_commands.contains(id) || fail(path, "unknown command");
    };
    if (!only(value, {"version", "state", "root", "actions"}, "panel.ui") || !value.find("version") ||
        !integer(value, "version", tree.version, 1, 1, "panel.ui")) {
        return fail("panel.ui.version", "expected version 1");
    }
    std::set<std::string> states, ids;
    if (const auto* entries = value.find("state")) {
        if (!entries->is_array() || entries->size() > 32) {
            return fail("panel.ui.state", "expected at most 32 entries");
        }
        for (size_t i = 0; i < entries->size(); ++i) {
            const auto& entry = entries->at(i);
            const auto path = "panel.ui.state[" + std::to_string(i) + "]";
            UiStateEntry state;
            if (!only(entry, {"key", "value"}, path) || !text(entry, "key", state.key, path, 64) ||
                !text(entry, "value", state.value, path)) {
                return false;
            }
            if (!Identifier(state.key) || !states.insert(state.key).second || !entry.find("value")) {
                return fail(path, "invalid or duplicate state entry");
            }
            tree.state.push_back(std::move(state));
        }
    }
    if (const auto* actions = value.find("actions")) {
        if (!actions->is_array() || actions->size() > 32) {
            return fail("panel.ui.actions", "expected at most 32 local actions");
        }
        for (size_t i = 0; i < actions->size(); ++i) {
            const auto& source = actions->at(i);
            const auto path = "panel.ui.actions[" + std::to_string(i) + "]";
            UiStateAction action;
            if (!only(source, {"id", "updates", "dataOperation", "validate"}, path) ||
                !text(source, "id", action.id, path, 64)) {
                return false;
            }
            if (!Identifier(action.id) || !allowed_commands.insert(action.id).second) {
                return fail(path + ".id", "invalid action ID or collision with action/host command");
            }
            const auto* updates = source.find("updates");
            if (updates && (!updates->is_array() || updates->size() == 0 || updates->size() > 32)) {
                return fail(path + ".updates", "expected 1..32 state updates");
            }
            std::set<std::string> updated;
            for (size_t j = 0; j < (updates ? updates->size() : 0); ++j) {
                const auto& entry = updates->at(j);
                const auto update_path = path + ".updates[" + std::to_string(j) + "]";
                UiStateEntry update;
                if (!only(entry, {"key", "value"}, update_path) ||
                    !text(entry, "key", update.key, update_path, 64) ||
                    !text(entry, "value", update.value, update_path)) {
                    return false;
                }
                if (!states.contains(update.key) || !updated.insert(update.key).second ||
                    !entry.find("value")) {
                    return fail(update_path, "update requires a unique declared state key and string value");
                }
                action.updates.push_back(std::move(update));
            }
            if (const auto* operation = source.find("dataOperation")) {
                UiDataOperation data_operation;
                if (!only(*operation, {"target", "operation"}, path + ".dataOperation") ||
                    !text(*operation, "target", data_operation.target, path + ".dataOperation", 64) ||
                    !text(*operation, "operation", data_operation.operation, path + ".dataOperation", 32)) {
                    return false;
                }
                if (!Identifier(data_operation.target) ||
                    !enumeration(data_operation.operation, {"refresh", "nextPage", "cancel", "retry"},
                                 path + ".dataOperation.operation")) {
                    return fail(path + ".dataOperation", "invalid target or operation");
                }
                action.data_operation = std::move(data_operation);
            }
            if (action.updates.empty() && !action.data_operation) {
                return fail(path, "action requires updates or dataOperation");
            }
            if (const auto* nodes = source.find("validate")) {
                if (!nodes->is_array() || nodes->size() == 0 || nodes->size() > 32) {
                    return fail(path + ".validate", "expected 1..32 unique input IDs");
                }
                std::set<std::string> names;
                for (const auto& node : nodes->items()) {
                    if (!node.is_string() || !Identifier(node.as_string()) ||
                        !names.insert(node.as_string()).second) {
                        return fail(path + ".validate", "invalid or duplicate input ID");
                    }
                    action.validate.push_back(node.as_string());
                }
            }
            tree.actions.push_back(std::move(action));
        }
    }
    const auto initial = [&](const std::string& key) -> std::string {
        for (const auto& entry : tree.state) {
            if (entry.key == key) {
                return entry.value;
            }
        }
        return {};
    };
    size_t menu_count = 0;
    std::set<std::string> menu_ids;
    std::function<bool(const json::Value&, UiMenuItem&, const std::string&, size_t, bool)> parse_menu;
    parse_menu = [&](const json::Value& v, UiMenuItem& item, const std::string& path, size_t depth,
                     bool action) {
        if (++menu_count > 128 || depth > 4) {
            return fail(path, "menu exceeds 128 items or depth 4");
        }
        if (!only(
                v,
                {"id", "text", "command", "glyph", "shortcut", "enabled", "checked", "separator", "children"},
                path)) {
            return false;
        }
        for (auto pair : {std::pair{"id", &item.id},
                          {"text", &item.text},
                          {"command", &item.command},
                          {"glyph", &item.glyph},
                          {"shortcut", &item.shortcut}}) {
            if (!text(v, pair.first, *pair.second, path, 128)) {
                return false;
            }
        }
        for (auto pair : {std::pair{"enabled", &item.enabled},
                          {"checked", &item.checked},
                          {"separator", &item.separator}}) {
            if (!boolean(v, pair.first, *pair.second, path)) {
                return false;
            }
        }
        if (!Identifier(item.id) || !menu_ids.insert(item.id).second) {
            return fail(path + ".id", "invalid or duplicate menu ID");
        }
        if (item.text.empty()) {
            return fail(path + ".text", "label required");
        }
        if (const auto* children = v.find("children")) {
            if (action || !children->is_array() || children->size() == 0) {
                return fail(path + ".children", "expected nonempty submenu");
            }
            for (size_t i = 0; i < children->size(); ++i) {
                UiMenuItem child;
                if (!parse_menu(children->at(i), child, path + ".children[" + std::to_string(i) + "]",
                                depth + 1, false)) {
                    return false;
                }
                item.children.push_back(std::move(child));
            }
        }
        if (!item.children.empty()) {
            if (!item.command.empty()) {
                return fail(path, "submenu cannot have command");
            }
        } else if (!command(item.command, path + ".command")) {
            return false;
        }
        if (action && (v.find("checked") || v.find("separator") || v.find("glyph") || v.find("shortcut"))) {
            return fail(path, "infoBar actions only support id/text/command/enabled");
        }
        return true;
    };
    size_t node_count = 0;
    std::function<bool(const json::Value&, UiNode&, size_t, const std::string&, bool, bool)> parse;
    parse = [&](const json::Value& v, UiNode& n, size_t depth, const std::string& path, bool tab_parent,
                bool stack_parent) {
        if (++node_count > 128 || depth > 8) {
            return fail(path, "tree exceeds 128 nodes or depth 8");
        }
        bool found = false;
        for (int i = 0; i < static_cast<int>(UiNodeType::Count); ++i) {
            if (v.get_string("type") == UiNodeTypeName(static_cast<UiNodeType>(i))) {
                n.type = static_cast<UiNodeType>(i);
                found = true;
                break;
            }
        }
        if (!found) {
            return fail(path + ".type", "unknown node type");
        }
        const auto t = n.type;
        const bool container = t == UiNodeType::Stack || t == UiNodeType::Grid || t == UiNodeType::Group ||
                               t == UiNodeType::ScrollView || t == UiNodeType::Tabs || t == UiNodeType::Tab ||
                               t == UiNodeType::SplitPane || t == UiNodeType::SidebarSection ||
                               t == UiNodeType::StagingTray;
        const bool data = t == UiNodeType::VirtualList || t == UiNodeType::Tree || t == UiNodeType::DataTable;
        const bool check = t == UiNodeType::Checkbox || t == UiNodeType::Switch || t == UiNodeType::Radio;
        const bool choice = t == UiNodeType::Select || t == UiNodeType::List || t == UiNodeType::Segmented;
        if ((t == UiNodeType::Tab) != tab_parent) {
            return fail(path, "tab must be a child of tabs; tabs children must be tab");
        }
        if (t == UiNodeType::ScrollView && depth != 1) {
            n.min_height = 240;
        }
        std::set<std::string> fields = {
            "type",     "id",       "enabled",   "visible", "enabledBinding", "visibleBinding", "minHeight",
            "minWidth", "maxWidth", "maxHeight", "flex"};
        const auto allow = [&](std::initializer_list<const char*> extra) {
            for (auto key : extra) {
                fields.insert(key);
            }
        };
        if (container) {
            allow({"children", "gap"});
        }
        if (t == UiNodeType::Stack) {
            allow({"orientation"});
        }
        if (t == UiNodeType::Grid) {
            allow({"columns"});
        }
        if ((!container && t != UiNodeType::Divider) || t == UiNodeType::Group || t == UiNodeType::Tab ||
            t == UiNodeType::SidebarSection || t == UiNodeType::StagingTray) {
            allow({"text"});
        }
        if (t == UiNodeType::Text) {
            allow({"binding", "role", "alignment"});
        }
        if (t == UiNodeType::Button) {
            allow({"command", "kind", "state", "glyph", "keycap", "iconOnly"});
        }
        if (t == UiNodeType::TextInput) {
            allow({"state", "placeholder", "glyph", "trailingGlyph", "keycap", "badgeText"});
        }
        if (check) {
            allow({"state", "checked"});
        }
        if (choice) {
            allow({"state", "options"});
        }
        if (t == UiNodeType::Tabs) {
            allow({"state"});
        }
        if (t == UiNodeType::Tab) {
            allow({"glyph", "closeCommand"});
        }
        if (t == UiNodeType::Progress) {
            allow({"value", "binding", "kind", "status"});
        }
        if (t == UiNodeType::Slider) {
            allow({"state", "stops", "value"});
        }
        if (t == UiNodeType::Badge) {
            allow({"kind", "dot"});
        }
        if (t == UiNodeType::Tag) {
            allow({"color", "bordered"});
        }
        if (t == UiNodeType::InfoBar) {
            allow({"kind", "message", "glyph", "closeCommand", "actions"});
        }
        if (t == UiNodeType::SplitButton) {
            allow({"command", "menu", "glyph"});
        }
        if (t == UiNodeType::MenuButton) {
            allow({"menu", "glyph"});
        }
        if (t == UiNodeType::SidebarItem) {
            allow({"command", "glyph", "detail", "badgeText", "selected"});
        }
        if (t == UiNodeType::Breadcrumb) {
            allow({"command", "glyph", "current", "separator"});
        }
        if (t == UiNodeType::EmptyState) {
            allow({"glyph", "message"});
        }
        if (data) {
            allow({"rows", "state", "dataSource"});
            n.min_height = 240;
        }
        if (t == UiNodeType::DataTable) {
            allow({"columns"});
        }
        if (t == UiNodeType::TrayCard) {
            allow({"detail", "itemCount", "moveIntent", "command"});
        }
        if (t == UiNodeType::CommandSearch) {
            allow({"placeholder", "keycap", "state"});
        }
        if (t == UiNodeType::SplitPane) {
            allow({"orientation", "state", "value"});
            n.value = 50;
            n.min_height = 240;
        }
        if (t == UiNodeType::SidebarSection) {
            allow({"glyph", "expanded", "state"});
        }
        if (t == UiNodeType::DriveItem) {
            allow({"detail", "glyph", "capacity", "freeBytes", "totalBytes", "command"});
        }
        if (t == UiNodeType::PaneHeader) {
            allow({"summary", "filterText", "filterCommand", "sortCommand"});
        }
        if (t == UiNodeType::FileRow) {
            allow({"modified", "fileType", "fileSize", "glyph", "status", "kind", "tagColors", "cut",
                   "command"});
        }
        if (t == UiNodeType::StagingTray) {
            allow({"helper", "countLabel", "actionText", "itemCount", "expanded", "state", "command"});
        }
        if (t == UiNodeType::StagingItem) {
            allow({"missing", "command"});
        }
        if (t == UiNodeType::StatusBar) {
            allow({"centerText", "rightText", "ready"});
        }
        if (t == UiNodeType::TitleBarButton) {
            allow({"role", "command", "glyph"});
        }
        if (t == UiNodeType::TextInput || t == UiNodeType::CommandSearch) {
            allow({"validation"});
        }
        if (!only(v, fields, path)) {
            return false;
        }
        for (auto pair : {std::pair{"id", &n.id},
                          {"text", &n.text},
                          {"command", &n.command},
                          {"binding", &n.binding},
                          {"state", &n.state},
                          {"orientation", &n.orientation},
                          {"placeholder", &n.placeholder},
                          {"glyph", &n.glyph},
                          {"keycap", &n.keycap},
                          {"detail", &n.detail},
                          {"message", &n.message},
                          {"color", &n.color},
                          {"closeCommand", &n.close_command},
                          {"enabledBinding", &n.enabled_binding},
                          {"visibleBinding", &n.visible_binding},
                          {"role", t == UiNodeType::TitleBarButton ? &n.title_bar_role : &n.text_role},
                          {"alignment", &n.alignment},
                          {"badgeText", &n.badge_text},
                          {"status", t == UiNodeType::FileRow ? &n.status_text : &n.progress_state},
                          {"trailingGlyph", &n.trailing_glyph},
                          {"summary", &n.summary},
                          {"filterText", &n.filter_text},
                          {"filterCommand", &n.filter_command},
                          {"sortCommand", &n.sort_command},
                          {"modified", &n.modified},
                          {"fileType", &n.file_type},
                          {"fileSize", &n.file_size},
                          {"helper", &n.helper},
                          {"countLabel", &n.count_label},
                          {"actionText", &n.action_text},
                          {"centerText", &n.center_text},
                          {"rightText", &n.right_text}}) {
            if (!text(v, pair.first, *pair.second, path)) {
                return false;
            }
        }
        auto& kind = t == UiNodeType::Button                                ? n.button_kind
                     : (t == UiNodeType::Badge || t == UiNodeType::FileRow) ? n.badge_kind
                     : t == UiNodeType::InfoBar                             ? n.info_kind
                                                                            : n.progress_kind;
        if (!text(v, "kind", kind, path)) {
            return false;
        }
        if (!Identifier(n.id) || !ids.insert(n.id).second) {
            return fail(path + ".id", "invalid or duplicate node ID");
        }
        if (v.find("state") && !states.contains(n.state)) {
            return fail(path + ".state", "undeclared state");
        }
        if ((v.find("enabledBinding") && n.enabled_binding.empty()) ||
            (v.find("visibleBinding") && n.visible_binding.empty()) ||
            (v.find("binding") && n.binding.empty())) {
            return fail(path, "binding must not be empty");
        }
        for (const auto& binding : {n.enabled_binding, n.visible_binding}) {
            if (!binding.empty() && (!binding.starts_with("state.") || !states.contains(binding.substr(6)) ||
                                     !Boolean(initial(binding.substr(6))))) {
                return fail(path, "visibility/enabled binding must reference declared boolean state");
            }
        }
        if (!n.binding.empty() &&
            !(n.binding.starts_with("state.") && states.contains(n.binding.substr(6))) &&
            n.binding != "context.directory" && n.binding != "context.selectionCount" &&
            n.binding != "context.selectionName" && n.binding != "context.viewMode" &&
            n.binding != "context.transferStatus") {
            return fail(path + ".binding", "unknown binding");
        }
        for (auto pair : {std::pair{"checked", &n.checked},
                          {"enabled", &n.enabled},
                          {"visible", &n.visible},
                          {"iconOnly", &n.icon_only},
                          {"bordered", &n.bordered},
                          {"selected", &n.selected},
                          {"current", &n.current},
                          {"separator", &n.separator},
                          {"dot", &n.dot},
                          {"moveIntent", &n.move_intent},
                          {"expanded", &n.expanded},
                          {"cut", &n.cut},
                          {"missing", &n.missing},
                          {"ready", &n.ready}}) {
            if (!boolean(v, pair.first, *pair.second, path)) {
                return false;
            }
        }
        if (!integer(v, "gap", n.gap, 0, 32, path) || !integer(v, "minHeight", n.min_height, 0, 4096, path) ||
            !integer(v, "stops", n.stops, 2, 64, path) ||
            !integer(v, "value", n.value, 0, t == UiNodeType::Slider ? n.stops - 1 : 100, path)) {
            return false;
        }
        if (!integer(v, "minWidth", n.min_width, 0, 4096, path) ||
            !integer(v, "maxWidth", n.max_width, 0, 4096, path) ||
            !integer(v, "maxHeight", n.max_height, 0, 4096, path) ||
            !integer(v, "flex", n.flex, 0, 16, path)) {
            return false;
        }
        if ((n.max_width && n.min_width > n.max_width) || (n.max_height && n.min_height > n.max_height)) {
            return fail(path, "minimum size exceeds bounded maximum");
        }
        if (container && t != UiNodeType::ScrollView && n.max_height > 0) {
            return fail(path + ".maxHeight", "bounded container height requires scrollView");
        }
        if (v.find("flex") && !stack_parent) {
            return fail(path + ".flex", "flex is only valid on a direct stack child");
        }
        if (!integer(v, "itemCount", n.item_count, 0, 1000000, path) ||
            !integer(v, "capacity", n.capacity, 0, 100, path)) {
            return false;
        }
        for (auto pair : {std::pair{"freeBytes", &n.free_bytes}, {"totalBytes", &n.total_bytes}}) {
            if (const auto* number = v.find(pair.first)) {
                if (!number->get_int(*pair.second, 0, int64_t{1} << 53)) {
                    return fail(path + "." + pair.first, "expected exact nonnegative byte count");
                }
            }
        }
        if (n.total_bytes > 0 && n.free_bytes > n.total_bytes) {
            return fail(path + ".freeBytes", "free bytes exceeds total bytes");
        }
        if (!enumeration(n.title_bar_role, {"custom", "minimize", "maximize", "restore", "close"},
                         path + ".role")) {
            return false;
        }
        for (const auto& id : {n.filter_command, n.sort_command}) {
            if (!id.empty() && !command(id, path)) {
                return false;
            }
        }
        if (const auto* colors = v.find("tagColors")) {
            if (!colors->is_array() || colors->size() > 3) {
                return fail(path + ".tagColors", "expected at most 3 colors");
            }
            for (const auto& c : colors->items()) {
                const auto str = c.as_string();
                if (!c.is_string() || str.size() != 7 || str[0] != '#' ||
                    !std::all_of(str.begin() + 1, str.end(), [](char ch) {
                        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
                               (ch >= 'A' && ch <= 'F');
                    })) {
                    return fail(path + ".tagColors", "expected #RRGGBB");
                }
                n.tag_colors.push_back(str);
            }
        }
        if (t == UiNodeType::Grid && !integer(v, "columns", n.columns, 1, 4, path)) {
            return false;
        }
        if (!enumeration(n.orientation, {"vertical", "horizontal"}, path + ".orientation") ||
            !enumeration(n.button_kind,
                         {"standard", "primary", "danger", "transparent", "toggle", "transparentToggle"},
                         path + ".kind") ||
            !enumeration(n.progress_kind, {"bar", "ring"}, path + ".kind") ||
            !enumeration(n.progress_state, {"normal", "paused", "error", "indeterminate"},
                         path + ".status") ||
            !enumeration(n.text_role, {"body", "caption", "section"}, path + ".role") ||
            !enumeration(n.alignment, {"left", "center", "right"}, path + ".alignment") ||
            !enumeration(n.badge_kind, {"neutral", "accent", "success", "warning", "danger", "keycap"},
                         path + ".kind") ||
            !enumeration(n.info_kind, {"informational", "success", "warning", "error", "neutral"},
                         path + ".kind")) {
            return false;
        }
        if (!n.color.empty() && (n.color.size() != 7 || n.color[0] != '#' ||
                                 !std::all_of(n.color.begin() + 1, n.color.end(), [](char c) {
                                     return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                                            (c >= 'A' && c <= 'F');
                                 }))) {
            return fail(path + ".color", "expected #RRGGBB");
        }
        if (const auto* options = v.find("options")) {
            if (!options->is_array() || options->size() == 0 || options->size() > 64) {
                return fail(path + ".options", "expected 1..64 strings");
            }
            for (const auto& item : options->items()) {
                if (!item.is_string() || item.as_string().size() > 128 ||
                    item.as_string().find('\0') != std::string::npos) {
                    return fail(path + ".options", "invalid option");
                }
                n.options.push_back(item.as_string());
            }
        }
        if (choice && n.options.empty()) {
            return fail(path + ".options", "required");
        }
        if (const auto* children = v.find("children")) {
            if (!children->is_array()) {
                return fail(path + ".children", "expected array");
            }
            for (size_t i = 0; i < children->size(); ++i) {
                UiNode child;
                if (!parse(children->at(i), child, depth + 1, path + ".children[" + std::to_string(i) + "]",
                           t == UiNodeType::Tabs, t == UiNodeType::Stack)) {
                    return false;
                }
                n.children.push_back(std::move(child));
            }
        }
        if (t == UiNodeType::Tabs && n.children.empty()) {
            return fail(path + ".children", "tabs require children");
        }
        if (t == UiNodeType::TextInput && n.text.empty()) {
            return fail(path + ".text", "accessible label required");
        }
        if (t == UiNodeType::SplitPane && n.children.size() != 2) {
            return fail(path + ".children", "splitPane requires exactly two children");
        }
        if (t == UiNodeType::StagingTray && !n.action_text.empty() && n.command.empty()) {
            return fail(path + ".command", "actionText requires command");
        }
        const bool toggle =
            t == UiNodeType::Button && (n.button_kind == "toggle" || n.button_kind == "transparentToggle");
        if (t == UiNodeType::Button && !toggle && v.find("state")) {
            return fail(path + ".state", "state only applies to toggle buttons");
        }
        if (toggle && n.state.empty()) {
            return fail(path + ".state", "toggle requires boolean state");
        }
        if ((t == UiNodeType::Button && !toggle) || t == UiNodeType::SplitButton ||
            t == UiNodeType::TitleBarButton) {
            if (!command(n.command, path + ".command")) {
                return false;
            }
        } else if (!n.command.empty() && !command(n.command, path + ".command")) {
            return false;
        }
        if (!n.close_command.empty() && !command(n.close_command, path + ".closeCommand")) {
            return false;
        }
        if (n.icon_only && n.glyph.empty()) {
            return fail(path + ".glyph", "iconOnly requires glyph");
        }
        for (auto pair : {std::pair{"menu", &n.menu}, {"actions", &n.actions}}) {
            if (const auto* items = v.find(pair.first)) {
                bool action = std::string_view(pair.first) == "actions";
                if (!items->is_array() || items->size() == 0 || items->size() > (action ? 2u : 64u)) {
                    return fail(path + "." + pair.first, "invalid item count");
                }
                for (size_t i = 0; i < items->size(); ++i) {
                    UiMenuItem item;
                    if (!parse_menu(items->at(i), item,
                                    path + "." + pair.first + "[" + std::to_string(i) + "]", 1, action)) {
                        return false;
                    }
                    pair.second->push_back(std::move(item));
                }
            }
        }
        if ((t == UiNodeType::MenuButton || t == UiNodeType::SplitButton) && n.menu.empty()) {
            return fail(path + ".menu", "required");
        }
        if (const auto* rule = v.find("validation")) {
            UiValidation validation;
            if (!only(*rule, {"required", "minLength", "maxLength", "message"}, path + ".validation") ||
                !boolean(*rule, "required", validation.required, path + ".validation") ||
                !integer(*rule, "minLength", validation.min_length, 0, 1024, path + ".validation") ||
                !integer(*rule, "maxLength", validation.max_length, 0, 1024, path + ".validation") ||
                !text(*rule, "message", validation.message, path + ".validation")) {
                return false;
            }
            if (validation.message.empty() || validation.min_length > validation.max_length ||
                (validation.required && validation.max_length == 0) || n.state.empty()) {
                return fail(path + ".validation",
                            "requires state, nonempty message and satisfiable length bounds");
            }
            n.validation = std::move(validation);
        }
        if (const auto* spec = v.find("dataSource")) {
            UiDataSource source;
            if (v.find("rows")) {
                return fail(path, "rows and dataSource are mutually exclusive");
            }
            if (!only(*spec,
                      {"source", "pageSize", "filterState", "sortState", "statusState", "errorState",
                       "progressState"},
                      path + ".dataSource") ||
                !text(*spec, "source", source.source, path + ".dataSource") ||
                !integer(*spec, "pageSize", source.page_size, 1, 256, path + ".dataSource")) {
                return false;
            }
            if (!spec->find("source") ||
                (source.source != "context.directoryEntries" && source.source != "context.transferTask" && source.source != "context.workflow")) {
                return fail(path + ".dataSource.source", "unsupported memory snapshot source");
            }
            if (source.source == "context.transferTask" || source.source == "context.workflow") {
                if (spec->find("filterState") || spec->find("sortState")) {
                    return fail(path + ".dataSource", "transferTask does not support filtering or sorting");
                }
                if (!spec->find("pageSize")) {
                    source.page_size = 1;
                }
                if (source.page_size != 1) {
                    return fail(path + ".dataSource.pageSize", "transferTask pageSize must be 1");
                }
            }
            for (auto pair : {std::pair{"filterState", &source.filter_state},
                              {"sortState", &source.sort_state},
                              {"statusState", &source.status_state},
                              {"errorState", &source.error_state},
                              {"progressState", &source.progress_state}}) {
                if (!text(*spec, pair.first, *pair.second, path + ".dataSource", 64)) {
                    return false;
                }
                if (spec->find(pair.first) && !states.contains(*pair.second)) {
                    return fail(path + ".dataSource." + pair.first, "undeclared state key");
                }
            }
            if (!source.sort_state.empty() && initial(source.sort_state) != "nameAsc" &&
                initial(source.sort_state) != "nameDesc") {
                return fail(path + ".dataSource.sortState", "initial sort must be nameAsc or nameDesc");
            }
            if (!source.progress_state.empty() && !Index(initial(source.progress_state), 101)) {
                return fail(path + ".dataSource.progressState", "initial progress must be canonical 0..100");
            }
            std::set<std::string> output_keys;
            for (const auto& key : {source.status_state, source.error_state, source.progress_state}) {
                if (!key.empty() && (!output_keys.insert(key).second || key == source.filter_state ||
                                     key == source.sort_state || key == n.state)) {
                    return fail(path + ".dataSource",
                                "output states must be distinct from each other and input/selection states");
                }
            }
            n.data_source = std::move(source);
        }
        std::set<std::string> row_ids, enabled_row_ids;
        if (data) {
            if (t == UiNodeType::DataTable) {
                const auto* columns = v.find("columns");
                if (!columns || !columns->is_array() || columns->size() == 0 || columns->size() > 16) {
                    return fail(path + ".columns", "expected 1..16 columns");
                }
                std::set<std::string> column_ids;
                for (size_t i = 0; i < columns->size(); ++i) {
                    const auto& c = columns->at(i);
                    UiDataColumn col;
                    const auto cp = path + ".columns[" + std::to_string(i) + "]";
                    if (!only(c, {"id", "text", "width"}, cp) || !text(c, "id", col.id, cp, 64) ||
                        !text(c, "text", col.text, cp, 128) || !integer(c, "width", col.width, 48, 800, cp)) {
                        return false;
                    }
                    if (!Identifier(col.id) || !column_ids.insert(col.id).second || col.text.empty()) {
                        return fail(cp, "invalid column ID or label");
                    }
                    n.data_columns.push_back(std::move(col));
                }
            }
            size_t count = 0;
            std::function<bool(const json::Value&, UiDataRow&, const std::string&, size_t)> row;
            row = [&](const json::Value& rv, UiDataRow& r, const std::string& rp, size_t d) {
                if (++count > 4096 || d > 8) {
                    return fail(rp, "data exceeds row/depth budget");
                }
                std::set<std::string> keys = {"id", "text", "glyph", "command", "enabled"};
                if (t == UiNodeType::Tree) {
                    keys.insert("children");
                    keys.insert("expanded");
                }
                if (t == UiNodeType::DataTable) {
                    keys.insert("cells");
                }
                if (!only(rv, keys, rp)) {
                    return false;
                }
                for (auto pair : {std::pair{"id", &r.id},
                                  {"text", &r.text},
                                  {"glyph", &r.glyph},
                                  {"command", &r.command}}) {
                    if (!text(rv, pair.first, *pair.second, rp)) {
                        return false;
                    }
                }
                if (!Identifier(r.id) || !row_ids.insert(r.id).second) {
                    return fail(rp + ".id", "invalid or duplicate row ID");
                }
                if (!boolean(rv, "enabled", r.enabled, rp) || !boolean(rv, "expanded", r.expanded, rp)) {
                    return false;
                }
                if (r.enabled) {
                    enabled_row_ids.insert(r.id);
                }
                if (!r.command.empty() && !command(r.command, rp + ".command")) {
                    return false;
                }
                if (const auto* cells = rv.find("cells")) {
                    if (!cells->is_array() || cells->size() != n.data_columns.size()) {
                        return fail(rp + ".cells", "cells must match column count");
                    }
                    for (const auto& cell : cells->items()) {
                        if (!cell.is_string() || cell.as_string().size() > 1024 ||
                            cell.as_string().find('\0') != std::string::npos) {
                            return fail(rp + ".cells", "invalid cell");
                        }
                        r.cells.push_back(cell.as_string());
                    }
                }
                if (t == UiNodeType::DataTable && !rv.find("cells")) {
                    return fail(rp + ".cells", "required");
                }
                if (const auto* children = rv.find("children")) {
                    if (!children->is_array()) {
                        return fail(rp + ".children", "expected array");
                    }
                    for (size_t i = 0; i < children->size(); ++i) {
                        UiDataRow child;
                        if (!row(children->at(i), child, rp + ".children[" + std::to_string(i) + "]",
                                 d + 1)) {
                            return false;
                        }
                        r.children.push_back(std::move(child));
                    }
                }
                return true;
            };
            const auto* rows = v.find("rows");
            if ((!rows && !n.data_source) || (rows && !rows->is_array())) {
                return fail(path + ".rows", "required array");
            }
            for (size_t i = 0; i < (rows ? rows->size() : 0); ++i) {
                UiDataRow r;
                if (!row(rows->at(i), r, path + ".rows[" + std::to_string(i) + "]", 1)) {
                    return false;
                }
                n.rows.push_back(std::move(r));
            }
        }
        if (!n.state.empty()) {
            const auto content = initial(n.state);
            if ((check || toggle || t == UiNodeType::SidebarSection || t == UiNodeType::StagingTray) &&
                !Boolean(content)) {
                return fail(path + ".state", "expected boolean text");
            }
            if ((choice || t == UiNodeType::Tabs || t == UiNodeType::Slider) &&
                !Index(content, t == UiNodeType::Tabs     ? n.children.size()
                                : t == UiNodeType::Slider ? static_cast<size_t>(n.stops)
                                                          : n.options.size())) {
                return fail(path + ".state", "expected canonical valid index");
            }
            if (t == UiNodeType::SplitPane && !Index(content, 101)) {
                return fail(path + ".state", "splitPane state must be canonical percentage 0..100");
            }
            if (data && !content.empty() && !enabled_row_ids.contains(content)) {
                return fail(path + ".state", "unknown selected row ID");
            }
        }
        if (t == UiNodeType::Progress && !n.binding.empty()) {
            if (n.binding.starts_with("state.")) {
                if (!Index(initial(n.binding.substr(6)), 101)) {
                    return fail(path + ".binding", "progress requires canonical integer 0..100");
                }
            } else if (n.binding != "context.selectionCount") {
                return fail(path + ".binding", "progress requires numeric binding");
            }
        }
        return true;
    };
    const auto* root = value.find("root");
    if (!root) {
        return fail("panel.ui.root", "required");
    }
    if (!parse(*root, tree.root, 1, "panel.ui.root", false, false)) {
        return false;
    }
    std::function<const UiNode*(const UiNode&, const std::string&)> find_node;
    find_node = [&](const UiNode& node, const std::string& id) -> const UiNode* {
        if (node.id == id) {
            return &node;
        }
        for (const auto& child : node.children) {
            if (const auto* found = find_node(child, id)) {
                return found;
            }
        }
        return nullptr;
    };
    std::function<bool(const UiNode&)> validate_source_outputs;
    validate_source_outputs = [&](const UiNode& node) {
        if (node.data_source) {
            const auto& source = *node.data_source;
            for (const auto status : {"idle", "loading", "ready", "error", "cancelled"}) {
                if (!source.status_state.empty() &&
                    !ValidateUiStateValue(tree, source.status_state, status)) {
                    return fail("panel.ui.dataSource.statusState",
                                "output conflicts with bound control state constraints");
                }
            }
            for (const auto progress : {"0", "100"}) {
                if (!source.progress_state.empty() &&
                    !ValidateUiStateValue(tree, source.progress_state, progress)) {
                    return fail("panel.ui.dataSource.progressState",
                                "output conflicts with bound control state constraints");
                }
            }
            for (const auto message : {"", "Error"}) {
                if (!source.error_state.empty() && !ValidateUiStateValue(tree, source.error_state, message)) {
                    return fail("panel.ui.dataSource.errorState",
                                "output conflicts with bound control state constraints");
                }
            }
        }
        return std::all_of(node.children.begin(), node.children.end(), validate_source_outputs);
    };
    if (!validate_source_outputs(tree.root)) {
        return false;
    }
    for (size_t i = 0; i < tree.actions.size(); ++i) {
        const auto& action = tree.actions[i];
        if (action.data_operation) {
            const auto* target = find_node(tree.root, action.data_operation->target);
            if (!target || !target->data_source) {
                return fail("panel.ui.actions[" + std::to_string(i) + "].dataOperation.target",
                            "target must have dataSource");
            }
        }
        for (const auto& id : action.validate) {
            const auto* input = find_node(tree.root, id);
            if (!input || !input->validation) {
                return fail("panel.ui.actions[" + std::to_string(i) + "].validate",
                            "target must be a validated input");
            }
        }
        for (size_t j = 0; j < action.updates.size(); ++j) {
            const auto& update = action.updates[j];
            if (!ValidateUiStateValue(tree, update.key, update.value)) {
                return fail("panel.ui.actions[" + std::to_string(i) + "].updates[" + std::to_string(j) +
                                "].value",
                            "value violates a referenced control state constraint");
            }
        }
    }
    output = std::move(tree);
    return true;
}
} // namespace pulse::extensions

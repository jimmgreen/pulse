#include "mcp_tools.h"
#include "../common/ai_action_request.h"
#include "../app/extensions/developer_tool_catalog.h"
#include <cstring>

namespace pulse::ai_host {
namespace {

constexpr int64_t kMaxPath = 32767;

constexpr int64_t kMaxSteps = 12;
constexpr const char* kStepWaits[] = {"listing_loaded", "op_finished", "preview_open"};

// run_actions' description: what it does, then the common actions and the
// settings, as the assistant's run_actions tells its model. Built from
// pieces: one literal may not pass MSVC's limit.
const char* RunActionsDescription() {
    static const std::string text = std::string(
        "Package-bound workflow.run/workflow.cancel and extension resource APIs are not run_actions IDs. "
        "Discovery and template validation grant no package identity or execution authority; caller, owner, principal and ticket fields are rejected. "
        "Does things in Pulse itself, in order, as one undo group (at most 12 steps): panes and the folders in them, wi"
        "ndows, tabs, view, sort, grouping, panels, selection, the staging tray, tags, favorites, settings, new folders"
        ", opening files, file operations, and any action find_actions returns. What AI apps may do is the user's choic"
        "e in Pulse (Settings > \345\242\236\345\274\272\345\214\205 > AI \344\270\216 MCP > AI \345\272\224"
        "\347\224\250\345\217\257\344\273\245\345\201\232\347\232\204\344\272\213): view steps by default; organize, cr"
        "eate, file steps and effects outside Pulse only when switched on. Pulse checks every step before running any: "
        "when one is not allowed nothing runs and the error not_allowed names the step, the switch and where it is - te"
        "ll the user, do not retry or work around it. View and organize steps (including settings.set) act at once. New"
        " files and folders ask the user in Pulse unless they always allow that folder. File steps (file.copy_to / file"
        ".move_to {items, dest}, file.paste, file.rename {item, name}, file.group_into_folder {items, name}, file.disso"
        "lve {items}, file.recycle {items}, file.delete_permanent {items}, recycle.restore, recycle.empty) and effects "
        "outside Pulse (opening a program or script, git.push / git.pull, shell.run_verb, toolbar.open_with, cloud.keep"
        " / cloud.free, drive.eject) show the user a confirm dialog in Pulse every time, with the plan (what goes where"
        "; a permanent delete takes two clicks); do not ask first yourself. A denial returns user_denied and no answer "
        "within 2 minutes user_timeout: nothing of the call after it runs. One file step per call; steps before it (sel"
        "ect.items) run before the dialog. items: absolute paths, or leave them out for the selection; dest: an absolut"
        "e folder or a known folder, made if missing. Locked actions (quitting Pulse, unlocking a BitLocker drive, the "
        "AI permissions, turning off delete confirmation) are only the user's. Stops at the first step that fails; earl"
        "ier steps stay done. undo_last undoes the whole call. Returns each step's outcome.\n") + std::string(
        "file.organize {moves: [{item, dest}]} sorts many files into folders with one plan dialog (a file step; missi"
        "ng folders are made; any name already taken refuses the whole plan). file.find_duplicates {folders, min_size,"
        " show} is a read step: Pulse reads the files on this PC to compare them byte for byte - their contents never "
        "reach you - skips online-only cloud files, opens a tab with the groups unless show is false, and returns each"
        " group's paths, sizes and dates, the copy it would keep and why (at most 50 groups). The user can move the ex"
        "tras to the Recycle Bin from that tab; file.recycle {items} asks them every time.\n") + std::string(
        "Common actions (args): window.layout {layout: single|side_by_side|stacked|three|four, panes: [a folder per pan"
        "e, left or top first; \"\" leaves one]}, nav.go {path, pane}, window.new {path}, tab.new {path}, tab.close, ta"
        "b.to_window, pane.to_window {pane}, window.merge, window.overview, nav.back, nav.up, view.mode {view: extra_la"
        "rge_icons|large_icons|medium_icons|small_icons|list|details|tiles|content, pane}, view.sort {by: name|modified"
        "|type|size|created, desc, pane}, view.group {by: none|name|date|type|size, pane}, sidebar.toggle {show}, favba"
        "r.toggle {show}, details.toggle {show}, select.items {pane, name, extensions, items, type: folder|document|ima"
        "ge|video|audio|archive|code, modified: today|yesterday|this_week|last_7_days|this_month|last_month|{after, bef"
        "ore}, size: {larger_than, smaller_than}, tag, selected, mode: replace|add|remove} (on a search tab Pulse first"
        " waits for the search to finish, 10 s at most, then picks among all its results; tag does not work on content "
        "search results), select.all, select.clear, tray.add {items}, tag.toggle {tag, items} (a new tag is made), fav."
        "add {path or items}, file.new_folder {name} (in the open folder), nav.open {path} (a file or program), setting"
        "s.set {key, value}, edit.undo, tab.pin {tab, pin}, tab.color {tab, color: red|orange|yellow|green|teal|blue|pu"
        "rple|pink|none}, tab.rename {tab, name}, tab.group {tab, group}, tab.activate {tab}, tag.create {name, color, "
        "group}, keymap.set {action, keys} (keymap.read lists the actions), git.refresh (the open folder's repository: "
        "branch, ahead / behind, changes), git.push, git.pull, shell.verbs (the selection's context menu items), shell."
        "run_verb {verb: an item shell.verbs listed; in a submenu \"Parent \342\226\270 Item\"}, toolbar.open_with {app"
        ": a program pinned to the toolbar}, cloud.keep, cloud.free, preview.cloud_download {path}, drive.eject {drive:"
        " E:}, drive.disconnect {drive}, file.context_menu and toolbar.customize (Pulse opens them; the user chooses). "
        "tab: 1 = first as in get_state, a title, \"all\" or a list - several tabs go in ONE step. Words: \346\240\207"
        "\347\255\276 is a tag (tag.create, then tag.toggle to put it on items; \345\212\240\346\240\207\347\255\276 / "
        "\346\211\223\346\240\207\347\255\276 = tag.toggle), \346\240\207\347\255\276\351\241\265 is a tab (tab.*), "
        "\346\211\230\347\233\230 / \346\232\202\345\255\230 is the staging tray (only when the user says so). pane: 1-"
        "4 in screen order as in get_state, focused, target, or all (view / sort / group). Paths: absolute, a drive (D:"
        ") or a known folder (Downloads, Desktop, Documents, Pictures, Videos, Music). items: full paths; omitted = the"
        " selection.\n") + std::string(
        "Settings (settings.set key and values; switches take true/false): theme light|dark|system, window_effect none|"
        "acrylic|mica|mica_alt, text_rendering auto|sharp|smooth, font_size small|default|large|larger, language system"
        "|zh-CN|zh-TW|en-US, menu_acrylic, row_height compact|standard|roomy, click_to_open double|single, name_highlig"
        "ht hover|always, hover_select, item_checkboxes, blank_double_click nothing|back|up, show_hidden, show_protecte"
        "d, folders_first top|follow|mixed, smart_dates, zebra_rows, size_bars, tag_name_color, selection_outline, thum"
        "bnail_app_badges, alias_mark, tree_sticky, mtime_marks off|dots|bars|text, mtime_range today|3|7|30, mtime_dim_older, mtime"
        "_bold_today, mtime_folders, mtime_other_views, mtime_flash, onedrive_status, svn_status, startup_open last_tab"
        "s|default_location, new_tab_open current|default_location, external_open smart|tab|window, vertical_tabs, pinn"
        "ed_tab_names, double_click_closes_tab, close_window_with_last_tab, launch_at_signin (Windows), start_in_tray, "
        "keep_running, tray_icon always|background|never, explorer_tag_menu (Windows), open_folders_with_pulse (default"
        " file manager; Windows), staging_icon_size small|standard|large, staging_hide_when_empty, change_tracking, cha"
        "nge_days today|3|7, lan_discovery, network_recent, hidden_shares, confirm_delete (can only be turned on), glob"
        "al_search (Windows), pinyin_search. Locked (only the user, in Settings): confirm_delete off, the AI permission"
        "s, always allow rules, connections, the API key.");
    return text.c_str();
}

std::vector<ToolDef> MakeTools() {
    const char* kPathsNote = "Absolute Windows paths, e.g. C:\\Users\\me\\Documents\\report.pdf.";
    std::vector<ToolDef> tools;
    tools.push_back({"get_context", "Current Pulse view",
        "Returns what the user sees in Pulse: the panes of the active tab in screen order (pane 1 is the left "
        "or top one) with each folder, view, filter and selection (up to 200 paths for the focused pane), the "
        "other tabs, the staging tray, the preview and any open dialog. Use it to find out what \"this folder\", "
        "\"these files\" or \"the right pane\" refers to.",
        true, false, true, {}});
    tools.push_back({"list_folder", "List a folder",
        "Lists the items in a folder: name, size, modified time and whether each is a folder. File contents are "
        "never read. Pass the returned cursor to get the next page.",
        true, false, true,
        {{"path", ParamKind::String, true, 1, kMaxPath, "Absolute path of the folder."},
         {"limit", ParamKind::Integer, false, 1, 500, "Items per page (default 100)."},
         {"cursor", ParamKind::String, false, 1, 256, "The cursor from the previous page."}}});
    tools.push_back({"search", "Search files",
        "Finds files and folders by name through Pulse's index, optionally inside one folder, of one type or "
        "modified after a date. Returns paths, names, sizes and modified times; matches file names, not contents.",
        true, false, true,
        {{"query", ParamKind::String, true, 1, 256, "Words to find in names; Pulse's search syntax is accepted."},
         {"folder", ParamKind::String, false, 1, kMaxPath, "Only search inside this folder."},
         {"type", ParamKind::String, false, 1, 16, "Only this kind of item.",
          {"file", "folder", "image", "video", "audio", "document", "archive"}},
         {"modified_after", ParamKind::Date, false, 10, 10, "Only items modified on or after this day (YYYY-MM-DD)."},
         {"limit", ParamKind::Integer, false, 1, 200, "Maximum results (default 50)."}}});
    tools.push_back({"get_properties", "File properties",
        "Returns properties of files and folders: size, times, and where available image dimensions, page "
        "count, installer version and an EXIF summary. File contents are not returned.",
        true, false, true,
        {{"paths", ParamKind::StringArray, true, 1, 50, kPathsNote}}});
    tools.push_back({"reveal", "Show in Pulse",
        "Shows items to the user in Pulse: opens their folder and selects them (one tab per client is reused). "
        "Changes nothing on disk.",
        true, false, true,
        {{"paths", ParamKind::StringArray, true, 1, 200, kPathsNote}}});
    tools.push_back({"create_zip", "Create a zip archive",
        "Packs files and folders into a new .zip archive. Pulse asks the user to confirm first unless they chose "
        "to always allow this for the destination folder; the archive can be undone with undo_last. Existing "
        "files are never overwritten.",
        false, false, false,
        {{"paths", ParamKind::StringArray, true, 1, 1000, kPathsNote},
         {"dest_folder", ParamKind::String, false, 1, kMaxPath,
          "Folder for the archive (default: the folder of the first item)."},
         {"name", ParamKind::String, false, 1, 255, "File name of the archive; .zip is added if missing."}}});
    tools.push_back({"find_actions", "Find Pulse actions",
        "Looks up Pulse's actions (menu commands, panels, view options, settings, file operations) for run_actions. Mat"
        "ches words in Chinese, pinyin or English. Returns up to 8, best first: id, title, args, the current value (now"
        "), whether it can run now (enabled false with why), its risk, the user's shortcut, and allowed: false when the"
        " user has not switched its group on for AI apps in Pulse - then why_not_allowed says which switch and where; t"
        "ell the user instead of calling run_actions. domain settings lists every setting with its key, values and curr"
        "ent value.",
        true, false, true,
        {{"query", ParamKind::String, false, 1, 200, "A few words naming the action, e.g. \350\257\246\346\203\205\351\235\242\346\235\277 (details panel)."},
         {"domain", ParamKind::String, false, 1, 16, "settings: list all settings instead of searching.", {"settings"}}}});
    tools.push_back({"run_actions", "Do things in Pulse",
        RunActionsDescription(),
        false, true, false,
        {{"steps", ParamKind::Steps, true, 1, kMaxSteps, "The steps in order (at most 12)."}}});
    tools.push_back({"undo_last", "Undo the last AI change",
        "Undoes the most recent change an AI client made through Pulse: a zip it created (it goes to the Recycle "
        "Bin) or a whole run_actions call. Refuses when the user's own last operation is on top.",
        false, true, false, {}});
    for (const auto& spec : extensions::kDeveloperTools) {
        ToolDef tool{spec.name, spec.title, spec.description, true, false, true, {}};
        if (spec.parameter) {
            Param parameter{spec.parameter, ParamKind::String, true, spec.minimum, spec.maximum, spec.parameter_description};
            if (spec.template_choices) parameter.choices.assign(extensions::generated::kTemplateNames.begin(),
                extensions::generated::kTemplateNames.end());
            tool.params.push_back(std::move(parameter));
        }
        tools.push_back(std::move(tool));
    }
    return tools;
}

const char* KindType(ParamKind kind) {
    switch (kind) {
        case ParamKind::Integer: return "integer";
        case ParamKind::StringArray:
        case ParamKind::Steps: return "array";
        default: return "string";
    }
}

bool HasNul(const std::string& s) { return s.find('\0') != std::string::npos; }

// Length in UTF-16 code units, the way Windows counts paths.
int64_t Utf16Length(const std::string& s) {
    int64_t n = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) == 0x80) continue;    // continuation byte
        n += c >= 0xF0 ? 2 : 1;              // 4-byte sequences are surrogate pairs
    }
    return n;
}

bool IsDate(const std::string& s) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
    for (size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u})
        if (s[i] < '0' || s[i] > '9') return false;
    const int month = (s[5] - '0') * 10 + (s[6] - '0');
    const int day = (s[8] - '0') * 10 + (s[9] - '0');
    return month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

std::string CheckString(const Param& p, const json::Value& v, const std::string& where) {
    if (!v.is_string()) return where + " must be a string";
    const std::string& s = v.as_string();
    if (HasNul(s)) return where + " must not contain NUL characters";
    const int64_t n = Utf16Length(s);
    if (n < p.minimum) return where + (p.minimum == 1 ? " must not be empty" : " is too short");
    if (n > p.maximum) return where + " is too long (at most " + std::to_string(p.maximum) + " characters)";
    if (p.kind == ParamKind::Date && !IsDate(s)) return where + " must be a date as YYYY-MM-DD";
    if (!p.choices.empty()) {
        bool known = false;
        for (const char* choice : p.choices) known |= s == choice;
        if (!known) {
            std::string list;
            for (const char* choice : p.choices) list += (list.empty() ? "" : ", ") + std::string(choice);
            return where + " must be one of: " + list;
        }
    }
    return {};
}

// run_actions' steps: objects with an action id, maybe args and a wait.
std::string CheckSteps(const Param& p, const json::Value& v, const std::string& where) {
    if (!v.is_array()) return where + " must be an array of steps";
    const auto count = static_cast<int64_t>(v.size());
    if (count < p.minimum) return where + " must not be empty";
    if (count > p.maximum) return where + " has too many steps (at most " + std::to_string(p.maximum) + ")";
    for (size_t i = 0; i < v.size(); ++i) {
        const json::Value& step = v.at(i);
        const std::string at = where + "[" + std::to_string(i) + "]";
        if (!step.is_object()) return at + " must be an object {action, args}";
        for (const std::string& key : step.keys())
            if (key != "action" && key != "args" && key != "wait")
                return at + ": unknown member \"" + key + "\" (put the action's arguments in args)";
        const json::Value* action = step.find("action");
        if (!action) return at + ".action is required";
        const Param id{"action", ParamKind::String, true, 1, 64, ""};
        if (std::string reason = CheckString(id, *action, at + ".action"); !reason.empty()) return reason;
        if (const json::Value* step_args = step.find("args"); step_args && !step_args->is_object())
            return at + ".args must be an object";
        if (const json::Value* wait = step.find("wait")) {
            const Param w{"wait", ParamKind::String, true, 1, 32, "", {kStepWaits[0], kStepWaits[1], kStepWaits[2]}};
            if (std::string reason = CheckString(w, *wait, at + ".wait"); !reason.empty()) return reason;
        }
    }
    return {};
}

} // namespace

const std::vector<ToolDef>& Tools() {
    static const std::vector<ToolDef> tools = MakeTools();
    return tools;
}

const ToolDef* FindTool(std::string_view name) {
    for (const ToolDef& tool : Tools())
        if (name == tool.name) return &tool;
    return nullptr;
}

json::Value InputSchema(const ToolDef& tool) {
    if (const auto* spec = extensions::FindDeveloperTool(tool.name)) return extensions::DeveloperToolInputSchema(*spec);
    json::Value properties = json::Value::Object();
    json::Value required = json::Value::Array();
    for (const Param& p : tool.params) {
        json::Value prop = json::Value::Object();
        prop.set("type", KindType(p.kind));
        prop.set("description", p.description);
        switch (p.kind) {
            case ParamKind::Integer:
                prop.set("minimum", p.minimum);
                prop.set("maximum", p.maximum);
                break;
            case ParamKind::StringArray: {
                json::Value items = json::Value::Object();
                items.set("type", "string");
                items.set("minLength", 1);
                prop.set("items", std::move(items));
                prop.set("minItems", p.minimum);
                prop.set("maxItems", p.maximum);
                break;
            }
            case ParamKind::Steps: {
                json::Value step = json::Value::Object();
                step.set("type", "object");
                json::Value fields = json::Value::Object();
                json::Value action = json::Value::Object();
                action.set("type", "string");
                action.set("description", "An action id, e.g. view.sort (find_actions lists them).");
                fields.set("action", std::move(action));
                json::Value step_args = json::Value::Object();
                step_args.set("type", "object");
                step_args.set("description", "The action's arguments.");
                fields.set("args", std::move(step_args));
                json::Value wait = json::Value::Object();
                wait.set("type", "string");
                json::Value waits = json::Value::Array();
                for (const char* w : kStepWaits) waits.push(w);
                wait.set("enum", std::move(waits));
                wait.set("description", "Wait for this before the step (a selection waits for its folder by itself).");
                fields.set("wait", std::move(wait));
                step.set("properties", std::move(fields));
                json::Value need = json::Value::Array();
                need.push("action");
                step.set("required", std::move(need));
                step.set("additionalProperties", false);
                prop.set("items", std::move(step));
                prop.set("minItems", p.minimum);
                prop.set("maxItems", p.maximum);
                break;
            }
            case ParamKind::Date:
                prop.set("format", "date");
                prop.set("pattern", "^[0-9]{4}-[0-9]{2}-[0-9]{2}$");
                break;
            case ParamKind::String:
                prop.set("minLength", p.minimum);
                prop.set("maxLength", p.maximum);
                if (!p.choices.empty()) {
                    json::Value choices = json::Value::Array();
                    for (const char* choice : p.choices) choices.push(choice);
                    prop.set("enum", std::move(choices));
                }
                break;
        }
        properties.set(p.name, std::move(prop));
        if (p.required) required.push(p.name);
    }
    json::Value schema = json::Value::Object();
    schema.set("type", "object");
    schema.set("properties", std::move(properties));
    if (required.size()) schema.set("required", std::move(required));
    schema.set("additionalProperties", false);
    return schema;
}

json::Value ToolsList() {
    json::Value list = json::Value::Array();
    for (const ToolDef& tool : Tools()) {
        json::Value annotations = json::Value::Object();
        annotations.set("title", tool.title);
        annotations.set("readOnlyHint", tool.read_only);
        annotations.set("destructiveHint", tool.destructive);
        annotations.set("idempotentHint", tool.idempotent);
        annotations.set("openWorldHint", false);
        json::Value t = json::Value::Object();
        t.set("name", tool.name);
        t.set("title", tool.title);
        t.set("description", tool.description);
        t.set("inputSchema", InputSchema(tool));
        t.set("annotations", std::move(annotations));
        list.push(std::move(t));
    }
    json::Value result = json::Value::Object();
    result.set("tools", std::move(list));
    return result;
}

std::string ValidateArguments(const ToolDef& tool, const json::Value& args) {
    if (std::string_view(tool.name) == "run_actions") return pulse::ai::ValidateActionRequest(args);
    if (!args.is_object()) return "arguments must be an object";
    for (const std::string& key : args.keys()) {
        bool known = false;
        for (const Param& p : tool.params) known |= key == p.name;
        if (!known) return "unknown argument \"" + key + "\"";
    }
    for (const Param& p : tool.params) {
        const json::Value* v = args.find(p.name);
        const std::string where = std::string("\"") + p.name + "\"";
        if (!v) {
            if (p.required) return where + " is required";
            continue;
        }
        if (std::string_view(tool.name) == "validate_extension_manifest" && std::string_view(p.name) == "manifest") {
            if (!v->is_string()) return where + " must be a string";
            if (v->as_string().size() > 65536) return where + " is too long (at most 65536 UTF-8 bytes)";
            continue;
        }
        switch (p.kind) {
            case ParamKind::Integer: {
                int64_t n = 0;
                if (!v->get_int(n)) return where + " must be an integer";
                if (n < p.minimum || n > p.maximum)
                    return where + " must be between " + std::to_string(p.minimum) + " and " + std::to_string(p.maximum);
                break;
            }
            case ParamKind::StringArray: {
                if (!v->is_array()) return where + " must be an array of strings";
                const auto count = static_cast<int64_t>(v->size());
                if (count < p.minimum) return where + " must not be empty";
                if (count > p.maximum) return where + " has too many items (at most " + std::to_string(p.maximum) + ")";
                const Param item{p.name, ParamKind::String, true, 1, kMaxPath, ""};
                for (size_t i = 0; i < v->size(); ++i) {
                    std::string reason = CheckString(item, v->at(i), where + "[" + std::to_string(i) + "]");
                    if (!reason.empty()) return reason;
                }
                break;
            }
            case ParamKind::Steps: {
                std::string reason = CheckSteps(p, *v, where);
                if (!reason.empty()) return reason;
                break;
            }
            case ParamKind::String:
            case ParamKind::Date: {
                std::string reason = CheckString(p, *v, where);
                if (!reason.empty()) return reason;
                break;
            }
        }
    }
    return {};
}

} // namespace pulse::ai_host

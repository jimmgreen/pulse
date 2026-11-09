// action_params.cpp - Argument specs and the assistant's action lookup.
#include "action_params.h"

#include "action_files.h"

#include "action_inventory.h"
#include "action_policy.h"
#include "action_settings.h"

#include <algorithm>
#include <cwctype>
#include <iterator>
#include <utility>

namespace pulse::app::actions {

namespace {

constexpr std::pair<std::string_view, std::string_view> kSpecs[] = {
    {"window.arrange", "layout:side_by_side|stacked|columns|tile|main|cascade|main_right|main_top"},
    {"window.layout", "layout:single|side_by_side|stacked|three|four panes:folders"},
    {"nav.go", "path:folder pane:pane"},
    {"window.new", "path:folder"},
    {"tab.new", "path:folder"},
    {"pane.to_window", "pane:pane"},
    {"view.mode", "view:extra_large_icons|large_icons|medium_icons|small_icons|list|details|tiles|content pane:pane"},
    {"view.sort", "by:name|modified|type|size|created desc:bool pane:pane"},
    {"view.group", "by:none|name|date|type|size pane:pane"},
    {"sidebar.toggle", "show:bool"},
    {"favbar.toggle", "show:bool"},
    {"details.toggle", "show:bool"},
    {"view.folder_sizes", "show:bool"},
    {"view.compare", "show:bool"},
    {"select.items", "selector"},
    {"tray.add", "items:paths"},
    {"tag.toggle", "tag:string items:paths"},
    {"fav.add", "path:path items:paths"},
    {"nav.open", "path:path"},
    {"file.new_folder", "name:string"},
    {"settings.set", "key:setting value:any"},
    // P4. "tab": a number (get_state), a title, "all" or a list of these.
    {"tab.pin", "tab:tab pin:bool"},
    {"tab.color", "tab:tab color:red|orange|yellow|green|teal|blue|purple|pink|none"},
    {"tab.rename", "tab:tab name:string"},
    {"tab.group", "tab:tab group:string"},
    {"tab.duplicate", "tab:tab"},
    {"tab.close_others", "tab:tab"},
    {"tab.activate", "tab:tab"},
    {"pane.focus", "pane:pane"},
    {"window.activate", "window:string"},
    {"sidebar.section", "section:workspaces|quick_access|saved_searches|drives|tags|networks open:bool"},
    {"view.icon_size", "size:bigger|smaller|16-512"},
    {"tag.create", "name:string color:red|orange|yellow|green|teal|blue|purple|pink group:string"},
    {"tag.edit", "tag:string name:string color:red|orange|yellow|green|teal|blue|purple|pink"},
    {"tag.delete", "tag:string"},
    {"tag.group", "tag:string group:string"},
    {"smartview.save", "name:string"},
    {"recent.remove", "items:paths"},
    {"alias.set", "path:path alias:string"},
    {"tray.remove", "items:paths|all"},
    {"search.open", "query:string"},
    {"net.place", "path:string name:string pin:bool"},
    {"keymap.read", "action:string"},
    {"keymap.set", "action:string keys:string"},
    // P4b
    {"shell.run_verb", "verb:string"},
    {"toolbar.open_with", "app:string"},
    {"preview.cloud_download", "path:string"},
    {"drive.eject", "drive:string"},
    {"drive.disconnect", "drive:string"},
    // P4c: the file steps (action_files.h). Without items: the selection.
    {"file.copy_to", "items:paths dest:folder"},
    {"file.move_to", "items:paths dest:folder"},
    {"file.rename", "item:path name:string"},
    {"file.group_into_folder", "items:paths name:string"},
    {"file.dissolve", "items:paths"},
    {"file.organize", "moves:moves"},
    {"file.find_duplicates", "folders:folders min_size:bytes show:bool"},
    {"file.recycle", "items:paths"},
    {"file.delete_permanent", "items:paths"},
    // Runners that take no arguments (check_actions.py holds every runner
    // to a spec here; empty is the same as none for find_actions).
    {"index.status", ""},
    {"git.refresh", ""},
    {"git.push", ""},
    {"git.pull", ""},
    {"shell.verbs", ""},
};

std::wstring Fold(std::wstring_view text) {
    std::wstring out(text);
    for (wchar_t& c : out) c = static_cast<wchar_t>(std::towlower(c));
    return out;
}

std::vector<std::wstring> Words(std::wstring_view text) {
    std::vector<std::wstring> out;
    std::wstring word;
    for (const wchar_t c : text) {
        if (std::iswspace(c) || c == L',' || c == L'\x3001' || c == L'\xFF0C') {
            if (!word.empty()) out.push_back(Fold(word));
            word.clear();
        } else {
            word.push_back(c);
        }
    }
    if (!word.empty()) out.push_back(Fold(word));
    return out;
}

std::string Utf8Of(std::wstring_view text) { return json::Utf8(text); }

} // namespace

std::string_view ArgSpec(std::string_view id) noexcept {
    for (const auto& [k, spec] : kSpecs)
        if (k == id) return spec;
    return {};
}

json::Value ArgSpecJson(std::string_view spec) {
    json::Value out = json::Value::Object();
    if (spec == "selector") {
        out.set("pane", "pane");
        out.set("name", "string or strings");
        out.set("extensions", "strings");
        out.set("items", "names");
        out.set("type", "folder|document|image|video|audio|archive|code");
        out.set("modified", "today|yesterday|this_week|last_7_days|this_month|last_month|{after,before}");
        out.set("size", "{larger_than,smaller_than}");
        out.set("tag", "string");
        out.set("selected", "bool");
        out.set("mode", "replace|add|remove");
        return out;
    }
    size_t at = 0;
    while (at < spec.size()) {
        size_t end = spec.find(' ', at);
        if (end == std::string_view::npos) end = spec.size();
        const std::string_view word = spec.substr(at, end - at);
        const size_t colon = word.find(':');
        if (colon == std::string_view::npos) {
            at = end + 1;
            continue;
        }
        std::string type(word.substr(colon + 1));
        // P4: say what a tab may be, so several tabs go in one step.
        if (type == "tab") type = "number (1 = first, as get_state) | title | \"all\" | a list of these - several tabs in ONE step";
        // Task 46: file.organize / file.find_duplicates.
        if (type == "moves")
            type = "[{item, dest}] - item: a path or a name in the open folder; dest: a folder path or a new folder "
                   "name made in the open folder. All moves go in ONE plan the user confirms once";
        if (type == "bytes") type = "number - skip files smaller than this (default 1024)";
        if (word.substr(0, colon) == "folders" && type == "folders")
            type = "folders - empty = the open folder; known names downloads/pictures/desktop/documents also work";
        out.set(word.substr(0, colon), type);
        at = end + 1;
    }
    return out;
}

std::vector<const ActionView*> FindActions(const std::vector<ActionView>& views, std::wstring_view query,
                                           size_t max) {
    const std::vector<std::wstring> words = Words(query);
    struct Hit {
        const ActionView* view;
        int tier;
        size_t order;
    };
    std::vector<Hit> hits;
    for (size_t i = 0; i < views.size(); ++i) {
        const ActionView& v = views[i];
        int best = 0, score = 0;
        for (const std::wstring& w : words) {
            const int t = ActionMatchTier(v, w);
            best = (std::max)(best, t);
            // A longer word says more (文件夹大小 over 列表): weigh by length.
            if (t > 0) score += t * static_cast<int>((std::min)(w.size(), size_t{8}));
        }
        if (!best) continue;
        // The settings rows are many: a lone choice-word match (开) is noise.
        if (!v.key.empty() && best < 2) continue;
        hits.push_back({&v, score, i});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
        if (a.view->enabled != b.view->enabled) return a.view->enabled;
        if (a.tier != b.tier) return a.tier > b.tier;
        return a.order < b.order;
    });
    std::vector<const ActionView*> out;
    for (const Hit& h : hits) {
        if (out.size() >= max) break;
        out.push_back(h.view);
    }
    return out;
}

json::Value ActionJson(const ActionView& v) {
    json::Value row = json::Value::Object();
    row.set("id", std::string(v.id));
    if (!v.key.empty()) row.set("key", std::string(v.key));
    row.set("title", Utf8Of(v.title));
    if (!v.key.empty()) {
        if (const SetDef* d = FindSetting(v.key)) {
            if (IsSwitch(*d)) {
                row.set("values", "true|false");
            } else {
                std::string values;
                for (const SetChoice& c : d->choices)
                    if (c.key) values += (values.empty() ? "" : "|") + std::string(c.key);
                row.set("values", values);
            }
            if (d->flags & kSetSys) row.set("affects_windows", true);
        }
    } else {
        const std::string_view spec = ArgSpec(v.id);
        if (!spec.empty()) {
            row.set("args", ArgSpecJson(spec));
        } else if (!v.choices.empty()) {
            std::string labels;
            for (const auto& c : v.choices)
                if (!c.text.empty() && !c.heading) labels += (labels.empty() ? "" : "|") + Utf8Of(c.text);
            json::Value args = json::Value::Object();
            args.set("choice", labels);
            row.set("args", std::move(args));
        }
    }
    if (!v.value.empty()) row.set("now", Utf8Of(v.value));
    if (!v.enabled) {
        // A file step names its items (and dest) in its arguments: the row's
        // state (现在只有一栏, nothing selected) only holds without them.
        if (IsFileOp(v.id) && ArgSpec(v.id).find("item") != std::string_view::npos) {
            row.set("without_args", Utf8Of(v.why) + " - pass items (and dest) to use it anyway");
        } else {
            row.set("enabled", false);
            row.set("why", Utf8Of(v.why));
        }
    }
    const Policy p = AiPolicy(v.id);
    row.set("risk", RiskName(p.risk));
    if (p.gate == Gate::Confirm) row.set("asks_user", true);
    else if (p.gate == Gate::Plan && IsFileOp(v.id)) row.set("shows_plan", true);   // Pulse asks with a plan card
    else if (p.gate == Gate::Plan) row.set("use_instead", "organize_files / rename_files / run_task / find_cleanup");
    else if (p.gate == Gate::Locked) row.set("locked", true);
    if (!v.shortcut.empty()) row.set("shortcut", Utf8Of(v.shortcut));
    return row;
}

} // namespace pulse::app::actions

// ai_assistant_agent.cpp - Pulse 助手's agent loop on DeepSeek.
//
// One user turn is a series of rounds: stream a chat completion (thinking,
// then content or tool calls), run the tool calls in order, send their
// results back, until the model answers without tools. Everything lands in
// the same conversation blocks the design shows:
//
//   reasoning_content   -> the thinking block (folds into 已思考 N 秒)
//   get_state           -> step 查看当前窗口 (UI thread; actions/action_state.h)
//   list_folder         -> step 读取文件夹 (worker thread, ai_tools_fs.h)
//   get_properties      -> step 查看属性 (worker thread)
//   organize_files      -> plan card + amber confirm card; on 整理 the moves
//                          run through Pulse's operation queue as one undo
//                          step, with a progress step and a result card
//   content             -> streamed answer, then the acts row
//
// Requests carry the tools, so every assistant message keeps its
// reasoning_content in the history (the service requires it back). Nothing
// moves without the user's 整理; the model cannot delete or overwrite.
#include "ai_agent_internal.h"
#include "ai_model_list.h"
#include "extensions/developer_tools.h"
#include "extensions/developer_tool_catalog.h"

#include <algorithm>

namespace pulse {
namespace ai_agent {
using app::ai::ModelEvent;

constexpr int kMaxRounds = 12;
constexpr size_t kMaxToolText = 48 * 1024;   // a tool result sent back, in UTF-8 bytes

std::wstring Text(I id) { return l10n::Get(id).c_str(); }

std::wstring Fill(std::wstring text, std::wstring_view key, const std::wstring& value) {
    if (const size_t at = text.find(key); at != std::wstring::npos) text.replace(at, key.size(), value);
    return text;
}

std::wstring Num(size_t n) { return std::to_wstring(n); }
uint64_t Now() { return ui::motion::NowMs(); }

void Redraw(AppState& s) {
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

AiAgent& Agent(AppState& s) {
    AiChat& c = Chat(s);
    if (!c.agent) c.agent = std::make_shared<AiAgent>();
    return *c.agent;
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    return dir.back() == L'\\' ? dir + name : dir + L"\\" + name;
}

std::wstring Leaf(const std::wstring& path) {
    std::wstring p = path;
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    const size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos || slash + 1 >= p.size() ? p : p.substr(slash + 1);
}

// The folder's name as Pulse shows it (下载 for Downloads).
std::wstring FolderLabel(const std::wstring& path) {
    std::wstring label = app::TabTitle(path);
    return label.empty() ? Leaf(path) : label;
}

// ------------------------------------------------- the list overlay (A4) --
// What the file list shows of the assistant's work (ui/ai_list_overlay.h).
std::shared_ptr<ui::AiListOverlay> g_list;

// Tabs keep the \\?\ form of long paths; tool arguments come back without
// it (CheckPathArg) or with it (the model echoes get_state).
std::wstring NormDir(std::wstring p) {
    for (wchar_t& c : p)
        if (c == L'/') c = L'\\';
    if (p.rfind(L"\\\\?\\UNC\\", 0) == 0) p = L"\\\\" + p.substr(8);
    else if (p.rfind(L"\\\\?\\", 0) == 0) p.erase(0, 4);
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    return p;
}

bool SameDir(const std::wstring& a, const std::wstring& b) {
    return !a.empty() && _wcsicmp(NormDir(a).c_str(), NormDir(b).c_str()) == 0;
}

// The folder is on screen: the active tab shows it and motion is on.
bool Shown(AppState& s, const std::wstring& folder) {
    app::Tab* tab = ActiveTab(s);
    return tab && SameDir(tab->current_path, folder) && ui::motion::SystemAnimationsEnabled();
}

ui::AiListOverlay& ListOverlay(const std::wstring& folder) {
    if (!g_list || !SameDir(g_list->folder, folder)) {
        g_list = std::make_shared<ui::AiListOverlay>();
        g_list->folder = NormDir(folder);
    }
    g_list->end_ms = 0;
    return *g_list;
}

ui::AiListOverlay* OverlayOf(const std::wstring& folder) {
    return g_list && SameDir(g_list->folder, folder) ? g_list.get() : nullptr;
}

// A sort still feeding its requests; Stop submits the rest at once (the
// user confirmed the whole plan).
std::function<void(AppState&)> g_flush_sort;

void EndOverlay(uint64_t in_ms) {
    if (g_list) g_list->end_ms = ui::motion::NowMs() + in_ms;
}

bool IsDirectory(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(app::ai::Win32Path(path).c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool Exists(const std::wstring& path) {
    return GetFileAttributesW(app::ai::Win32Path(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

// The model writes Markdown; the panel draws "- " lists and **bold**. Headings
// become bold lines, other bullets "- ", code ticks go.
std::wstring Display(const std::string& utf8) {
    const std::wstring in = json::Wide(utf8);
    std::wstring out;
    out.reserve(in.size());
    size_t start = 0;
    while (start <= in.size()) {
        size_t end = in.find(L'\n', start);
        if (end == std::wstring::npos) end = in.size();
        std::wstring line = in.substr(start, end - start);
        size_t lead = 0;
        while (lead < line.size() && (line[lead] == L' ' || line[lead] == L'\t')) ++lead;
        std::wstring body = line.substr(lead);
        if (!body.empty() && body[0] == L'#') {
            size_t h = 0;
            while (h < body.size() && body[h] == L'#') ++h;
            while (h < body.size() && body[h] == L' ') ++h;
            body = body.substr(h);
            if (!body.empty() && body.find(L"**") == std::wstring::npos) body = L"**" + body + L"**";
            line = body;
        } else if (body.size() >= 2 && (body[0] == L'*' || body[0] == L'+' || body[0] == L'-') && body[1] == L' ') {
            line = L"- " + body.substr(2);
        } else if (body.rfind(L"```", 0) == 0) {
            line.clear();
        }
        line.erase(std::remove(line.begin(), line.end(), L'`'), line.end());
        out += line;
        if (end >= in.size()) break;
        out += L'\n';
        start = end + 1;
    }
    // Trailing blank lines would leave a gap under the answer.
    while (!out.empty() && (out.back() == L'\n' || out.back() == L' ')) out.pop_back();
    return out;
}

std::string Utf8(const std::wstring& w) { return json::Utf8(w); }

// ---------------------------------------------------------------- request --

const json::Value& Tools() {
    static const json::Value tools = [] {
        static constexpr char kTools[] = R"JSON([
{"type":"function","function":{"name":"get_state",
 "description":"What the user sees in this Pulse window: the panes of the active tab in screen order (pane 1 is the left or top one; focused marks the one in use, target the Ctrl+Alt+D destination) with each folder (null for This PC or a virtual view such as a search, named in location), view, sort, filter, item counts and selection (up to 200 full paths for the focused pane, 20 names for the others), plus the other tabs and windows by title, the staging tray, the preview, an open dialog and the running file task.",
 "parameters":{"type":"object","properties":{}}}},
{"type":"function","function":{"name":"search_files",
 "description":"Finds files and folders anywhere with Pulse's file index in milliseconds (no disk walk, never reads contents). Pulse opens the results in the active tab as a search and shows the conditions to the user as chips they can edit. Returns the count, total size, the folders they are in and the first items. Use it whenever the user wants to find files by name, type, date or size, even without saying where.",
 "parameters":{"type":"object","properties":{
   "label":{"type":"string","description":"2 to 6 words in the user's language naming the results, e.g. 发票 · 9 月"},
   "name":{"type":"string","description":"Words the name contains (all of them), e.g. 发票. Omit when the user gave no name words"},
   "exclude":{"type":"string","description":"A word the name must not contain"},
   "type":{"type":"string","enum":["folder","document","image","video","audio","archive","code"],"description":"A kind of item; omit when extensions are given"},
   "extensions":{"type":"array","items":{"type":"string"},"description":"Exact extensions without dots, e.g. [\"pdf\",\"png\",\"jpg\"] for PDF and pictures"},
   "modified_after":{"type":"string","description":"First day, YYYY-MM-DD (inclusive)"},
   "modified_before":{"type":"string","description":"Last day, YYYY-MM-DD (inclusive)"},
   "larger_than":{"type":"string","description":"A size such as 100MB or 1GB"},
   "smaller_than":{"type":"string","description":"A size such as 500KB"},
   "folder":{"type":"string","description":"Absolute folder to search in; omit to search every indexed location"}}}}},
{"type":"function","function":{"name":"list_folder",
 "description":"Lists a folder's items: folders first, then files, each with type, size and modified time. Returns next_cursor when there are more.",
 "parameters":{"type":"object","properties":{
   "path":{"type":"string","description":"Absolute folder path, e.g. C:\\Users\\me\\Downloads"},
   "limit":{"type":"integer","description":"Items per page, 1-500 (default 200)"},
   "cursor":{"type":"string","description":"next_cursor of the previous page"}},
  "required":["path"]}}},
{"type":"function","function":{"name":"get_properties",
 "description":"File system and shell properties of up to 50 items: sizes, dates, attributes, image size, pages, duration, camera, program version. Never file contents.",
 "parameters":{"type":"object","properties":{
   "paths":{"type":"array","items":{"type":"string"},"description":"Absolute paths"}},
  "required":["paths"]}}},
{"type":"function","function":{"name":"organize_files",
 "description":"Moves items of one folder into subfolders of that folder. Missing subfolders are created, existing ones reused; nothing is overwritten (a clashing name gets a suffix). Pulse shows this plan to the user, who must accept it in Pulse before anything moves; the result says whether it ran. The user can undo it in Pulse.",
 "parameters":{"type":"object","properties":{
   "folder":{"type":"string","description":"Absolute path of the folder whose items move"},
   "groups":{"type":"array","description":"One entry per destination subfolder","items":{"type":"object","properties":{
      "name":{"type":"string","description":"Subfolder name, short, in the user's language"},
      "items":{"type":"array","items":{"type":"string"},"description":"Names of items directly inside folder"}},
     "required":["name","items"]}}},
  "required":["folder","groups"]}}},
{"type":"function","function":{"name":"rename_files",
 "description":"Renames files by one rule that Pulse applies on this PC. You give the rule as batch rename steps, not the new names: Pulse reads each file's date taken and other properties itself, makes every new name, checks for clashes (a clashing name gets a number such as (2)), shows old and new names to the user and asks in Pulse before anything changes. The user can undo it in Pulse.",
 "parameters":{"type":"object","properties":{
   "paths":{"type":"array","items":{"type":"string"},"description":"Absolute paths of the files; omit to rename the files selected in the active tab"},
   "steps":{"type":"array","description":"Applied in order. {\"type\":\"template\",\"pattern\":\"{taken}_Hangzhou_{n}\",\"start\":1,\"digits\":2} replaces the whole name (the extension stays); {\"type\":\"replace\",\"find\":\"a\",\"with\":\"b\",\"regex\":false,\"match_case\":false,\"all\":true}; {\"type\":\"remove\",\"brackets\":false,\"spaces\":false,\"digits\":false,\"custom\":\"\"} (brackets: copy markers such as (1)); {\"type\":\"insert\",\"text\":\"...\",\"where\":\"start|end|index\",\"at\":1}; {\"type\":\"case\",\"mode\":\"lower|upper|title\"}; {\"type\":\"number\",\"where\":\"start|end\",\"sep\":\" \",\"start\":1,\"digits\":3}; {\"type\":\"ext\",\"mode\":\"lower|upper|set\",\"to\":\"jpg\"}. Tokens in pattern and insert text: {name} original name, {n} running number, {taken} date taken yyyy-MM-dd (falls back to the modified date; {taken:yyyyMMdd} for another format), {modified} modified date, {folder} parent folder name, {size} image size WxH.","items":{"type":"object"}},
   "order":{"type":"string","enum":["list","taken","modified","name"],"description":"Numbering order: list (as shown), taken (date taken: use it for photos), modified or name"},
   "daily":{"type":"boolean","description":"true: the number starts again each day (by date taken), e.g. 2026-10-03_01, 2026-10-03_02, 2026-10-04_01"}},
  "required":["steps"]}}},
{"type":"function","function":{"name":"run_task",
 "description":"Does a multi-step file task in one go: finds files with Pulse's index (or takes the selected files), moves them into a destination folder, optionally into subfolders by modified month, year or kind, and optionally packs them into a zip there. Pulse builds the plan from the real files, shows every step and asks the user once in Pulse; the whole task is one undo step, and if a file cannot be moved Pulse pauses and asks the user. Nothing is deleted or overwritten (a clashing name gets a number). Use it instead of search_files plus organize_files when files should be found and then filed or zipped.",
 "parameters":{"type":"object","properties":{
   "label":{"type":"string","description":"The files in a few words in the user's language, e.g. 今年的发票"},
   "noun":{"type":"string","description":"What the files are in one word, e.g. 发票"},
   "find":{"type":"object","description":"Search conditions (as search_files). Omit to use the files selected in the active tab","properties":{
     "name":{"type":"string","description":"Every word the name must contain, separated by spaces: all name words the user gave, e.g. Pelican 发票"},
     "exclude":{"type":"string"},
     "type":{"type":"string","enum":["document","image","video","audio","archive","code"]},
     "extensions":{"type":"array","items":{"type":"string"}},
     "modified_after":{"type":"string","description":"YYYY-MM-DD (inclusive)"},
     "modified_before":{"type":"string","description":"YYYY-MM-DD (inclusive)"},
     "larger_than":{"type":"string"},
     "smaller_than":{"type":"string"},
     "folder":{"type":"string","description":"Absolute folder to search in; omit for everywhere"}}},
   "dest":{"type":"string","description":"Destination folder: an absolute path, or a known folder first (Documents, Downloads, Desktop, Pictures, Videos, Music), e.g. Documents\\发票. Created if missing"},
   "group_by":{"type":"string","enum":["none","month","year","type"],"description":"Subfolders inside dest by modified month (2026-01), year (2026) or kind of file; none puts the files in dest itself"},
   "move":{"type":"boolean","description":"false: leave the files where they are and only make the zip (default true)"},
   "zip":{"type":"string","description":"File name of a zip of the files to make in dest, e.g. 发票-2026.zip; omit for none. With subfolders the zip has the same folders"}},
  "required":["label","dest"]}}},
{"type":"function","function":{"name":"disk_usage",
 "description":"Where a drive's space went, read from Pulse's index in under a second (no disk walk): total, free, system / programs / users / other, the Recycle Bin, and the biggest folders and files. Pulse shows it to the user as a card.",
 "parameters":{"type":"object","properties":{
   "drive":{"type":"string","description":"Drive letter, e.g. C (default C)"}}}}},
{"type":"function","function":{"name":"find_cleanup",
 "description":"Finds what in the user's own folders can safely go to free space: installers of programs already installed, archives already extracted next to them, (1)/(2) copies identical to the original, Temp files untouched for 7 days, and videos unopened for a year (unchecked). Pulse checks it all on this PC, shows the categories with checkboxes and their reasons, checks the Recycle Bin has room, and asks the user; only after they accept in Pulse are the ticked files moved to the Recycle Bin (undoable). Never system or program files. Returns what was moved, or that the user declined.",
 "parameters":{"type":"object","properties":{
   "drive":{"type":"string","description":"Drive letter to free, e.g. C (default C)"}}}}},
{"type":"function","function":{"name":"find_duplicates",
 "description":"Finds files with exactly the same content in the given folders. Pulse compares them on this PC (size, the first 64 KB, then every byte; contents never leave the PC), picks the copy each group keeps (in an organised folder > a name without (1) or copy > the earliest, or the newest with prefer=newest; copies in two organised folders all stay), shows the groups, checks the Recycle Bin has room and asks the user; only after they accept in Pulse are the extra copies moved to the Recycle Bin (undoable), with each kept file locked and re-checked first. Returns what was moved, or that the user declined.",
 "parameters":{"type":"object","properties":{
   "folders":{"type":"array","items":{"type":"string"},"description":"Known folders downloads, pictures, desktop, documents, videos, music, or full folder paths; empty for the current folder. When the user names 下载 / 图片 / 桌面 / 文档 / 视频 / 音乐, pass the known folder name even if the open folder has the same name, unless they point at the open folder (这里 / 当前文件夹)"},
   "prefer":{"type":"string","enum":["earliest","newest"],"description":"Which copy the last rule keeps (default earliest)"}}}}},
)JSON" R"JSON({"type":"function","function":{"name":"find_actions",
 "description":"Looks up Pulse's actions (its menu commands, panels, view options, settings) for run_actions, when what the user wants is not in run_actions' common list. Matches words in Chinese, pinyin or English. Returns up to 8, best first: id, title, args, the current value (now), whether it can run now (enabled false with why), its risk, and the user's shortcut. domain settings lists every setting with its key, values and current value.",
 "parameters":{"type":"object","properties":{
   "query":{"type":"string","description":"A few words naming the action, e.g. 详情面板 文件夹大小"},
   "domain":{"type":"string","enum":["settings"],"description":"settings: list all settings instead of searching"}}}}},
{"type":"function","function":{"name":"run_actions",
 "description":"Does things in Pulse itself, in order, as one undo group (at most 12 steps): panes and the folders in them, windows, tabs, view, sort, grouping, panels, selection, the staging tray, tags, favorites, new folders, opening files, settings, and any action find_actions returns. View, organize and settings steps act at once: Pulse shows each step and one result card with old -> new, Undo and the user's own shortcuts. Opening a program or script, Git push / pull, shell menu items, other programs and system actions (cloud keep / free, eject) show the user a confirm card first, every time (Pulse asks, not you): read first with git.refresh / shell.verbs in a call of their own, then ask for the action in the next call. File steps never run at once: file.copy_to / file.move_to {items, dest}, file.paste, file.rename {item, name}, file.group_into_folder {items, name}, file.dissolve {items}, file.recycle {items}, file.delete_permanent {items}, recycle.restore, recycle.empty - Pulse shows the user a plan card (what goes where; Recycle Bin room; a permanent delete takes two clicks) and does them only after they confirm, so do not ask first. One file step per call; a step that opens the folder it makes (window.layout panes, nav.go) goes after it in the same call, and Pulse runs it after the confirm. items: absolute paths, or leave them out for the selection (select.items first); dest: an absolute folder or a known folder (Documents\\Invoices), made if missing; without dest a copy or move goes to the other pane. Use file.delete_permanent and recycle.empty only when the user asks to delete permanently. To sort many files into several folders use organize_files; to rename several, rename_files. Locked settings and actions only the user does (unlocking a BitLocker drive, quitting Pulse, your own permissions) show the user why and where to do it; do not retry them. Stops at the first step that fails; earlier steps stay done. Returns each step's outcome.\nCommon actions (args): window.layout {layout: single|side_by_side|stacked|three|four, panes: [a folder per pane, left or top first; \"\" leaves one]}, nav.go {path, pane}, window.new {path}, tab.new {path}, tab.close, tab.to_window, pane.to_window {pane}, window.merge, window.overview, nav.back, nav.up, view.mode {view: extra_large_icons|large_icons|medium_icons|small_icons|list|details|tiles|content, pane}, view.sort {by: name|modified|type|size|created, desc, pane}, view.group {by: none|name|date|type|size, pane}, sidebar.toggle {show}, favbar.toggle {show}, details.toggle {show}, select.items {pane, name, extensions, items, type: folder|document|image|video|audio|archive|code, modified: today|yesterday|this_week|last_7_days|this_month|last_month|{after, before}, size: {larger_than, smaller_than}, tag, selected, mode: replace|add|remove} (on a search tab Pulse first waits for the search to finish, 10 s at most, then picks among all its results; tag does not work on content search results), select.all, select.clear, tray.add {items}, tag.toggle {tag, items} (a new tag is made), fav.add {path or items}, file.new_folder {name} (in the open folder), nav.open {path} (a file or program), settings.set {key, value}, edit.undo, tab.pin {tab, pin}, tab.color {tab, color: red|orange|yellow|green|teal|blue|purple|pink|none}, tab.rename {tab, name}, tab.group {tab, group}, tab.activate {tab}, tag.create {name, color, group}, keymap.set {action, keys} (keymap.read lists the actions), git.refresh (the open folder's repository: branch, ahead / behind, changes), git.push, git.pull, shell.verbs (the selection's context menu items), shell.run_verb {verb: an item shell.verbs listed; in a submenu \"Parent ▸ Item\"}, toolbar.open_with {app: a program pinned to the toolbar}, cloud.keep, cloud.free, preview.cloud_download {path}, drive.eject {drive: E:}, drive.disconnect {drive}, file.context_menu and toolbar.customize (Pulse opens them; the user chooses). tab: 1 = first as in get_state, a title, \"all\" or a list - several tabs go in ONE step. Words: 标签 is a tag (tag.create, then tag.toggle to put it on items; 加标签 / 打标签 = tag.toggle), 标签页 is a tab (tab.*), 托盘 / 暂存 is the staging tray (only when the user says so). pane: 1-4 in screen order as in get_state, focused, target, or all (view / sort / group). Paths: absolute, a drive (D:) or a known folder (Downloads, Desktop, Documents, Pictures, Videos, Music). items: full paths; omitted = the selection.\nSettings (settings.set key and values; switches take true/false): theme light|dark|system, window_effect none|acrylic|mica|mica_alt, text_rendering auto|sharp|smooth, font_size small|default|large|larger, language system|zh-CN|zh-TW|en-US, menu_acrylic, row_height compact|standard|roomy, click_to_open double|single, name_highlight hover|always, hover_select, item_checkboxes, blank_double_click nothing|back|up, show_hidden, show_protected, folders_first top|bottom|mixed, smart_dates, zebra_rows, size_bars, tag_name_color, selection_outline, thumbnail_app_badges, alias_mark, tree_sticky, mtime_marks off|dots|bars|text, mtime_range today|3|7|30, mtime_dim_older, mtime_bold_today, mtime_folders, mtime_other_views, mtime_flash, onedrive_status, svn_status, startup_open last_tabs|default_location, new_tab_open current|default_location, external_open smart|tab|window, vertical_tabs, pinned_tab_names, double_click_closes_tab, close_window_with_last_tab, launch_at_signin (Windows), start_in_tray, keep_running, tray_icon always|background|never, explorer_tag_menu (Windows), open_folders_with_pulse (default file manager; Windows), staging_icon_size small|standard|large, staging_hide_when_empty, change_tracking, change_days today|3|7, lan_discovery, network_recent, hidden_shares, confirm_delete (can only be turned on), global_search (Windows), pinyin_search. Locked (only the user, in Settings): confirm_delete off, assistant_permissions, always_allow, connections, api_key.",
 "parameters":{"type":"object","properties":{
   "steps":{"type":"array","description":"The steps in order (at most 12)","items":{"type":"object","properties":{
     "action":{"type":"string","description":"An action id, e.g. view.sort"},
     "args":{"type":"object","description":"The action's arguments"},
     "wait":{"type":"string","enum":["listing_loaded","op_finished","preview_open"],"description":"Wait for this before the step (a selection waits for its folder by itself)"}},
    "required":["action"]}}},
  "required":["steps"]}}}
])JSON";
        json::Value v;
        json::Parse(kTools, v);
        const auto extension_tools = extensions::DeveloperFunctionTools();
        for (const auto& tool : extension_tools.items()) v.push(tool);
        return v;
    }();
    return tools;
}

std::string SystemPrompt() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    char date[32];
    sprintf_s(date, "%04u-%02u-%02u", now.wYear, now.wMonth, now.wDay);
    std::string p =
        "You are Pulse Assistant (Pulse \xE5\x8A\xA9\xE6\x89\x8B), built into Pulse, a file manager for Windows. Today is ";
    p += date;
    p += ".\n"
         "You help the user find, understand and organize files with the tools provided.\n"
         "Rules:\n"
         "- Base every statement about files on tool results; never invent files, sizes or folders.\n"
         "- When the request is about this folder, the selection or what is open, call get_state first; "
         "use list_folder to see a folder's items.\n"
         "- To find files by name, type, date or size, call search_files once with the conditions (work out dates "
         "such as last month from today's date; without a place, search everywhere). Pulse lists the results itself, "
         "so answer with one short sentence saying how many were found. The card already shows the conditions, size and "
         "folders, so do not repeat them and never list files or paths.\n"
         "- To move items into folders call organize_files. Pulse shows the plan and asks the user itself, so do not "
         "ask for confirmation in text first, and never say anything was moved unless organize_files returned ok. "
         "If the user declines, do not propose the same plan again.\n"
         "- Before organize_files, write one short sentence that introduces the plan (what you found and how you "
         "will group it).\n"
         "- To rename files call rename_files with one rule (steps), never a list of names: Pulse reads dates taken "
         "itself, makes the names, shows them and asks the user. For photos by date use a template with {taken}, "
         "order taken, and daily when the user wants numbers per day. Copy words the user gave exactly. Before "
         "rename_files write one short sentence saying how you will name them; afterwards say how many were renamed "
         "and mention files that had no date taken. Never say files were renamed unless rename_files returned ok.\n"
         "- When one request has several steps (find files, then file them into folders and/or zip them), call "
         "run_task once with all of it instead of search_files or organize_files; Pulse builds the plan, asks the user "
         "and pauses on problems itself. Put every name word the user gave in find.name (e.g. both Pelican and 发票) and "
         "dates in modified_after / modified_before. Before run_task write one short sentence saying what you will do; afterwards "
         "say in one or two sentences what was done and name skipped files. Never say files were moved or zipped "
         "unless run_task returned ok.\n"
         "- When the user asks why a drive is full or what takes its space, call disk_usage. To free space call "
         "find_cleanup (after disk_usage when the question is about a drive); Pulse finds what is safe, shows it with "
         "checkboxes and asks the user itself. Before each write one short sentence; never list files or repeat the "
         "card's numbers. After find_cleanup say in one or two sentences what went to the Recycle Bin, and that it "
         "still takes space until the user clicks \xE6\xB0\xB8\xE4\xB9\x85\xE5\x88\xA0\xE9\x99\xA4\xE8\xBF\x99\xE4\xBA\x9B on the card. "
         "Never say space was freed unless Pulse told you so. Never suggest deleting system or program files.\n"
         "- To find or clean duplicate files call find_duplicates with the folders the user named (none: the current "
         "folder); prefer newest only when they ask to keep the newest. Pulse compares on this PC, picks what each group "
         "keeps, shows it and asks the user itself. Before it write one short sentence; never list the files or repeat "
         "the card's numbers. Afterwards say in one or two sentences how many copies went to the Recycle Bin, name a "
         "group where all copies stayed, and that the copies still take space until the user clicks "
         "\xE6\xB0\xB8\xE4\xB9\x85\xE5\x88\xA0\xE9\x99\xA4\xE8\xBF\x99\xE4\xBA\x9B on the card.\n"
         "- To change Pulse itself (panes and the folders in them, windows, tabs, view, sort, grouping, panels, "
         "selection, the staging tray, tags, favorites, new folders, opening a file or program, settings) call "
         "run_actions once with every step in order. Use its common actions; call find_actions first only for anything "
         "else (domain settings gives current setting values). For left/right or top/bottom folders use window.layout "
         "with panes; to pick files by name, type, date or size use select.items. View, organize and settings steps act "
         "at once (the card shows old -> new, Undo and the user's shortcuts), so never ask for confirmation first and "
         "never list the shortcuts; Pulse itself asks before running a program. Afterwards say in one short sentence "
         "what changed and, when useful, how to use it (e.g. drag files between the panes). If a step failed, say which "
         "and why and what was not done. Never say something changed unless run_actions returned it ok.\n"
         "- Some settings are locked (the delete confirmation cannot be turned off; your own permissions, the Always "
         "allow list, connections and API keys): still call run_actions with settings.set so Pulse shows the user why. "
         "Then, in two short sentences, say the step is there so a misunderstanding of yours can never delete their "
         "files (or that you cannot give yourself permissions). Add no other reasons.\n"
         "- You cannot overwrite files or read their contents. Files are moved, renamed or deleted only through steps "
         "the user confirms on Pulse's card (run_actions file steps, organize_files, rename_files, run_task, "
         "find_cleanup, find_duplicates); delete permanently only when the user asks for exactly that.\n"
         "- Reply in the user's language and keep it brief. Format with short paragraphs, lists whose lines start "
         "with \"- \", and **bold**. No headings, tables, code blocks or links.\n"
         "- Names of folders you create: short, in the user's language, no special characters.";
    return p;
}

std::string Body(AppState& s, const AiAgent& a) {
    json::Value body = json::Value::Object();
    body.set("model", Utf8(AiChatModelId(s)));
    json::Value messages = json::Value::Array();
    json::Value sys = json::Value::Object();
    sys.set("role", "system");
    std::string prompt = SystemPrompt();
    // 仅聊天 (ai_model_list.h): no tools go out, and the model says so rather
    // than pretending to act.
    if (!a.sent_tools)
        prompt += "\n- This model cannot call tools here, so you cannot look at or change anything in Pulse. If "
                  "the user asks for an action, say in one sentence that the current model only chats and that a "
                  "model with tool support can be chosen from the model menu; then help with words where you can.";
    sys.set("content", std::move(prompt));
    messages.push(std::move(sys));
    for (const json::Value& m : a.history.items()) messages.push(m);
    body.set("messages", std::move(messages));
    body.set("stream", true);
    json::Value so = json::Value::Object();
    so.set("include_usage", true);
    body.set("stream_options", std::move(so));
    if (a.sent_tools) body.set("tools", Tools());
    // Each service takes 思考 in its own field, or not at all (ai_providers.h).
    app::ai::AddThinking(app::ai::CurrentProvider(), static_cast<app::ai::ThinkLevel>(std::clamp(s.aiAssistant.think, 0, 6)),
                         Utf8(AiChatModelId(s)), body);
    return json::Serialize(body);
}

// ------------------------------------------------------------------ turn --

void SendRound(AppState& s);

AiBlockView* Block(AppState& s, uint32_t id) { return Chat(s).Find(id); }

void FoldThinking(AppState& s, AiAgent& a) {
    AiChat& c = Chat(s);
    AiBlockView* b = Block(s, a.think_id);
    if (b) {
        if (a.reasoning.empty()) {
            // Nothing was said: the block goes rather than reading 已思考 0 秒.
            c.blocks.erase(std::remove_if(c.blocks.begin(), c.blocks.end(),
                                          [&](const AiBlockView& v) { return v.id == a.think_id; }),
                           c.blocks.end());
        } else {
            const uint64_t secs = std::max<uint64_t>(1, (Now() - c.think_start + 500) / 1000);
            b->running = false;
            b->streaming = false;
            b->label = Fill(Text(I::AiAsstThought), L"{n}", std::to_wstring(secs)) + L" \u00B7 " + AiChatEffortName(s);
            b->meta.clear();
            b->seconds.clear();
        }
    }
    if (c.think_id == a.think_id) c.think_id = 0;
    a.think_id = 0;
}

// The steps so far fold into one line once the model speaks again.
void FoldSteps(AppState& s, AiAgent& a) {
    AiBlockView* b = Block(s, a.steps_id);
    if (b && !b->collapsed) {
        int n = static_cast<int>(b->steps.size());
        // A search is the point of the turn; context reads are just groundwork.
        if (a.did_search)
            b->label = Fill(Text(I::AiQStepsLabel), L"{n}", Num(a.searched));
        else if (!a.read_folder.empty())
            b->label = Fill(Fill(Text(I::AiAsstStepsRead), L"{folder}", a.read_folder), L"{n}", Num(a.read_files));
        else
            b->label = Fill(Text(I::AiAsstStepsUsed), L"{n}", std::to_wstring(n));
        b->collapsed = true;
        b->open = false;
    }
    a.steps_id = 0;
}

ui::AiStepView& AddStep(AppState& s, AiAgent& a, ui::AiIcon icon, std::wstring title, std::wstring arg,
                        std::wstring sub) {
    AiBlockView* b = Block(s, a.steps_id);
    if (!b) {
        AiBlockView& nb = Chat(s).Add(AiBlockKind::Steps, Now());
        a.steps_id = nb.id;
        b = &nb;
    }
    ui::AiStepView st;
    st.icon = icon;
    st.title = std::move(title);
    st.arg = std::move(arg);
    st.sub = std::move(sub);
    st.born_ms = Now();
    b->steps.push_back(std::move(st));
    return b->steps.back();
}

ui::AiStepView* LastStep(AppState& s, AiAgent& a) {
    AiBlockView* b = Block(s, a.steps_id);
    return b && !b->steps.empty() ? &b->steps.back() : nullptr;
}

void EndStep(AppState& s, AiAgent& a, bool ok, std::wstring sub) {
    if (ui::AiStepView* st = LastStep(s, a)) {
        st->state = ok ? ui::AiStepState::Done : ui::AiStepState::Failed;
        st->done_ms = Now();
        st->progress = -1.0f;
        st->sub = std::move(sub);
    }
}

void AddToolResult(AiAgent& a, const std::string& id, const ToolOutcome& out) {
    json::Value result = json::Value::Object();
    if (out.ok) {
        result = out.data.is_null() ? json::Value::Object() : out.data;
    } else {
        result.set("ok", false);
        json::Value err = json::Value::Object();
        err.set("code", out.error.code);
        err.set("message", out.error.message);
        result.set("error", std::move(err));
    }
    std::string text = json::Serialize(result);
    if (text.size() > kMaxToolText) {
        // Cut on a character boundary and say so; the model can page.
        size_t cut = kMaxToolText;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
        text.resize(cut);
        text += "... [truncated: ask for a smaller page]";
    }
    json::Value msg = json::Value::Object();
    msg.set("role", "tool");
    msg.set("tool_call_id", id);
    msg.set("content", std::move(text));
    a.history.push(std::move(msg));
}

// Runs `work` on a worker thread; the queue polls until it is done.
void RunJob(AppState& s, std::function<ToolOutcome(const std::atomic<bool>&)> work,
            std::function<void(AppState&, const ToolOutcome&)> done) {
    AiAgent& a = Agent(s);
    auto job = std::make_shared<AiAgent::Job>();
    a.job = job;
    std::thread([job, work = std::move(work)] {
        ToolOutcome out = work(job->cancelled);
        job->out = std::move(out);
        job->done.store(true);
    }).detach();
    const uint64_t gen = a.generation;
    const uint64_t started = Now();
    AiChatPush(s, 0, [job, gen, started, done = std::move(done)](AppState& st) {
        AiAgent& ag = Agent(st);
        if (ag.generation != gen) return -1;
        // Keep the running step on screen long enough to read.
        if (!job->done.load() || Now() - started < 450) return 40;
        ag.job.reset();
        done(st, job->out);
        return -1;
    });
}

// --------------------------------------------------------------- tools --

void ToolGetState(AppState& s, const app::ai::ModelToolCall& call) {
    AiAgent& a = Agent(s);
    AddStep(s, a, ui::AiIcon::Window, Text(I::AiAsstStepWindow), {}, Text(I::AiAsstStepWindowRun));
    json::Value out = app::actions::StateSnapshot(s);
    // The step card reads the focused pane, as before.
    std::wstring sub;
    app::Tab* tab = ActiveTab(s);
    const std::wstring folder = tab ? tab->current_path : std::wstring();
    if (!tab || folder.empty() || fs::IsVirtualPath(folder)) {
        sub = Text(I::AiAsstStepWindowNone);
    } else {
        const size_t picked = static_cast<size_t>((std::max)(0, tab->SelectedCount()));
        const std::wstring label = FolderLabel(folder);
        sub = picked == 0
                  ? Fill(Fill(Text(I::AiAsstStepWindowDone), L"{folder}", label), L"{n}", Num(tab->EntryCount()))
                  : Fill(Fill(Fill(Text(I::AiAsstStepWindowSel), L"{folder}", label), L"{n}", Num(tab->EntryCount())),
                         L"{k}", Num(picked));
    }
    ToolOutcome result;
    result.data = std::move(out);
    AddToolResult(a, call.id, result);
    const uint64_t gen = a.generation;
    AiChatPush(s, 450, [gen, sub](AppState& st) {
        AiAgent& ag = Agent(st);
        if (ag.generation != gen) return -1;
        EndStep(st, ag, true, sub);
        ++ag.call_index;
        NextTool(st);
        return -1;
    });
}

void ToolListFolder(AppState& s, const app::ai::ModelToolCall& call, json::Value args) {
    AiAgent& a = Agent(s);
    const std::wstring path = json::Wide(args.get_string("path"));
    const std::wstring label = path.empty() ? std::wstring() : FolderLabel(path);
    AddStep(s, a, ui::AiIcon::Rows, Text(I::AiAsstStepRead), label, Text(I::AiAsstStepReadWait));
    AiChatSetChip(Chat(s), ui::StatusAiState::Busy, Text(I::AiAsstChipReadingAny), 0.3f);
    if (!args.find("limit")) args.set("limit", 200);
    const std::string id = call.id;
    RunJob(s, [args](const std::atomic<bool>& cancelled) { return app::ai::ListFolder(args, cancelled); },
           [id, label, path](AppState& st, const ToolOutcome& out) {
               AiAgent& ag = Agent(st);
               std::wstring sub;
               if (out.ok) {
                   size_t files = 0, dirs = 0;
                   uint64_t bytes = 0;
                   if (const json::Value* items = out.data.find("items"); items && items->is_array()) {
                       for (const json::Value& it : items->items()) {
                           if (it.get_string("type") == "folder") { ++dirs; continue; }
                           ++files;
                           if (const json::Value* sz = it.find("size")) bytes += static_cast<uint64_t>(sz->as_number());
                       }
                   }
                   sub = dirs ? Fill(Fill(Fill(Text(I::AiAsstStepReadDoneDirs), L"{n}", Num(files)), L"{d}", Num(dirs)),
                                     L"{size}", AiChatFormatSize(bytes))
                              : Fill(Fill(Text(I::AiAsstStepReadDone), L"{n}", Num(files)), L"{size}",
                                     AiChatFormatSize(bytes));
                   ag.read_folder = label;
                   ag.read_files = files;
               } else {
                   sub = Fill(Text(I::AiAsstStepFailed), L"{reason}", json::Wide(out.error.message));
               }
               AddToolResult(ag, id, out);
               ++ag.call_index;
               // The folder is on screen: a spark walks the rows it read, the
               // step and the chip count along (scene 3).
               std::unordered_set<std::wstring> names;
               if (const json::Value* items = out.ok ? out.data.find("items") : nullptr; items && items->is_array())
                   for (const json::Value& it : items->items())
                       names.insert(ui::AiListLower(json::Wide(it.get_string("name"))));
               names.erase(std::wstring());
               if (names.empty() || !Shown(st, path)) {
                   EndStep(st, ag, out.ok, sub);
                   AiChatSetChip(Chat(st), ui::StatusAiState::Busy, Text(I::AiAsstThinking));
                   AiChatPush(st, 250, [](AppState& s2) { NextTool(s2); return -1; });
                   return;
               }
               ui::AiListOverlay& o = ListOverlay(path);
               const uint64_t n = names.size();
               o.read_total = names.size();
               o.read_names = std::move(names);
               o.read_ms = (std::min<uint64_t>)(185, (std::max<uint64_t>)(12, 2400 / n)) * n;
               o.read_start = Now() + 300;   // the title pill comes in first
               o.watch_text = Text(I::AiListWatch);
               std::shared_ptr<const ui::AiListOverlay> ov = g_list;
               const uint64_t gen = ag.generation;
               const bool ok = out.ok;
               AiChatPush(st, 0, [ov, gen, ok, sub](AppState& s2) {
                   AiAgent& a2 = Agent(s2);
                   if (a2.generation != gen) return -1;
                   const uint64_t now = Now();
                   if (const size_t k = ui::AiListReadK(*ov, now)) {
                       const std::wstring kk = Num(k), nn = Num(ov->read_total);
                       if (ui::AiStepView* step = LastStep(s2, a2))
                           step->sub = Fill(Fill(Text(I::AiAsstStepReading), L"{k}", kk), L"{n}", nn);
                       AiChatSetChip(Chat(s2), ui::StatusAiState::Busy,
                                     Fill(Fill(Text(I::AiAsstChipReading), L"{k}", kk), L"{n}", nn),
                                     static_cast<float>(k) / static_cast<float>(ov->read_total));
                   }
                   if (now < ov->read_start + ov->read_ms + 300) return 50;
                   EndStep(s2, a2, ok, sub);
                   AiChatSetChip(Chat(s2), ui::StatusAiState::Busy, Text(I::AiAsstThinking));
                   AiChatPush(s2, 350, [](AppState& s3) { NextTool(s3); return -1; });
                   return -1;
               });
           });
}

void ToolGetProperties(AppState& s, const app::ai::ModelToolCall& call, const json::Value& args) {
    AiAgent& a = Agent(s);
    size_t n = 0;
    std::wstring arg;
    if (const json::Value* paths = args.find("paths"); paths && paths->is_array()) {
        n = paths->size();
        if (n) arg = Leaf(json::Wide(paths->at(0).as_string()));
        if (n > 1) arg += L" +" + Num(n - 1);
    }
    AddStep(s, a, ui::AiIcon::Eye, Text(I::AiAsstStepProps), arg,
            Fill(Text(I::AiAsstStepPropsRun), L"{n}", Num(n)));
    const std::string id = call.id;
    RunJob(s, [args](const std::atomic<bool>& cancelled) { return app::ai::GetProperties(args, cancelled); },
           [id, n](AppState& st, const ToolOutcome& out) {
               AiAgent& ag = Agent(st);
               EndStep(st, ag, out.ok,
                       out.ok ? Fill(Text(I::AiAsstStepPropsDone), L"{n}", Num(n))
                              : Fill(Text(I::AiAsstStepFailed), L"{reason}", json::Wide(out.error.message)));
               AddToolResult(ag, id, out);
               ++ag.call_index;
               AiChatPush(st, 250, [](AppState& s2) { NextTool(s2); return -1; });
           });
}

void NextTool(AppState& s) {
    AiAgent& a = Agent(s);
    if (a.call_index >= a.calls.size()) {
        a.calls.clear();
        a.call_index = 0;
        SendRound(s);
        return;
    }
    const app::ai::ModelToolCall& call = a.calls[a.call_index];
    ++a.tool_count;
    json::Value args;
    if (!json::Parse(call.arguments, args) || !args.is_object()) {
        AddToolResult(a, call.id, ToolOutcome::Fail("invalid_args", "arguments are not a JSON object"));
        ++a.call_index;
        NextTool(s);
        return;
    }
    if (call.name == "describe_extension_api" || call.name == "validate_extension_manifest" || call.name == "get_extension_template") {
        const std::atomic<bool> cancelled{false};
        const app::ai::ToolCall developer_call{L"Pulse assistant", args, cancelled};
        const ToolOutcome result = call.name == "describe_extension_api"
            ? extensions::DescribeExtensionApiTool(developer_call)
            : call.name == "get_extension_template" ? extensions::GetExtensionTemplateTool(developer_call)
            : extensions::ValidateExtensionManifestTool(developer_call);
        AddToolResult(a, call.id, result);
        ++a.call_index;
        NextTool(s);
    }
    else if (call.name == "get_state") ToolGetState(s, call);
    else if (call.name == "search_files") ToolSearch(s, call, args);
    else if (call.name == "list_folder") ToolListFolder(s, call, std::move(args));
    else if (call.name == "get_properties") ToolGetProperties(s, call, args);
    else if (call.name == "organize_files") ToolOrganize(s, call, args);
    else if (call.name == "rename_files") ToolRename(s, call, args);
    else if (call.name == "run_task") ToolTask(s, call, args);
    else if (call.name == "disk_usage") ToolDiskUsage(s, call, args);
    else if (call.name == "find_cleanup") ToolFindCleanup(s, call, args);
    else if (call.name == "find_duplicates") ToolFindDuplicates(s, call, args);
    else if (call.name == "find_actions") ToolFindActions(s, call, args);
    else if (call.name == "run_actions") ToolRunActions(s, call, args);
    else {
        AddToolResult(a, call.id, ToolOutcome::Fail("unknown_tool", "no tool named " + call.name));
        ++a.call_index;
        NextTool(s);
    }
    Redraw(s);
}

ui::AiErrButton Btn(AiErrAct act, AiIcon icon, I text, bool primary) {
    ui::AiErrButton b;
    b.act = act;
    b.icon = icon;
    b.text = Text(text);
    b.primary = primary;
    return b;
}

// DeepSeek at its own address: the one service whose top-up page Pulse knows.
bool Official() {
    return app::ai::IsDefaultProvider(app::ai::CurrentProvider()) && AiAgentBaseUrl() == AiAgentDefaultBaseUrl();
}
std::wstring ServiceName() {
    const app::ai::Provider& p = app::ai::CurrentProvider();
    return app::ai::IsCustomProvider(p) ? AiAgentHost() : app::ai::ProviderName(p);
}

// A refusal that names tools: the model cannot call them (function calling).
bool MentionsTools(const std::string& message) {
    std::string lower(message);
    for (char& ch : lower) ch = static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
    return lower.find("tool") != std::string::npos || lower.find("function") != std::string::npos;
}

AiChatError CutError() {
    AiChatError e;
    e.cut = true;
    e.tone = AiErrTone::Warn;
    e.icon = AiIcon::Cloud;
    e.title = Text(I::AiErrCutT);
    e.sub = Text(I::AiErrEarlyS);
    e.buttons = {Btn(AiErrAct::Retry, AiIcon::Retry, I::AiAsstRetry, true)};
    return e;
}

AiChatError BuildError(const ModelEvent& ev) {
    AiChatError e;
    const std::wstring name = ServiceName();
    const ui::AiErrButton retry = Btn(AiErrAct::Retry, AiIcon::Retry, I::AiAsstRetry, false);
    const ui::AiErrButton retry_pri = Btn(AiErrAct::Retry, AiIcon::Retry, I::AiAsstRetry, true);
    switch (ev.http_status) {
    case 401:
    case 403:
        e.tone = AiErrTone::Err;
        e.icon = AiIcon::Key;
        e.title = Text(I::AiErrAuthT);
        e.sub = Fill(Text(I::AiErrAuthS), L"{name}", name);
        e.buttons = {Btn(AiErrAct::ManageConn, AiIcon::Gear, I::AiErrBtnManage, true), retry};
        return e;
    case 402:
        e.tone = AiErrTone::Err;
        e.icon = AiIcon::Wallet;
        e.peek_title = Text(I::AiErrBalanceT);
        if (Official()) {
            e.title = Text(I::AiErrBalanceOfficialT);
            e.sub = Text(I::AiErrBalanceS);
            e.buttons = {Btn(AiErrAct::TopUp, AiIcon::Open, I::AiErrBtnTopUp, true), retry};
        } else {
            e.title = Text(I::AiErrBalanceT);
            e.sub = Fill(Text(I::AiErrBalanceHostS), L"{host}", AiAgentHost());
            e.buttons = {retry_pri};
        }
        return e;
    case 429:
        // No automatic retry: it would only keep sending refused requests.
        e.tone = AiErrTone::Warn;
        e.icon = AiIcon::Clock;
        e.title = Text(I::AiErrRateT);
        e.sub = Fill(Text(I::AiErrRateS), L"{name}", name);
        e.buttons = {retry_pri};
        return e;
    case 0:
        if (ev.error_type == "incomplete") return CutError();
        e.tone = AiErrTone::Warn;
        e.icon = AiIcon::WifiOff;
        e.title = Fill(Text(I::AiErrNetT), L"{host}", AiAgentHost());
        e.sub = Fill(Text(I::AiErrNetS), L"{code}", std::to_wstring(ev.win32_error));
        e.peek_title = Text(I::AiPeekNet);
        e.buttons = {retry_pri, Btn(AiErrAct::ManageConn, AiIcon::Gear, I::AiErrBtnManage, false)};
        return e;
    default:
        break;
    }
    const std::wstring code = std::to_wstring(ev.http_status);
    if (ev.http_status >= 500) {
        e.tone = AiErrTone::Warn;
        e.icon = AiIcon::Cloud;
        e.title = Fill(Fill(Text(I::AiErrServerT), L"{name}", name), L"{code}", code);
        e.sub = Text(I::AiErrServerS);
        e.peek_title = Text(I::AiPeekServer);
        e.buttons = {retry_pri};
        return e;
    }
    // Refused (400, 404, 422 …): the service's words in the grey box.
    std::wstring detail = json::Wide(ev.error_message);
    if (detail.size() > 300) detail = detail.substr(0, 300) + L"\u2026";
    e.tone = AiErrTone::Err;
    e.icon = AiIcon::Ban;
    e.title = Fill(Text(I::AiErrBadT), L"{code}", code);
    if (!detail.empty()) e.sub = Text(I::AiErrBadS);
    e.detail = detail;
    if (ev.http_status == 404) e.hint = Text(I::AiErrBadHint404);
    else if ((ev.http_status == 400 || ev.http_status == 422) && MentionsTools(ev.error_message))
        e.hint = Text(I::AiErrToolsHint);
    else if (ev.http_status == 400 || ev.http_status == 422) e.hint = Text(I::AiErrBadHint);
    e.buttons = {Btn(AiErrAct::SwitchModel, AiIcon::Swap, I::AiErrBtnSwitch, true), retry};
    if (!detail.empty()) e.buttons.push_back(Btn(AiErrAct::CopyDetail, AiIcon::Copy, I::AiErrBtnCopy, false));
    return e;
}

AiChatError TooManyError() {
    AiChatError e;
    e.tone = AiErrTone::Warn;
    e.icon = AiIcon::AlertCircle;
    e.title = Text(I::AiErrTooManyT);
    e.sub = Text(I::AiErrTooManyS);
    e.buttons = {Btn(AiErrAct::Retry, AiIcon::Retry, I::AiAsstRetry, true)};
    return e;
}

AiChatError NoKeyError() {
    // Not an error: accent, one button.
    AiChatError e;
    e.tone = AiErrTone::Accent;
    e.icon = AiIcon::Plug;
    e.title = Text(I::AiErrNoKeyT);
    e.sub = Text(I::AiErrNoKeyS);
    e.buttons = {Btn(AiErrAct::Connect, AiIcon::Plug, I::AiErrBtnConnect, true)};
    return e;
}

void EndTurn(AppState& s) {
    AiAgent& a = Agent(s);
    FoldThinking(s, a);
    FoldSteps(s, a);
    a.stream.reset();
    const bool searched = a.pending_query != nullptr;
    const size_t found = a.pending_query ? a.pending_query->count : 0;
    AddQueryCard(s, a);
    AiChatAddActs(s, a.tool_count);
    AiChatFinish(s, true);
    if (searched) AiChatSetChip(Chat(s), ui::StatusAiState::Done, Fill(Text(I::AiQChipFound), L"{n}", Num(found)));
}

void Fail(AppState& s, AiChatError error) {
    AiAgent& a = Agent(s);
    FoldThinking(s, a);
    if (AiBlockView* t = Block(s, a.text_id)) t->streaming = false;
    a.text_id = 0;
    // The model never saw a complete turn: forget it so a retry starts clean
    // (an organize that already ran is remembered through the note).
    if (a.history.size() > a.turn_history) {
        json::Value kept = json::Value::Array();
        for (size_t i = 0; i < a.turn_history; ++i) kept.push(a.history.at(i));
        a.history = std::move(kept);
    }
    if (a.organized && a.pending_note.empty())
        a.pending_note = L"(Earlier in this conversation organize_files ran and moved items; the turn then failed.)";
    for (AiBlockView& b : Chat(s).blocks)
        for (ui::AiStepView& st : b.steps)
            if (st.state == ui::AiStepState::Running) { st.state = ui::AiStepState::Failed; st.progress = -1.0f; }
    FoldSteps(s, a);
    a.stream.reset();
    a.pending_query.reset();
    // The card ends the turn; no acts row under a failed one.
    AiChatFail(s, std::move(error));
}

// One drained event batch. Returns false when the round ended.
bool Pump(AppState& s) {
    AiAgent& a = Agent(s);
    AiChat& c = Chat(s);
    if (!a.stream) return false;
    std::vector<ModelEvent> events;
    a.stream->Drain(events);
    for (ModelEvent& ev : events) {
        switch (ev.kind) {
        case ModelEvent::Kind::Reasoning:
            a.reasoning += ev.text;
            if (AiBlockView* b = Block(s, a.think_id)) b->text = json::Wide(a.reasoning);
            break;
        case ModelEvent::Kind::Content: {
            if (a.think_id) FoldThinking(s, a);
            FoldSteps(s, a);
            a.content += ev.text;
            AiBlockView* t = Block(s, a.text_id);
            if (!t && a.carry_id) {
                // 继续生成: the words go on after the part already there.
                t = Block(s, a.carry_id);
                if (t) {
                    a.text_id = a.carry_id;
                    t->streaming = true;
                }
            }
            if (!t) {
                AiBlockView& nt = c.Add(AiBlockKind::Text, Now());
                nt.streaming = true;
                a.text_id = nt.id;
                t = &nt;
            }
            t->text = Display(a.text_id == a.carry_id ? a.carry + a.content : a.content);
            break;
        }
        case ModelEvent::Kind::Finished: {
            FoldThinking(s, a);
            if (AiBlockView* t = Block(s, a.text_id)) {
                t->streaming = false;
                t->text = Display(a.text_id == a.carry_id ? a.carry + a.content : a.content);
            }
            a.text_id = 0;
            a.carry.clear();
            a.carry_id = 0;
            json::Value msg = json::Value::Object();
            msg.set("role", "assistant");
            msg.set("content", ev.content);
            if (!ev.reasoning.empty()) msg.set("reasoning_content", ev.reasoning);
            if (!ev.tool_calls.empty()) {
                json::Value calls = json::Value::Array();
                for (const auto& tc : ev.tool_calls) {
                    json::Value call = json::Value::Object();
                    call.set("id", tc.id);
                    call.set("type", "function");
                    json::Value fn = json::Value::Object();
                    fn.set("name", tc.name);
                    fn.set("arguments", tc.arguments);
                    call.set("function", std::move(fn));
                    calls.push(std::move(call));
                }
                msg.set("tool_calls", std::move(calls));
            }
            a.history.push(std::move(msg));
            a.stream.reset();
            if (ev.tool_calls.empty()) {
                EndTurn(s);
            } else {
                a.calls = std::move(ev.tool_calls);
                a.call_index = 0;
                NextTool(s);
            }
            return false;
        }
        case ModelEvent::Kind::Failed:
            // The service refused the tools (no function calling): remember
            // the model as 仅聊天 and ask again without them, once a turn.
            if ((ev.http_status == 400 || ev.http_status == 422) && a.sent_tools && !a.tools_retried &&
                MentionsTools(ev.error_message)) {
                app::ai::MarkChatOnlyModel(AiAgentBaseUrl(), Utf8(AiChatModelId(s)));
                a.tools_retried = true;
                FoldThinking(s, a);
                a.stream.reset();
                --a.rounds;
                SendRound(s);
                return false;
            }
            Fail(s, BuildError(ev));
            return false;
        }
    }
    if (a.stream && a.stream->Ended()) {
        Fail(s, CutError());
        return false;
    }
    return true;
}

void SendRound(AppState& s) {
    AiAgent& a = Agent(s);
    AiChat& c = Chat(s);
    if (++a.rounds > kMaxRounds) {
        Fail(s, TooManyError());
        return;
    }
    std::wstring key;
    if (!app::ai::LoadModelKey(key)) {
        Fail(s, NoKeyError());
        return;
    }
    a.reasoning.clear();
    a.content.clear();
    a.text_id = 0;
    if (AiAssistantThinks(s) && !a.carry_id) {
        AiBlockView& b = c.Add(AiBlockKind::Thinking, Now());
        b.running = true;
        b.label = Text(I::AiAsstThinking);
        b.meta = L"\u00B7 " + AiChatEffortName(s);
        a.think_id = b.id;
        c.think_id = b.id;
        c.think_start = Now();
    }
    app::ai::ModelEndpoint endpoint;
    endpoint.base_url = AiAgentBaseUrl();
    endpoint.key = std::move(key);
    a.sent_tools = !app::ai::IsChatOnlyModel(endpoint.base_url, Utf8(AiChatModelId(s)));
    a.stream = app::ai::ModelStream::Start(std::move(endpoint), Body(s, a));
    const uint64_t gen = a.generation;
    AiChatPush(s, 30, [gen](AppState& st) {
        if (Agent(st).generation != gen) return -1;
        const bool more = Pump(st);
        Redraw(st);
        return more ? 30 : -1;
    });
}
uint64_t g_key_checked = 0;
bool g_key_present = false;
bool g_base_loaded = false;
std::wstring g_base_url;
} // namespace ai_agent


bool AiAgentConnected() {
    const uint64_t now = GetTickCount64();
    if (!g_key_checked || now - g_key_checked > 2000) {
        g_key_present = app::ai::HasModelKey();
        g_key_checked = now;
    }
    return g_key_present;
}

void AiAgentKeyChanged() {
    g_key_checked = 0;
    g_base_loaded = false;
    app::ai::ProvidersChanged();
}

std::wstring AiAgentBaseUrl() {
    if (!g_base_loaded) {
        g_base_loaded = true;
        const std::string url = app::ai::LoadSettings().assistant.base_url;
        g_base_url = url.empty() ? AiAgentDefaultBaseUrl() : json::Wide(url);
    }
    return g_base_url;
}

// The address of the service in use; empty for a custom one without its own.
std::wstring AiAgentDefaultBaseUrl() { return app::ai::CurrentProvider().base_url; }

std::wstring AiAgentHost() { return app::ai::ModelHost(AiAgentBaseUrl()); }

namespace {
// What the model gets for the user's message: the words, the open folder,
// a note left from an earlier turn.
std::wstring UserContent(AppState& s, AiAgent& a, const std::wstring& prompt, bool with_context) {
    std::wstring content = prompt;
    if (with_context) {
        if (app::Tab* tab = ActiveTab(s); tab && !tab->current_path.empty() && !fs::IsVirtualPath(tab->current_path)) {
            content += L"\n\n[Pulse: the open folder is " + tab->current_path + L" (\u201C" +
                       FolderLabel(tab->current_path) + L"\u201D, " + Num(tab->EntryCount()) + L" items)]";
        }
    }
    if (!a.pending_note.empty()) {
        content = a.pending_note + L"\n\n" + content;
        a.pending_note.clear();
    }
    return content;
}

void PushMessage(AiAgent& a, const char* role, const std::wstring& content) {
    json::Value m = json::Value::Object();
    m.set("role", role);
    m.set("content", Utf8(content));
    a.history.push(std::move(m));
}
}  // namespace

namespace {
// A new turn: nothing of the last one carries over.
AiAgent& ResetTurn(AppState& s) {
    AiAgent& a = Agent(s);
    ++a.generation;
    a.carry.clear();
    a.carry_id = 0;
    a.turn_history = a.history.size();
    a.rounds = 0;
    a.tools_retried = false;
    a.tool_count = 0;
    a.organized = false;
    a.think_id = a.text_id = a.steps_id = 0;
    a.read_folder.clear();
    a.read_files = 0;
    a.pending_query.reset();
    a.searched = 0;
    a.did_search = false;
    a.calls.clear();
    a.call_index = 0;
    return a;
}
}  // namespace

void AiAgentStart(AppState& s, const std::wstring& prompt, bool with_context, const std::wstring& note) {
    AiAgent& a = ResetTurn(s);
    std::wstring content = UserContent(s, a, prompt, with_context);
    if (!note.empty()) content += L"\n\n[Pulse: " + note + L"]";
    PushMessage(a, "user", content);
    SendRound(s);
}

void AiAgentContinue(AppState& s, const std::wstring& prompt, bool with_context, const std::wstring& partial,
                     uint32_t text_id) {
    // The stop forgot the turn: the message, the part written so far, and a
    // request to go on from there.
    AiAgent& a = ResetTurn(s);
    PushMessage(a, "user", UserContent(s, a, prompt, with_context));
    PushMessage(a, "assistant", partial);
    PushMessage(a, "user",
                L"[Pulse: the user stopped the answer above partway. Continue exactly where it stopped, "
                L"without repeating anything already written.]");
    a.carry = Utf8(partial);
    a.carry_id = text_id;
    SendRound(s);
}

int AiAgentToolCount(AppState& s) {
    AiChat& c = Chat(s);
    return c.agent ? c.agent->tool_count : 0;
}

size_t AiAgentHistorySize(AppState& s) {
    AiChat& c = Chat(s);
    return c.agent ? c.agent->history.size() : 0;
}

json::Value AiAgentHistoryJson(AppState& s) {
    AiChat& c = Chat(s);
    return c.agent ? c.agent->history : json::Value::Array();
}

void AiAgentSetHistory(AppState& s, json::Value history) {
    if (!history.is_array()) return;
    Agent(s).history = std::move(history);
}

std::shared_ptr<AiHistoryTail> AiAgentCutHistory(AppState& s, size_t from) {
    AiChat& c = Chat(s);
    auto tail = std::make_shared<AiHistoryTail>();
    if (!c.agent) return tail;
    AiAgent& a = *c.agent;
    const size_t n = a.history.size();
    if (from >= n) return tail;
    json::Value kept = json::Value::Array();
    for (size_t i = 0; i < n; ++i) (i < from ? kept : tail->messages).push(a.history.at(i));
    a.history = std::move(kept);
    return tail;
}

void AiAgentRestoreHistory(AppState& s, size_t from, const std::shared_ptr<AiHistoryTail>& tail) {
    AiChat& c = Chat(s);
    if (!c.agent && (!tail || tail->messages.size() == 0)) return;
    AiAgent& a = Agent(s);
    AiAgentCutHistory(s, from);
    if (!tail) return;
    for (const json::Value& m : tail->messages.items()) a.history.push(m);
}

void AiAgentStop(AppState& s) {
    AiChat& c = Chat(s);
    if (!c.agent) return;
    AiAgent& a = *c.agent;
    ++a.generation;
    if (a.stream) a.stream->Cancel();
    a.stream.reset();
    if (a.job) a.job->cancelled.store(true);
    a.job.reset();
    if (g_flush_sort) {
        auto flush = std::move(g_flush_sort);
        g_flush_sort = nullptr;
        flush(s);
    }
    EndOverlay(400);
    if (a.history.size() > a.turn_history) {
        json::Value kept = json::Value::Array();
        for (size_t i = 0; i < a.turn_history; ++i) kept.push(a.history.at(i));
        a.history = std::move(kept);
    }
    if (a.organized && a.pending_note.empty())
        a.pending_note = L"(Earlier in this conversation organize_files moved items before the user stopped the turn.)";
    a.think_id = a.text_id = a.steps_id = 0;
    a.pending_query.reset();
    a.calls.clear();
    a.call_index = 0;
}

void AiListFlashRows(const std::wstring& folder, const std::vector<std::wstring>& names) {
    if (names.empty() || !ui::motion::SystemAnimationsEnabled()) return;
    // The assistant is busy in another folder: its overlay wins.
    if (g_list && !SameDir(g_list->folder, folder) && !g_list->end_ms) return;
    const bool live = g_list && SameDir(g_list->folder, folder) && !g_list->end_ms;
    const uint64_t prev_end = g_list && SameDir(g_list->folder, folder) ? g_list->end_ms : 0;
    ui::AiListOverlay& o = ListOverlay(folder);
    const uint64_t start = ui::motion::NowMs() + 120;   // after the dialog's exit
    uint64_t at = start;
    for (const std::wstring& name : names) {
        o.flash[ui::AiListLower(name)] = at;
        at += 30;
    }
    // ListOverlay reopened it: close again once the last flash is over, but
    // an overlay the assistant is still using stays open ended.
    if (!live) o.end_ms = (std::max)(prev_end, at + 1500);
}

std::shared_ptr<const ui::AiListOverlay> AiListOverlayFor(const std::wstring& folder) {
    if (!g_list || !SameDir(g_list->folder, folder)) return nullptr;
    if (g_list->end_ms && ui::motion::NowMs() >= g_list->end_ms) {
        g_list.reset();
        return nullptr;
    }
    return g_list;
}

bool AiAssistantOwnsTask(uint64_t task_id) { return task_id != 0 && g_task_ops.count(task_id) != 0; }

void AiAssistantRefreshLists(AppState& s) {
    std::vector<std::wstring> paths;
    ForEachPane(s, [&](app::Pane& pane) {
        const app::Tab* tab = pane.ActiveTab();
        std::wstring kind, rest;
        if (tab && app::ParsePulsePath(tab->current_path, &kind, &rest) && kind == L"ai-list" &&
            std::find(paths.begin(), paths.end(), tab->current_path) == paths.end())
            paths.push_back(tab->current_path);
    });
    for (const std::wstring& path : paths) RefreshPath(s, path);
}

} // namespace pulse

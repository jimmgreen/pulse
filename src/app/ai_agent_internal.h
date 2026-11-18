// ai_agent_internal.h - what Pulse 助手's agent files share: ai_assistant_agent.cpp
// and ai_agent_*.cpp. The agent's state, the run_actions run, the text helpers
// and the functions one file calls in another. Only those files include it.
#pragma once

#include "ai_assistant_chat.h"

#include "ai_assistant.h"
#include "ai_broker.h"
#include "ai_cleanup_scan.h"
#include "ai_dup_scan.h"
#include "ai_model_key.h"
#include "ai_providers.h"
#include "ai_model_stream.h"
#include "ai_search_spec.h"
#include "ai_settings.h"
#include "ai_tools_fs.h"
#include "app_commands.h"
#include "app_hosted_edit.h"
#include "app_internal.h"
#include "app_messages.h"
#include "app_navigation.h"
#include "app_network_page.h"
#include "app_ops_ui.h"
#include "batch_rename_ai.h"
#include "batch_rename_plan.h"
#include "batch_rename_presets.h"
#include "favorites_bar.h"
#include "folder_alias.h"
#include "keymap.h"
#include "places.h"
#include "tab_transfer.h"
#include "vertical_tabs.h"
#include "window_factory.h"
#include "window_overview_ui.h"
#include "window_placement.h"
#include "ai_tools_ui.h"
#include "ai_mcp_run.h"
#include "actions/action_inventory.h"
#include "tray_reveal.h"
#include "../ui/confirm_dialog.h"
#include "zip_compress.h"
#include "actions/action_args.h"
#include "actions/action_files.h"
#include "actions/action_params.h"
#include "actions/action_policy.h"
#include "actions/action_settings.h"
#include "actions/action_state.h"
#include "../common/path_utils.h"
#include "../common/json_value.h"
#include "../common/localization.h"
#include "../fs/fs_enum.h"
#include "../ops/recycle_empty.h"
#include "../ui/ai_list_overlay.h"
#include "../ui/ui_motion.h"
#include "../ui/ui_renderer.h"
#include "../ui/window_material.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cwctype>
#include <map>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <propkey.h>
#include <propsys.h>
#include <shobjidl.h>
#include <shlobj.h>

namespace pulse {
// 55 P2: model messages cut off the history with a version (ai_assistant_chat.h).
struct AiHistoryTail {
    json::Value messages = json::Value::Array();
};

struct AiAgent {
    json::Value history = json::Value::Array();   // user / assistant / tool messages
    std::wstring pending_note;                    // told to the model with the next prompt
    uint64_t generation = 0;                      // bumped by stop: late callbacks drop out

    // The running turn.
    size_t turn_history = 0;                      // history size before the turn
    std::shared_ptr<app::ai::ModelStream> stream;
    int rounds = 0;
    int tool_count = 0;
    bool organized = false;                       // organize_files ran this turn
    bool sent_tools = false;                      // this round's request carried the tools
    bool tools_retried = false;                   // a tools refusal was resent once this turn
    uint32_t think_id = 0;
    uint32_t text_id = 0;
    uint32_t steps_id = 0;
    std::string reasoning;                        // this round, UTF-8
    std::string content;
    // 继续生成 (55 P2): the part already written (UTF-8) and its text block;
    // the first round's words go on there and it shows no thinking block.
    std::string carry;
    uint32_t carry_id = 0;
    std::vector<app::ai::ModelToolCall> calls;    // this round's
    size_t call_index = 0;
    std::wstring read_folder;                     // for the folded steps line
    size_t read_files = 0;

    // search_files (flow A): the card and the search it shows.
    struct QueryCard {
        app::AdvancedSearchSpec spec;
        std::vector<app::ai::SearchField> fields;   // per chip on the card
        std::wstring path;                          // pulse:search: path of the tab
        std::wstring label;                         // the model's title (发票 · 9 月)
        std::vector<std::wstring> paths;            // results Pulse loaded (复制路径)
        size_t count = 0;
        uint64_t ms = 0;
        uint32_t block_id = 0, follow_id = 0;
    };
    std::shared_ptr<QueryCard> pending_query;     // this turn's; shown under the answer
    std::vector<std::shared_ptr<QueryCard>> queries;
    size_t searched = 0;                          // results of this turn's search
    bool did_search = false;

    struct Job {
        std::atomic<bool> done{false};
        std::atomic<bool> cancelled{false};
        app::ai::ToolOutcome out;
    };
    std::shared_ptr<Job> job;
};

namespace ai_agent {
using I = l10n::StringId;
using ui::AiBlockKind;
using ui::AiBlockView;
using app::ai::ToolOutcome;
constexpr size_t kMaxOrganize = 2000;

// ai_assistant_agent.cpp
std::wstring Text(I id);
std::wstring Fill(std::wstring text, std::wstring_view key, const std::wstring& value);
std::wstring Num(size_t n);
uint64_t Now();
void Redraw(AppState& s);
AiAgent& Agent(AppState& s);
std::wstring JoinPath(const std::wstring& dir, const std::wstring& name);
std::wstring Leaf(const std::wstring& path);
std::wstring FolderLabel(const std::wstring& path);
extern std::shared_ptr<ui::AiListOverlay> g_list;
std::wstring NormDir(std::wstring p);
bool SameDir(const std::wstring& a, const std::wstring& b);
bool Shown(AppState& s, const std::wstring& folder);
ui::AiListOverlay& ListOverlay(const std::wstring& folder);
ui::AiListOverlay* OverlayOf(const std::wstring& folder);
extern std::function<void(AppState&)> g_flush_sort;
bool IsDirectory(const std::wstring& path);
bool Exists(const std::wstring& path);
std::wstring Display(const std::string& utf8);
std::string Utf8(const std::wstring& w);
void NextTool(AppState& s);
void Fail(AppState& s, AiChatError error);
AiBlockView* Block(AppState& s, uint32_t id);
void FoldSteps(AppState& s, AiAgent& a);
ui::AiStepView& AddStep(AppState& s, AiAgent& a, ui::AiIcon icon, std::wstring title, std::wstring arg,
                        std::wstring sub);
ui::AiStepView* LastStep(AppState& s, AiAgent& a);
void EndStep(AppState& s, AiAgent& a, bool ok, std::wstring sub);
void AddToolResult(AiAgent& a, const std::string& id, const ToolOutcome& out);
void RunJob(AppState& s, std::function<ToolOutcome(const std::atomic<bool>&)> work,
            std::function<void(AppState&, const ToolOutcome&)> done);

// ai_agent_search.cpp
std::wstring ResultFolderLabel(const std::wstring& folder);
std::wstring Secs(uint64_t ms);
void AwaitSearch(AppState& s, const std::wstring& path, std::function<void(AppState&, const app::Tab*, uint64_t)> done);
void ToolSearch(AppState& s, const app::ai::ModelToolCall& call, const json::Value& args);
void AddQueryCard(AppState& s, AiAgent& a);

// ai_agent_organize.cpp
bool ValidName(const std::wstring& name);
void ToolOrganize(AppState& s, const app::ai::ModelToolCall& call, const json::Value& args);

// ai_agent_rename.cpp
void ToolRename(AppState& s, const app::ai::ModelToolCall& call, const json::Value& args);

// ai_agent_task.cpp
extern std::unordered_set<uint64_t> g_task_ops;
std::wstring KnownFolder(const KNOWNFOLDERID& id);
std::wstring ResolveTaskDest(std::wstring d);
void ToolTask(AppState& s, const app::ai::ModelToolCall& call, const json::Value& args);
// ---------------------------------------------------------- space (D) --
// disk_usage and find_cleanup (flow D): where a drive's space went, then what in the user's own folders can
// go to the Recycle Bin, after the user ticks and confirms it in Pulse.

constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;

// ai_agent_space.cpp
double Gb(uint64_t bytes);
std::wstring Gb1(uint64_t bytes);
std::wstring Int(uint64_t n);
struct SpaceClean {
    wchar_t drive = L'C';
    app::ai::CleanupScan scan;            // categories sorted, items biggest first
    std::vector<std::vector<bool>> on;    // per category, per item
    uint32_t card_id = 0, confirm_id = 0, list_base = 0;
    std::string call_id;
    uint64_t gen = 0;
    app::ai::BinState bin;
    // The batch once it went to the bin.
    std::vector<std::wstring> sources;
    std::vector<uint64_t> source_bytes;
    std::vector<size_t> source_cat;
    std::vector<bool> moved;
    uint64_t moved_bytes = 0;
    size_t moved_n = 0;
    FILETIME since{};
    uint64_t undo_group = 0;
    uint32_t result_id = 0;
    uint64_t free_before = 0, total = 0;
    uint64_t bin_items_before = 0;
    bool dup = false;                     // find_duplicates' copies (flow E)
};

// ai_agent_space.cpp
void ToolDiskUsage(AppState& s, const app::ai::ModelToolCall& call, const json::Value& args);
// The selection's totals.
struct CleanPick {
    uint64_t files = 0, bytes = 0, items = 0;
    std::vector<uint64_t> cat_files, cat_bytes;
};

// ai_agent_space.cpp
CleanPick Picked(const SpaceClean& sc, size_t upto = SIZE_MAX);
bool DriveSpace(wchar_t drive, uint64_t& total, uint64_t& free);
void AskPurge(AppState& s, std::shared_ptr<SpaceClean> sc);
std::function<void(AppState&)> MakeCleanUndo(std::shared_ptr<SpaceClean> sc);
void ToolFindCleanup(AppState& s, const app::ai::ModelToolCall& call, const json::Value& args);

// ai_agent_dups.cpp
void ToolFindDuplicates(AppState& s, const app::ai::ModelToolCall& call, const json::Value& args);
// --------------------------------------------------------- run_actions --
// P3, grown from the third batch's
// control_pulse (flow F): the assistant runs actions of the action table
// (actions/action_registry.h) by id, in order. View, organize and create
// actions run at once; programs and system actions ask first (one confirm
// card for the call); moving and deleting files stays with the flow tools
// (P4). Each action is a step; the result card undoes the call as one group
// and shows the user's own keys (or what to type in the palette). The call
// stops at the first step that fails; what was done stays done and undoable.

struct CtlStep;
struct CtlRun {
    std::string call_id;
    uint64_t gen = 0;
    std::vector<json::Value> queue;     // the actions; a layout's panes add opens
    size_t index = 0;
    // 撤销, in reverse order. Each checks that what it restores still exists.
    std::vector<std::function<void(AppState&)>> undo;
    std::vector<ui::AiKeyRowView> keys;
    std::vector<ui::AiResultLine> lines;
    json::Value report = json::Value::Array();
    size_t ok = 0;
    // What kinds were done (title, undo text).
    bool layout = false, open = false, window = false, other = false;
    size_t organize = 0;
    std::vector<std::wstring> window_names;   // the new windows
    HWND last_window = nullptr;               // the newest of them: the next cascades from it
    std::vector<std::wstring> back_parts;     // 桌面回到右栏 / D 盘窗口已关闭
    std::vector<std::wstring> organize_kinds; // 托盘 / 标签 / 收藏夹
    bool single_back = false;                 // a pane left: the window is one pane again
    app::LayoutPreset first_layout = app::LayoutPreset::Single;
    bool has_first_layout = false;
    std::wstring single_title;                // one plain action: its done line
    bool merged = false;
    // P3.
    bool locked = false, deny_delete = false;   // a locked setting: the lock card
    std::wstring lock_title, lock_sub, lock_where;   // P4b: an only-you action's own lock card
    std::wstring fail_title, fail_arg, fail_why; // the step that failed
    std::vector<std::wstring> not_run;          // titles of the steps after it
    size_t waited = SIZE_MAX;                   // the step whose wait is over
    bool touched = false;                       // a file was made or opened: no 文件没动 line
    size_t undo_steps = 0;                      // steps 撤销 reverses (a selection is not one)
    // P4: one step whose card has its own words (改快捷键), and where 在设置中查看 goes.
    std::wstring title_override, sub_override;
    int settings_page = -1;
    // P4: the last runner step's line, so the same action one tab at a time
    // reads as one line (固定了 3 个标签页).
    std::string arg_last_id;
    size_t arg_last_line = SIZE_MAX;
    size_t arg_tally = 0;
    bool arg_last_pin = false;
    std::wstring arg_new_tag;   // tag.create's name: a tag.toggle right after it joins its line
    // P4c: the call's file step (action_files.h). 1 = its card is up or its
    // tasks run; 2 = over, `file_step` is how it went (CtlNext takes it).
    // A select.items on a content search's results waits for its match the
    // same way (P4d).
    int file_state = 0;
    std::shared_ptr<CtlStep> file_step;
    // P4d: the card says only its title and sub (在搜索结果里选中了 41 项 /
    // 分布在 9 个文件夹), not the same line again.
    bool override_alone = false;
    bool search_partial = false;   // the search was not done within the wait
    // P5: run for an AI app (ai_mcp_run.h): no chat; Pulse's dialog asks.
    std::shared_ptr<app::ai::McpActionsRun> mcp;
    std::wstring mcp_line;   // the file step's done line (6 项已移到「2026」)
    std::wstring mcp_open;   // where its notification's 打开 goes
    bool mcp_files = false;  // the file step changed files
    std::vector<std::wstring> mcp_steps;   // 本次运行's summary: what each done step did
    std::wstring mcp_clicked;              // the button the user pressed last (你点了「移动」)
    bool mcp_by_rule = false;              // a 始终允许 rule answered instead of the user
    int mcp_already = 0;                   // steps that found it so already (nothing to do)
};

// ai_agent_actions.cpp
bool PaneAlive(AppState& s, const app::Pane* p);
std::wstring PanePos(AppState& s, size_t i);
size_t PaneIndex(AppState& s, const app::Pane* p);
std::wstring PlaceLabel(const std::wstring& path);
std::vector<std::wstring> KeysFor(app::KeyAction action, int position = -1);
void AddKeys(CtlRun& run, std::wstring label, std::vector<std::wstring> caps);
std::wstring CtlFolder(const std::string& utf8);
struct CtlStep {
    ui::AiIcon icon = ui::AiIcon::Window;
    std::wstring title, arg, sub;
    bool ok = false;
    std::string error;               // back to the model when !ok
    app::Pane* wait = nullptr;       // the sub is the item count once it loads
    bool skip = false;               // nothing to do (already so): no step
    std::wstring why;                // !ok: the reason the card shows (找不到这个文件夹)
    json::Value data;                // more for the model (a selection's report)
    bool quiet = false;              // no step row: the card that follows says it (the lock card)
};

// ai_agent_dupfind.cpp: file.find_duplicates waits for its compare (task 46);
// true while it runs (CtlNext goes on when it ends).
bool DupStepAsk(AppState& s, std::shared_ptr<CtlRun> run, const json::Value& args);

// ai_agent_mcp.cpp
bool RunGone(AppState& s, const CtlRun& run);
void McpLater(AppState& s, std::shared_ptr<app::ai::McpActionsRun> mcp, uint32_t delay, std::function<int(AppState&)> fn);

// ai_agent_actions.cpp
void Later(AppState& s, const CtlRun& run, uint32_t delay, std::function<int(AppState&)> fn);
CtlStep CtlLayout(AppState& s, CtlRun& run, const json::Value& act);
CtlStep CtlOpen(AppState& s, CtlRun& run, const json::Value& act);
CtlStep CtlNewWindow(AppState& s, CtlRun& run, const json::Value& act);
CtlStep CtlToWindow(AppState& s, CtlRun& run, const json::Value& act, bool pane_only);
ui::AiResultLine SplitLine(const std::wstring& text, std::wstring_view key, const std::wstring& value);
void AddLine(CtlRun& run, ui::AiResultLine ln);
void PlainDone(CtlRun& run, ui::AiResultLine ln);
CtlStep CtlView(AppState& s, CtlRun& run, const json::Value& act);
CtlStep CtlSort(AppState& s, CtlRun& run, const json::Value& act);
CtlStep CtlGroup(AppState& s, CtlRun& run, const json::Value& act);
CtlStep CtlBar(AppState& s, CtlRun& run, const json::Value& act, bool favorites);
CtlStep CtlTray(AppState& s, CtlRun& run, const json::Value& act);
CtlStep CtlTag(AppState& s, CtlRun& run, const json::Value& act);
CtlStep CtlFavorite(AppState& s, CtlRun& run, const json::Value& act);
CtlStep CtlMerge(AppState& s, CtlRun& run);
CtlStep CtlOverview(AppState& s, CtlRun& run);
CtlStep CtlSelect(AppState& s, CtlRun& run, const json::Value& act);

// ai_agent_files.cpp
void CtlNext(AppState& s, std::shared_ptr<CtlRun> run);

// ai_agent_actions.cpp
bool SelectAsk(AppState& s, std::shared_ptr<CtlRun> run, const json::Value& act);
CtlStep CtlSelectNone(AppState& s, CtlRun& run);
std::wstring OpenTarget(AppState& s, const json::Value& act);
std::wstring LeafName(const std::wstring& path);
CtlStep CtlOpenFile(AppState& s, CtlRun& run, const json::Value& act);
CtlStep CtlNewFolder(AppState& s, CtlRun& run, const json::Value& act);
CtlStep CtlSetting(AppState& s, CtlRun& run, const json::Value& act);
CtlStep CtlGeneric(AppState& s, CtlRun& run, const std::string& id, const json::Value& act);

// ai_agent_mcp.cpp
void McpFinish(AppState& s, std::shared_ptr<CtlRun> run);

// ai_agent_actions.cpp
void CtlFinish(AppState& s, std::shared_ptr<CtlRun> run);
std::string StepAction(const json::Value& step);
const json::Value& StepArgs(const json::Value& step);
std::wstring StepTitle(AppState& s, const std::string& id);
CtlStep CtlArgs(AppState& s, CtlRun& run, const std::string& id, const json::Value& act);
bool IsChooser(std::string_view id);
CtlStep CtlChooser(AppState& s, const std::string& id);
// ------------------------------------------------------ file steps (P4c) --
// A move, copy, rename or delete in
// run_actions shows its plan card (the first batch's 整理计划 row for moves,
// the second batch's Recycle Bin card, the red two-click card for a permanent
// delete); only after 确认 does Pulse queue the menus' own requests, under one
// undo group, and the call goes on once they are done.

namespace fo = app::actions;

// ai_agent_files.cpp
std::wstring FoShort(const std::wstring& path);
void FoCard(AppState& s, CtlRun& run, const fo::FilePlan& p, AiBlockView& cf);
void FoSubmit(AppState& s, std::shared_ptr<CtlRun> run, std::shared_ptr<fo::FilePlan> plan);

// ai_agent_mcp.cpp
void McpFoAsk(AppState& s, std::shared_ptr<CtlRun> run, std::shared_ptr<fo::FilePlan> plan);

// ai_agent_settings.cpp
void ToolChangeSettings(AppState& s, const app::ai::ModelToolCall& call, const json::Value& args);

// ai_agent_files.cpp
void ToolRunActions(AppState& s, const app::ai::ModelToolCall& call, const json::Value& args);
void ToolFindActions(AppState& s, const app::ai::ModelToolCall& call, const json::Value& args);
// ---- Third batch, flow S: Pulse's settings ----------------------------------
// The rows live in actions/action_settings.cpp (the command palette lists them too).
using app::actions::ApplySetting;
using app::actions::ChoiceIndex;
using app::actions::FindSetting;
using app::actions::IsSwitch;
using app::actions::kSetSys;
using app::actions::SetDef;
using app::actions::SettingText;

// ai_agent_settings.cpp
json::Value SettingJson(const SetDef& d, int v);
// ---------------------------------------------------------------- stream --

// Error card A: red needs the user,
// amber passes on its own. "DeepSeek" only for the official service; a custom
// address is named by its host.
using ui::AiErrAct;
using ui::AiErrTone;
using ui::AiIcon;
} // namespace ai_agent

// The agent's files see these as the single file's unnamed namespace did.
using namespace ai_agent;

} // namespace pulse

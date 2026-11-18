// ai_assistant_chat.h - Pulse 助手's conversation: the blocks the panel shows,
// the status chip's words, and the driver that fills them.
//
// A2 drives it with a local demo that plays the design's script on the open
// folder (read only, nothing moves); A3 replaces the driver with the DeepSeek
// agent loop. Internal to src/app (not part of AppState's header).
#pragma once

#include "../ui/ai_panel_view.h"
#include "../ui/ui_view_model.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulse {
struct AppState;
struct AiAgent;   // the DeepSeek agent loop (ai_assistant_agent.cpp)
struct AiHistoryTail;   // model messages cut off the history (ai_agent_internal.h)
namespace json { class Value; }

// 55 P2: the versions of one user message, like branches. A version is the
// message's words plus everything after it: the blocks and the model
// history. The current version lives in AiChat::blocks (its entry here only
// keeps its words); the others wait here until ‹ › brings one back.
struct AiTurnVersion {
    std::wstring text, context;
    std::vector<ui::AiBlockView> tail;
    std::shared_ptr<AiHistoryTail> history;
};

struct AiTurnBranch {
    std::vector<AiTurnVersion> versions;
    size_t current = 0;
};

struct AiChat {
    std::vector<ui::AiBlockView> blocks;
    uint32_t next_id = 1;
    bool busy = false;            // a turn runs (send shows stop)
    bool noted = false;           // the demo note is in this conversation
    std::wstring last_prompt;

    // The status bar chip while the assistant works (Idle: not ours).
    ui::StatusAiState chip = ui::StatusAiState::Idle;
    std::wstring chip_text;
    float chip_progress = -1.0f;
    bool unread = false;

    // Thinking block whose seconds count up (0: none).
    uint32_t think_id = 0;
    uint64_t think_start = 0;
    uint64_t turn_start = 0;

    // Driver: steps run in order, each after its delay; a step returns the
    // ms until it runs again (streaming) or -1 when it is done.
    struct Step {
        uint32_t delay = 0;
        std::function<int(AppState&)> run;
    };
    std::deque<Step> queue;
    uint64_t next_at = 0;
    // Paused at a confirmation card until the user answers.
    uint32_t confirm_id = 0;
    std::function<void(AppState&, bool)> on_confirm;
    std::function<void(AppState&)> on_extra;      // the confirmation's link button
    // A paused task (flow C): its card and what its buttons do.
    uint32_t pause_id = 0;
    std::function<void(AppState&, ui::AiErrAct)> on_task_act;
    // The latest result card's undo and 在 Pulse 中查看.
    uint32_t result_id = 0;
    std::function<void(AppState&)> on_undo;
    std::function<void(AppState&)> on_show;
    std::function<void(AppState&)> on_purge;      // 永久删除这些… (flow D)
    // The cleanup card (flow D) while it can still change: its checkboxes,
    // 查看 links and file rows (row, file; file -1 for a category).
    uint32_t clean_id = 0;
    std::function<void(AppState&, ui::AiPanelPart, int, int)> on_clean;
    // The duplicates card (flow E) while it can still change: part DupFile
    // (group, file), DupRule, DupMore (-1, -1).
    uint32_t dup_id = 0;
    std::function<void(AppState&, ui::AiPanelPart, int, int)> on_dup;
    // The model conversation; created by the first real turn.
    std::shared_ptr<AiAgent> agent;
    // The peek bubble over the chip once the panel is closed (.peek).
    uint64_t peek_at = 0;         // shows from (0: none)
    uint64_t peek_until = 0;      // hides at
    uint64_t peek_turn = 0;       // turn_start of the turn it was shown for
    std::wstring peek_text;       // the turn's summary (organize_files sets it)
    bool peek_undo = false;       // the turn's result card can still undo
    // The turn failed: the peek leads with 重试 (连接模型) in the card's tone.
    bool peek_failed = false;
    ui::AiErrTone peek_tone = ui::AiErrTone::Err;
    std::wstring peek_title;

    // 55 P2. Per user block id: the model history's length before it, and
    // its versions once it has more than one.
    std::unordered_map<uint32_t, size_t> turn_hist;
    std::unordered_map<uint32_t, AiTurnBranch> branches;

    // 55 P3 历史对话: the file this conversation saves to (empty until its
    // first save), its title, whether the user renamed or pinned it, and
    // what was last written (unchanged: not written again).
    std::wstring history_id, title;
    bool renamed = false, pinned = false;
    int64_t created = 0;
    size_t saved_sig = 0;

    ui::AiBlockView* Find(uint32_t id);
    ui::AiBlockView& Add(ui::AiBlockKind kind, uint64_t now_ms);
};

// A failed turn (the agent builds it from the stream's error; error card A).
struct AiChatError {
    ui::AiErrTone tone = ui::AiErrTone::Err;
    ui::AiIcon icon = ui::AiIcon::AlertCircle;
    std::wstring title, sub, detail, hint;
    std::vector<ui::AiErrButton> buttons;
    std::wstring peek_title;      // shorter title for the bubble (empty: title)
    bool cut = false;             // cut off: a partial answer keeps an amber strip
};

AiChat& Chat(AppState& s);
// Starts a turn for `prompt`; `context` is the composer's folder chip text.
void AiChatSubmit(AppState& s, const std::wstring& prompt, const std::wstring& context);
void AiChatStop(AppState& s);
void AiChatReset(AppState& s);
// Conversation parts the panel reports (block index into blocks).
void AiChatActivate(AppState& s, ui::AiPanelPart part, int block);
// The cleanup card's rows: part CleanCheck / CleanLink (index block * 32 +
// row) or CleanFile ((block * 32 + row) * 32 + file).
void AiChatClean(AppState& s, ui::AiPanelPart part, int index);
// The duplicates card: DupFile (index block * 64 + group * 8 + file),
// DupRule / DupMore (index block).
void AiChatDup(AppState& s, ui::AiPanelPart part, int index);

// Shared by the demo driver and the agent loop.
void AiChatPush(AppState& s, uint32_t delay, std::function<int(AppState&)> run);   // queues and schedules
// Runs the steps that are due. The frame pump calls it: while motion runs its
// posted frames outrank WM_TIMER, which would hold the queue back for seconds.
void AiChatRunDue(AppState& s);
void AiChatSetChip(AiChat& c, ui::StatusAiState state, std::wstring text, float progress = -1.0f);
void AiChatFinish(AppState& s, bool done_chip);   // ends the turn
std::wstring AiChatEffortName(const AppState& s);
std::wstring AiChatModelId(const AppState& s);
std::wstring AiChatFormatSize(uint64_t bytes);
// The turn's acts row: copy / retry / helpful and "model · effort · s · tools".
// failed: the turn ended on an error; the row shows only the model and no rating.
void AiChatAddActs(AppState& s, int tools, bool failed = false);
// A plan group's icon and tint from the kinds of its items.
void AiChatGroupLook(const std::vector<std::wstring>& names, ui::AiIcon& icon, ui::AiTint& tint);
// The peek bubble: shows `delay` ms from now for 4.2 s, once per turn, when
// the turn is done and the panel is closed.
void AiChatPeek(AppState& s, uint32_t delay);
void AiChatHidePeek(AppState& s);
void AiChatFillPeek(AppState& s, bool panel_open, ui::AiPeekView& peek);
void AiChatUndoLast(AppState& s);   // the latest result card's 撤销
// Ends the turn on `err`: the card (or the cut strip), the error chip and,
// with the panel closed, the peek.
void AiChatFail(AppState& s, AiChatError err);
// Sends the last prompt again in place of the failed turn (no second bubble).
void AiChatRetryTurn(AppState& s);
// 55 P2, `block` any block of the turn. 重新生成: the turn's answer becomes a
// new version (kind 0 再试一次, 1 更详细, 2 更简洁). ‹ ›: another version.
// 继续生成: a stopped answer goes on. Edit: the message gets new words and
// a new version.
enum class AiRegen : uint8_t { Again, Longer, Shorter };
void AiChatRegenerate(AppState& s, int block, AiRegen kind);
void AiChatSwitchVersion(AppState& s, int block, int delta);
void AiChatContinue(AppState& s, int block);
void AiChatEditTurn(AppState& s, int user_block, const std::wstring& text);
// The user block of the turn `block` is in (-1: none).
int AiChatUserOf(const AiChat& c, int block);
// The error card button behind an ErrAction hit (false: none).
bool AiChatErrAction(AppState& s, int index, ui::AiErrAct& act);
// Runs the chat's own error buttons: 重试, 去充值, 复制详情.
void AiChatErrRun(AppState& s, int index);

// The agent loop (ai_assistant_agent.cpp).
bool AiAgentConnected();        // a model key is stored (checked at most every 2 s)
void AiAgentKeyChanged();       // after the key or the address was saved or removed
std::wstring AiAgentBaseUrl();  // ai.json assistant.base_url, else the default
std::wstring AiAgentDefaultBaseUrl();   // the service's own address (ai_providers.h)
std::wstring AiAgentHost();     // "api.deepseek.com"
std::wstring AiAssistantModelName(const AppState& s);   // the model a request uses, as the service names it
// 思考 for that model (ai_providers.h ModelThinking): whether a request
// thinks, and the level's name for the turn's meta (高, 开启, 自动 …).
bool AiAssistantThinks(const AppState& s);
std::wstring AiAssistantThinkName(const AppState& s);
// `note`: one more line for the model only (重新生成's 更详细 / 更简洁).
void AiAgentStart(AppState& s, const std::wstring& prompt, bool with_context, const std::wstring& note = {});
// 继续生成: the turn again with the part already written; what comes goes
// on in the text block `text_id`, without a thinking block.
void AiAgentContinue(AppState& s, const std::wstring& prompt, bool with_context, const std::wstring& partial,
                     uint32_t text_id);
// The model history: its length, the messages from `from` on (cut off and
// returned), and putting such a tail back at `from`.
size_t AiAgentHistorySize(AppState& s);
// 55 P3: the whole model history as a saved conversation keeps it, and
// putting such a history into a fresh conversation (历史对话 › open).
json::Value AiAgentHistoryJson(AppState& s);
void AiAgentSetHistory(AppState& s, json::Value history);

// 55 P3 历史对话 (ai_chat_history.cpp). Saving needs a message and
// 保存对话历史 on; opening another conversation stops and saves this one.
void AiHistorySave(AppState& s);
bool AiHistoryOpenChat(AppState& s, const std::wstring& id);
// The layer over the panel: open lists the saved conversations (and drops
// those past 保留时间).
void AiHistoryShow(AppState& s, bool open);
void AiHistoryFillVm(AppState& s, ui::AiHistoryView& view);
// A click in the layer; true when it started renaming a row (the app puts
// its EDIT there with AiHistoryRenameTitle).
bool AiHistoryActivate(AppState& s, ui::AiPanelPart part, int row);
void AiHistorySetQuery(AppState& s, std::wstring query);
void AiHistoryRenameText(AppState& s, std::wstring text);
const std::wstring& AiHistoryRenameTitle(AppState& s);
bool AiHistoryRenaming(const AppState& s);
void AiHistoryRenameEnd(AppState& s, bool save);   // Enter saves, Esc / elsewhere gives up
bool AiHistoryOpenFirst(AppState& s);              // Enter in the search box
// Esc: the rename, the confirmation, then the layer; true when taken.
bool AiHistoryEscape(AppState& s);
bool AiHistoryRowPinned(AppState& s, int row);     // its tip: 固定 / 取消固定
int AiAgentToolCount(AppState& s);   // tool calls of the running (or last) turn
std::shared_ptr<AiHistoryTail> AiAgentCutHistory(AppState& s, size_t from);
void AiAgentRestoreHistory(AppState& s, size_t from, const std::shared_ptr<AiHistoryTail>& tail);
void AiAgentStop(AppState& s);   // cancels the request and any tool still running
// Flow A: the 搜索条件 card's chip × and + 条件 search again in Pulse without
// the model; 接着做 fills the composer (复制路径 copies at once).
void AiAgentQueryChip(AppState& s, int block, int chip);
void AiAgentQueryAdd(AppState& s, int block);
void AiAgentFollow(AppState& s, int block, int item);
// Puts `text` into the composer without sending it and focuses it (ai_assistant.cpp).
// `select_from`: the text from there on starts selected (AI 整理此文件夹…'s
// 按类型分到文件夹, typed over to change it); past the end: the caret at the end.
void AiAssistantFillComposer(AppState& s, const std::wstring& text, size_t select_from = static_cast<size_t>(-1));
// 语音输入: recognized words go after what is there (a space only between
// letters or digits), the caret at the end. Never sends.
void AiAssistantAppendComposer(AppState& s, const std::wstring& words);
void AiChatCopyText(HWND hwnd, const std::wstring& text);

} // namespace pulse

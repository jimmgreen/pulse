// ai_panel_view.h — The Pulse 助手 panel.
//
// Drawn in the main window by MainRenderer. Docked (the default) it is a
// column right of the sidebar rail and the toolbar and panes move over;
// floating it is a card over the lower left of the window. The view owns the
// open/close motion and the geometry of the last frame (hit tests, where the
// app puts the composer's EDIT); everything it shows comes from the view model.
#pragma once
#include "ai_panel_icons.h"
#include "FluentTokens.h"
#include "fluent_components.h"

#include <d2d1_1.h>
#include <dwrite.h>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <wrl/client.h>

namespace pulse::ui {

class Compositor;
struct AiRichDoc;     // ai_panel_rich.h
struct AiRichPiece;
// A point in the conversation's text: a block, a piece of it, a layout offset.
struct AiRichSelPos {
    uint32_t id = 0;
    int piece = 0;
    uint32_t offset = 0;
};
struct AiVoiceMenuView;   // 语音设置 menu (ai_panel_voice.h)

enum class AiPanelMode : uint8_t { Docked = 0, Floating = 1 };

// The design's file-type tints (icon tiles, plan groups, result chips).
enum class AiTint : uint8_t { Image, Document, App, Video, Archive, Neutral };

enum class AiSendState : uint8_t { Dim, Ready, Busy };

enum class AiPanelPart : uint8_t {
    None,
    Panel,          // anywhere else on the panel (swallows the click)
    NewChat,
    Mode,           // docked <-> floating
    Close,
    Suggestion,     // index
    ContextChip,
    ContextRemove,
    AddFiles,
    ModelButton,
    Send,
    Input,
    Menu,           // the 思考强度 popover's background
    MenuModel,      // its 「model ›」: opens the model list
    MenuSlider,     // its slider; index = the stop under the pointer
    MenuReset,      // its ↺: back to the default level
    Conversation,
    // Conversation parts; index = block index in AiPanelViewModel::blocks.
    ThinkToggle,
    StepsToggle,
    PlanToggle,
    ConfirmDeny,
    ConfirmAccept,
    ResultUndo,
    ResultShow,
    ActCopy,
    ActRetry,
    ActThumb,
    ErrAction,      // index = block index * 4 + button (AiBlockView::actions)
    CutRetry,       // the 重试 link on the 回答中途断开了 strip
    // Search conditions card (first batch, flow A).
    QueryChipRemove,   // index = block index * 16 + chip (the chip's ×)
    QueryAdd,          // + 条件
    FollowItem,        // index = block index * 8 + item (接着做 chips)
    // Rename plan card and its confirmation (first batch, flow B).
    RenameToggle,      // the card's header (folds the card)
    RenameMore,        // 还有 4 个 / 收起
    ConfirmExtra,      // 在批量重命名中调整… on the confirmation
    TaskToggle,        // 任务计划 card header (flow C)
    // Second batch, flow D (space).
    DiskToggle,        // 本地磁盘 card header
    CleanToggle,       // 清理建议 card header
    CleanCheck,        // index = block index * 32 + row (the category's box)
    CleanLink,         // index = block index * 32 + row (查看)
    CleanFile,         // index = (block index * 32 + row) * 32 + file (a file's box)
    ResultPurge,       // 永久删除这些… on a result card
    // Second batch, flow E (duplicates).
    FunnelToggle,      // 比对过程 card header
    DupToggle,         // 重复文件 card header
    DupFile,           // index = block index * 64 + group * 8 + file (that copy stays)
    DupRule,           // 改规则 (block index)
    DupMore,           // 在 Pulse 中查看全部 (block index)
    // Not connected yet: the empty state's 去设置模型服务.
    GoSettings,
    // 语音输入 (third batch, flow V): the composer's microphone and its menu.
    Mic,
    MicMenu,        // the small chevron beside it
    VoiceMenu,      // the menu's background
    VoiceManage,    // 语音设置… in the menu
    VoiceMicPick,   // the microphone dropdown
    VoicePause,     // index 0..2: 1 秒 / 2 秒 / 手动
    // The bubble above the chip once the panel is closed.
    PeekCard,
    PeekClose,
    PeekUndo,
    PeekOpen,
    // Rich answers (index = block index * 256 + target): a link, a local path
    // chip, a code block's 复制, a table's 复制表格.
    RichLink,
    RichPath,
    CodeCopy,
    TableCopy,
    // The selection's mini toolbar: 复制 · 引用 · 在 Pulse 中搜索.
    SelCopy,
    SelQuote,
    SelSearch,
    // The composer's quote chip: its ×.
    QuoteRemove,
    // 55 P2 (index = block index): 重新生成's menu caret, 没帮助, the ‹ › of
    // an answer's versions, the user bubble's 复制 / 编辑, 继续生成 on a
    // stopped version.
    ActRegenMenu,
    ActThumbDown,
    VerPrev,
    VerNext,
    UserCopy,
    UserEdit,
    StopContinue,
    // The message being edited in place (index = its block): the text area
    // (while the panel draws it), 取消, 发送.
    EditBox,
    EditCancel,
    EditSend,
    // 55 P3 历史对话: the header button; the layer itself (absorbs clicks),
    // its search box and foot link; a row (index = row) and its 固定 /
    // 重命名 / 删除, the inline 取消 · 删除, the title while it is renamed.
    History,
    HistArea,
    HistSearch,
    HistSettings,
    HistRow,
    HistPin,
    HistRename,
    HistDelete,
    HistDelCancel,
    HistDelOk,
    HistRenameBox,
    JumpLatest,   // 55 P4: the 回到最新 pill over the conversation's bottom
};

struct AiPanelHit {
    AiPanelPart part = AiPanelPart::None;
    int index = -1;
    bool operator==(const AiPanelHit&) const = default;
};

struct AiSuggestionView {
    AiIcon icon = AiIcon::Move;
    AiTint tint = AiTint::Image;
    std::wstring title;
    std::wstring detail;
};

// One piece of the conversation (v2.css "messages" … "result card").
enum class AiBlockKind : uint8_t {
    User,       // bubble on the right, optional context chip above it
    Thinking,   // ✦ 思考中 / 已思考 4 秒 · 高, body scrolls while running
    Text,       // the answer: paragraphs, "- " bullets, **bold**
    Steps,      // tool timeline (card); collapses to one summary line
    Plan,       // 整理方案 card with groups
    Confirm,    // amber confirmation, decided → one quiet line
    Result,     // 已整理 … with folder tags, 撤销 / 在 Pulse 中查看
    Acts,       // copy / retry / thumb + meta under a finished answer
    Working,    // ✦ shimmer line while waiting for the next thing
    Note,       // centred line between rules (你撤销了这次整理)
    Error,      // the turn failed: tile + title + sub (+ detail, hint) + buttons
    Cut,        // amber strip under a partial answer: 回答中途断开了 · 重试
    Query,      // 搜索条件 card: condition chips, count, where the results are
    Follow,     // 接着做: chips that put a next step into the composer
    Rename,     // 重命名方案 card: rule, old → new rows, checks, foot
    Task,       // 任务计划 card: numbered steps (they are the progress), summary
    Disk,       // 本地磁盘 (C:) card: used / free, segmented bar, the biggest places
    Clean,      // 清理建议 card: categories with boxes, reasons, 查看, the total
    Funnel,     // 比对过程 card: three local stages with counts and bars (flow E)
    Dup,        // 重复文件 card: keep rules, groups (保留 / 副本), the total
    Stopped,    // 这个版本你中途停止了 · 继续生成 under a partial answer (55 P2)
};

// Error card tone: Err needs the user (red, red border), Warn is temporary
// (amber), Accent is not an error (没连接模型).
enum class AiErrTone : uint8_t { Err, Warn, Accent };

// What an error card button does (app/ai_assistant_chat.cpp).
enum class AiErrAct : uint8_t { Retry, ManageConn, TopUp, SwitchModel, CopyDetail, Connect,
                               // A paused task (flow C): 重试 / 跳过它继续 / 撤销已做的.
                               TaskRetry, TaskSkip, TaskUndo,
                               // Voice input (flow V): 添加语音连接 / 打开 Windows 设置 / 再试一次 / 管理连接.
                               VoiceAdd, VoiceSettings, VoiceRetry, VoiceManage };

struct AiErrButton {
    AiErrAct act = AiErrAct::Retry;
    AiIcon icon = AiIcon::Retry;
    std::wstring text;
    bool primary = false;
};

enum class AiStepState : uint8_t { Running, Done, Failed };

struct AiStepView {
    AiIcon icon = AiIcon::Search;
    std::wstring title;
    std::wstring arg;                 // grey pill after the title
    std::wstring sub;
    AiStepState state = AiStepState::Running;
    float progress = -1.0f;           // 0..1 shows the bar
    uint64_t born_ms = 0;             // msg-in
    uint64_t done_ms = 0;             // the check pops
};

struct AiGroupView {
    AiIcon icon = AiIcon::Image;
    AiTint tint = AiTint::Image;
    std::wstring name;
    std::wstring count;               // 3 个
    std::wstring list;                // a、b、c (ellipsized)
    std::wstring badge;               // 新建 (empty: none)
    uint64_t in_ms = 0;               // 0: not shown yet (keeps its space)
};

struct AiTagView {
    AiTint tint = AiTint::Neutral;
    std::wstring text;                // 图片 3
    std::wstring bold;                // Query locations: the count after the name
};

// A search condition on the Query card: 名称 含「发票」 ×.
struct AiQueryChipView {
    std::wstring key;                 // 名称 (accent, semibold)
    std::wstring value;               // 含「发票」
    bool fixed = false;               // no × (所有位置, the last condition)
    uint64_t in_ms = 0;               // pops in (0: no entrance)
    uint64_t out_ms = 0;              // removed: shrinks and fades, then goes
};

// 接着做: one chip that fills the composer.
struct AiFollowView {
    AiIcon icon = AiIcon::Package;
    std::wstring text;                // 打包这 7 个
}; 

// 重命名方案 (flow B). The rule as chips (tokens), literal text (mono) and
// plain words (换成, 开头加 in text3).
struct AiRuleToken {
    std::wstring text;
    bool token = false;
    bool word = false;
};

// One old → new row; the new stem in parts (token parts in accent).
struct AiRenameRow {
    std::wstring old_name;
    std::vector<AiRuleToken> parts;
    std::wstring ext;                 // ".jpg" (shown in text3)
    bool warn = false;                // amber dot: no date taken
    bool sample = false;              // shown while the list is folded
    uint64_t in_ms = 0;               // slides in (0: not yet)
};

struct AiCheckView {
    std::wstring text;
    bool warn = false;
};

// 任务计划 (flow C): one numbered step. The card is the task's progress.
enum class AiTaskState : uint8_t { Todo, Running, Done, Paused };

struct AiTaskStepView {
    std::wstring title;               // 新建月份文件夹
    std::wstring arg;                 // grey pill: 文档 › 发票
    std::wstring tag;                 // 只读 · 已完成 / 跳过 1 个
    bool tag_warn = false;            // amber tag (else ok on accent-soft)
    std::wstring desc;                // 6 个：2026-01、02…
    AiTaskState state = AiTaskState::Todo;
    float progress = 0.0f;            // the bar, while running or paused
    uint64_t state_ms = 0;            // the circle's colour change
};

// The summary line under the steps: icon, then before **bold** after.
struct AiTaskSumView {
    AiIcon icon = AiIcon::FolderPlus;
    std::wstring before, bold, after;
};

// 任务完成: one line per step (warn = amber, a step that skipped items).
struct AiResultLine {
    bool warn = false;
    bool mute = false;                // P3: a step that was not done (text3, a dash)
    std::wstring before, bold, after;
    // P4: key caps after `before` (改快捷键: 新建文件夹 Ctrl Shift N → Ctrl Shift F).
    std::vector<std::wstring> caps_from, caps_to;
};

// Third batch. A result card's 快捷键 row: what was done and the user's own
// keys for it (key caps, modifiers first).
struct AiKeyRowView {
    std::wstring label;               // 左右分屏
    std::vector<std::wstring> caps;   // Ctrl / Alt / 2
    // P3: no key for it: the caps are what to type in the command palette
    // (>详情面板), drawn as accent pills; a row of only these shows a search icon.
    bool palette = false;
};

// A result card's 设置 row: name, the old value (struck through) → the new
// one (accent chip), an amber tag after the name (影响系统).
struct AiChangeView {
    std::wstring name, from, to, tag;
};

// 本地磁盘 (C:) card (second batch, flow D). Numbers in GB; the used and
// free figures count from *_from to their value over .9 s from count_ms.
struct AiDiskRowView {
    bool file = false;                // hiberfil.sys: the file icon
    bool sys = false;                 // grey bar, 不动 in the sub line
    std::wstring name;                // 下载 / AppData › Local
    std::wstring sub;                 // 用户 › SS / 系统 · 不动
    std::wstring value;               // 31.2
    float frac = 0.0f;                // bar width against the biggest row
};

struct AiDiskView {
    float used_gb = 0.0f, free_gb = 0.0f;
    float used_from = 0.0f, free_from = 0.0f;
    uint64_t count_ms = 0;
    bool free_ok = false;             // after a purge the free figure turns ok
    std::wstring used_of;             // GB 已用 / 238 GB
    std::wstring free_unit;           // GB 可用
    float seg[5]{};                   // share of the drive: 系统 程序 用户 其他 回收站中
    std::wstring legend[4];           // 系统 52.2 …
    std::wstring legend_recycle;      // 回收站中 11.7 (shown while seg[4] > 0)
    std::wstring legend_free;         // 可用 (the free figure follows)
    std::wstring top_label;           // 最占地方的
    std::vector<AiDiskRowView> rows;
    uint64_t grow_ms = 0;             // the bar grows, then the rows come in
};

// The small disk box on a result card: 「C 盘可用 17.0 GB」 (or 「17.0 GB →
// 28.7 GB」 counting), the used / recycled bar and its legend.
struct AiMiniDiskView {
    bool on = false;
    std::wstring label;               // C 盘可用
    std::wstring from;                // 17.0 GB → (empty: no arrow)
    float value = 0.0f, value_from = 0.0f;
    uint64_t count_ms = 0;
    std::wstring unit;                // GB
    float used = 0.0f;                // share of the drive in use, without the bin part
    float recycle = 0.0f, recycle_from = 0.0f;   // hatched part, tweens from recycle_from
    std::wstring leg_used, leg_recycle, leg_free;
};

// 清理建议 card: one category per row.
struct AiCleanFileView {
    std::wstring name;                // DockerDesktopInstaller.exe
    std::wstring size;                // 612 MB
    bool on = true;                   // off: grey, 留着
    AiIcon icon = AiIcon::File;
    AiTint tint = AiTint::Neutral;
};

struct AiCleanRowView {
    AiIcon icon = AiIcon::Install;
    AiTint tint = AiTint::App;
    std::wstring title;               // 装过的安装包
    std::wstring tag;                 // 看一眼 (amber), may be empty
    std::wstring count, size;         // 23 · 6.8 (个 / GB come from the card)
    std::wstring desc;                // 下载 · 对应的程序都已经装好了
    std::wstring link;                // 查看
    bool on = true;
    bool open = false;                // the file list below is unfolded
    std::vector<AiCleanFileView> files;
    std::wstring more;                // 还有 18 个 · 右边已列出全部
    uint64_t in_ms = 0;               // fades in
    uint64_t bump_ms = 0;             // the numbers changed
};

struct AiCleanView {
    std::vector<AiCleanRowView> rows;
    std::wstring unit_n, unit_gb;     // 个 / GB after a row's numbers
    std::wstring sum_pre, sum_mid, sum_unit;   // 选中 … 个 · … GB
    std::wstring sum_n, sum_gb;
    uint64_t sum_bump_ms = 0;
    std::wstring safe;                // 移到回收站，可撤销
    std::wstring note;                // 不碰系统文件和程序目录。…
    std::wstring keep;                // 留着 (a file left out)
};

// 比对过程 card (flow E): one row per stage. The number counts from 0 to
// `value` over `count_dur` ms from `count_ms`; the bar grows to `bar`.
enum class AiFunnelState : uint8_t { Wait, Run, Ok };

struct AiFunnelStageView {
    std::wstring label;               // 按大小分组
    std::wstring sub;                 // 6,842 个文件里大小相同的
    std::wstring unit;                // 个 / 个 · 41 组
    std::wstring time;                // 0.1 秒 · 索引
    uint64_t value = 0;
    uint64_t count_ms = 0;
    uint32_t count_dur = 650;
    float bar = 0.0f;                 // 0..1 of the row
    AiFunnelState state = AiFunnelState::Wait;
    uint64_t state_ms = 0;
};

struct AiFunnelView {
    std::vector<AiFunnelStageView> stages;
    std::wstring foot;                // 全部在这台电脑上算，文件内容不发给模型
};

// 重复文件 card (flow E): the rules, up to four groups, the total.
struct AiDupFileView {
    std::wstring dir;                 // 图片 › 2026 杭州 ›  (text3)
    std::wstring name;                // IMG_2041.jpg
    std::wstring date;                // 10月9日
    bool keep = false;                // 保留 (else 副本, struck through)
    uint64_t swap_ms = 0;             // the badge just changed (flash)
};

struct AiDupGroupView {
    AiIcon icon = AiIcon::File;
    AiTint tint = AiTint::Neutral;
    std::wstring name;                // IMG_2041.jpg
    std::wstring meta;                // 3 份 · 每份 4.2 MB
    std::wstring why;                 // 按 ①：留在整理过的文件夹里的
    bool both = false;                // 都留 (amber tag and line)
    std::vector<AiDupFileView> files;
    std::wstring more;                // 还有 3 份
    uint64_t in_ms = 0;
};

struct AiDupView {
    std::vector<std::wstring> rules;  // 在整理过的文件夹里 / 名字不带 (1)、副本 / 最早的那份
    std::wstring change;              // 改规则
    uint64_t rule_ms = 0;             // the third rule changed (flash)
    std::wstring both_tag;            // 都留
    std::wstring keep_tag, extra_tag; // 保留 / 副本
    std::vector<AiDupGroupView> groups;
    std::wstring more;                // 还有 37 组，都按同样的规则
    std::wstring more_link;           // 在 Pulse 中查看全部
    // .cl-sum: text runs, numbers in the 15/600 face.
    std::vector<std::pair<std::wstring, bool>> sum;
    uint64_t sum_bump_ms = 0;
    std::wstring safe;                // 每组至少留 1 份
    bool settled = false;             // moved: the copies fade to .45
};

// P4c .cf-rows: one item of a file step's card, and where it goes (→
// D:\财务\2026, → 新名字) or its size (88.0 MB).
struct AiConfirmRow {
    std::wstring name, to;
    bool folder = false;
};

struct AiBlockView {
    AiBlockKind kind = AiBlockKind::Text;
    uint32_t id = 0;                  // stable while the block lives (motion)
    uint64_t born_ms = 0;             // msg-in; 0 = no entrance
    std::wstring text;                // User / Text / Working / Note
    std::wstring context;             // User: chip above the bubble
    bool streaming = false;           // Text: caret after the last character
    bool running = false;             // Thinking / Working
    bool open = false;                // Thinking / Steps (collapsed) expanded
    bool collapsed = false;           // Steps: summary line; Plan: header only
    std::wstring label;               // Thinking header; Steps summary; Plan title
    std::wstring meta;                // Thinking " · 高"; Plan / Acts meta
    std::wstring seconds;             // Thinking: "3 秒" while running
    std::wstring title, sub, hint;    // Confirm / Result
    std::vector<AiStepView> steps;
    std::vector<AiGroupView> groups;
    std::vector<AiTagView> tags;
    std::wstring deny, accept;        // Confirm buttons; Result: undo, show
    bool done = false;                // Confirm decided; Result undone
    bool declined = false;            // Confirm: the user said no (or it stopped): a muted ✕, not ✓
    // Error: tone, tile icon, the service's own words (mono box) and buttons.
    AiErrTone tone = AiErrTone::Err;
    AiIcon icon = AiIcon::AlertCircle;
    std::wstring detail;
    std::vector<AiErrButton> actions;
    uint64_t gone_ms = 0;             // Error / Cut: fading out (retry)
    // Query: title 搜索条件, meta "Pulse 索引 · 0.02 秒", tags = locations,
    // label = "个文件 · 4.2 MB" after the count, hint = 本机重新搜索… (shown
    // from re_ms), add = 条件. Follow: label 接着做 and the chips.
    std::vector<AiQueryChipView> chips;
    std::vector<AiFollowView> follow;
    std::wstring count;               // 7
    std::wstring add;
    uint64_t bump_ms = 0;             // the count changed (bump)
    uint64_t re_ms = 0;               // 0: no rerun note
    // Confirm: a link button on the left (在批量重命名中调整…) and the line
    // that opens under the card while it is hovered.
    std::wstring extra, extra_tip;
    // Result: the icon of the second button (在 Pulse 中查看 / 在批量重命名中打开).
    AiIcon accept_icon = AiIcon::Open;
    // Rename: label 重命名方案, meta 8 个文件, rule_label 规则 + rule, hint
    // 扩展名不变, rows (folded: the sample ones, then `more`; open: all,
    // then `less`), checks, sub = the foot line. `collapsed` folds the card.
    std::wstring rule_label, more, less;
    std::vector<AiRuleToken> rule;
    std::vector<AiRenameRow> rows;
    std::vector<AiCheckView> checks;
    // Task: label 任务计划, meta 4 步 · 第 1 步已完成, the steps, the summary
    // and `safe` (不删除 · 不覆盖, shield in ok). Result: `lines` under the
    // title. Error (a paused task): `file` + `file_note` (.e-f row); `done`
    // folds it to the chosen line. Confirm: the hint's icon.
    std::vector<AiTaskStepView> task;
    std::vector<AiTaskSumView> sums;
    std::wstring safe;
    std::vector<AiResultLine> lines;
    std::wstring file, file_note;
    AiIcon hint_icon = AiIcon::Eye;
    // Second batch, flow D. Disk: label 本地磁盘 (C:), meta Pulse 索引 · 0.4
    // 秒. Clean: label 清理建议, meta 只读分析…. Confirm: `tag` (删除类, red)
    // after the title, `hint2` a second hint (ok check, or amber warn when
    // `hint2_warn`), `danger` the red card, `accept_off` a disabled primary.
    // Result: `mini` the disk box, `purge` a third (red) button, `gone` (the
    // batch was deleted for good: no buttons, no disk box, no warn lines),
    // `freed` (no buttons; the ok icon).
    AiDiskView disk;
    AiCleanView clean;
    std::wstring tag;
    std::wstring hint2;
    bool hint2_warn = false;
    bool danger = false;
    bool accept_off = false;
    AiMiniDiskView mini;
    std::wstring purge;
    bool gone = false;
    bool freed = false;
    // Flow E. Funnel / Dup cards; Confirm: `hint1` a line between hint and
    // hint2 with its own icon (移到回收站，可以撤销).
    AiFunnelView funnel;
    AiDupView dup;
    std::wstring hint1;
    AiIcon hint1_icon = AiIcon::Undo;
    // Third batch. Result: `keys` (.r-tip, hidden once undone), `changes`
    // (.r-chg; undone: the old value comes back, the new one is struck) and
    // `warn` (.r-warn, an amber icon and text2). Error: `pills` (.deny-l)
    // under the hint, whose icon is `hint_icon` when `pills` is not empty.
    std::vector<AiKeyRowView> keys;
    std::vector<AiChangeView> changes;
    std::wstring warn;
    std::vector<std::wstring> pills;
    // Flow V. Error: `checklist` (.e-l: ok rows text2 with a check, `warn`
    // rows text with a red ×) between the sub and the hint, and `note` (.e-m)
    // after the buttons in any tone.
    std::vector<AiCheckView> checklist;
    std::wstring note;
    // P3. Result: `partial` some step failed
    // (the amber warn icon). Confirm: `cf_icon` the tile's icon, `tag_amber`
    // the tag in amber (外部效果), `file` + `file_note` the .cf-file row.
    bool partial = false;
    AiIcon cf_icon = AiIcon::Shield;
    bool tag_amber = false;
    // P4c. Confirm: `cf_rows` the items
    // (.cf-rows, at most 3), `cf_more` its grey last row (还有 3 个 · 共 18.4
    // MB), `cf_bin` the accent line with a check under it (.cf-bin: 回收站放得
    // 下…). `arm`: the card takes two clicks (a permanent delete); the first
    // swaps it into `accept` (再点一次，永久删除) and sets `armed`.
    std::vector<AiConfirmRow> cf_rows;
    std::wstring cf_more, cf_bin, arm;
    bool armed = false;
    // 55 P2. Acts: `ver` / `ver_count` the ‹ 2 / 3 › of the turn's versions
    // (shown when ver_count > 1), `ver_only` a row that has nothing else (a
    // version that failed), `rating` 👍 1 / 👎 -1 (local only). User:
    // `edited` the text differs from the first version (已编辑).
    int ver = 0;
    int ver_count = 0;
    bool ver_only = false;
    int rating = 0;
    bool edited = false;
};

// 语音输入: the composer while the microphone listens, and the mic button.
struct AiVoiceView {
    bool available = false;           // the mic button is there
    bool listening = false;           // .composer.listen: the filled mic and its ring
    bool menu_open = false;           // the chevron's menu
    uint64_t shake_at = 0;            // the mic shakes (a precheck failed), ms
    std::array<float, 14> levels{};   // the waveform, oldest first, 0..1
    std::wstring time;                // 0:03
    std::wstring service;             // 硅基流动 · SenseVoiceSmall
    std::wstring hint;                // 说完停顿一下就会识别，或再点一次麦克风
    bool recognizing = false;         // 识别中… after the text
    std::wstring recognizing_text;
    std::shared_ptr<const AiVoiceMenuView> menu;   // non-null while the menu is open
};

// The status bar chip (left end): "✦ AI" plus what the assistant is doing.
enum class AiChipState : uint8_t { Idle, Busy, Waiting, Done, Error, Warn };

struct AiStatusChipView {
    std::wstring label;               // AI
    std::wstring text;                // 思考中 / 读取中 9/12 / 等你确认 / 完成
    bool words = false;               // room for `text`
    AiChipState state = AiChipState::Idle;
    float progress = -1.0f;           // 0..1 shows the small bar
    bool open = false;
    bool unread = false;
    bool hover = false;
    bool pressed = false;
    bool dark = false;
};

// Chip geometry shared by layout and drawing (DIPs scaled): padding 0 10 0 8,
// spark 15, gap 6, bar 38.
float AiStatusChipWidth(float label_w, float text_w, bool words, bool progress, float scale);

// .peek: above the status bar chip when the panel is closed and the
// assistant finished (or needs you); 撤销 and 打开对话.
struct AiPeekView {
    bool on = false;
    std::wstring title;               // Pulse 助手 · 完成
    std::wstring text;                // 已把 12 个文件整理到 5 个新文件夹：…
    std::wstring undo;                // 撤销 (empty: no button)
    std::wstring open;                // 打开对话
    // A failed turn: the spark takes the tone; `undo` is then 重试 (primary,
    // retry icon) or 连接模型.
    bool failed = false;
    AiErrTone tone = AiErrTone::Err;
};

// How the model takes 思考 (app/ai_providers.h ThinkKind).
enum class AiEffortMode : uint8_t { Levels, Toggle, Fixed };

// 55 P3 历史对话: one saved conversation in the list (grouped in order: a
// row with `group` set starts 已固定 / 今天 / 昨天 / 7 天内 / 更早).
struct AiHistoryRowView {
    std::wstring group;           // the heading above this row (empty: same group)
    std::wstring title, snippet, when;
    bool pinned = false;
    bool current = false;         // the conversation in the panel
};

struct AiHistoryView {
    bool open = false;
    std::wstring search_cue, query;   // query: drawn while the search EDIT is hidden
    std::wstring empty;               // no rows: 还没有保存的对话 / 没有找到相关对话
    std::wstring foot, settings;      // 🔒 对话只保存在这台电脑… · 历史设置
    std::wstring rename_hint, delete_note, cancel, del;
    std::vector<AiHistoryRowView> rows;
    int renaming = -1;                // row whose title is an EDIT
    std::wstring rename_text;         // drawn while that EDIT is hidden
    int deleting = -1;                // row in its inline confirmation
};

struct AiPanelViewModel {
    bool dark = false;
    bool high_contrast = false;
    // Header.
    std::wstring title;               // Pulse 助手
    // Empty state.
    bool empty = true;
    std::wstring greeting;            // 早上好，想整理点什么？
    std::wstring intro;
    std::vector<AiSuggestionView> suggestions;
    std::wstring safe_note;
    std::wstring jump_latest;   // 55 P4: 回到最新
    // No model service yet: the empty state shows the services and a button
    // to Settings > AI 助手 instead of the greeting and suggestions.
    bool setup = false;
    std::wstring setup_title, setup_intro, setup_more, setup_go;
    std::vector<std::string> setup_marks;   // provider ids, drawn by the mark painter
    // Conversation (shown when !empty).
    std::vector<AiBlockView> blocks;
    // Composer.
    bool context_visible = false;
    std::wstring context_text;        // 下载 · 12 项
    bool composer_focused = false;
    bool composer_empty = true;
    int composer_lines = 1;           // the EDIT's line count (input grows to 6)
    float composer_line_px = 0.0f;    // the EDIT's line height (0: the design's 21)
    // While the model menu covers the input the EDIT hides and its text is
    // drawn here instead.
    bool draw_composer_text = false;
    std::wstring composer_text;
    std::wstring placeholder;
    // 引用: the chosen words above the input (empty: none).
    std::wstring quote;
    std::wstring add_files;
    std::wstring model_label;         // deepseek-flash
    std::wstring effort_label;        // " · 高", " · 始终思考" (empty when thinking is off)
    AiSendState send = AiSendState::Dim;
    // 思考强度 popover over the model button, as the model takes it: its
    // levels on the slider, a switch, or 始终思考 with no control. A model
    // without thinking has none: the button opens the model list (a FluentMenu).
    bool menu_open = false;
    AiEffortMode effort_mode = AiEffortMode::Levels;
    std::array<std::wstring, 7> effort_names;     // Fixed: [0] 始终思考
    std::array<std::wstring, 7> effort_details;   // each level's line; Fixed: [0]
    std::wstring effort_note;         // under the line: 这个模型始终会思考… (may be empty)
    std::wstring effort_toggle_label; // Toggle: the switch's 深度思考
    int effort_count = 4;             // stops (2..7; a switch has 2), 0 for Fixed
    int effort = 2;                   // the selected stop
    int effort_default = 2;           // the stop ↺ goes back to
    bool connected = false;
    AiVoiceView voice;
    // Pointer.
    AiPanelHit hover;
    std::wstring edited_label;        // 已编辑 under a bubble whose text was changed
    // Editing a message in place (55 P2): its block (-1: none), the EDIT's
    // line count, its words (drawn while the EDIT is hidden), the note and
    // the buttons.
    int edit_block = -1;
    int edit_lines = 1;
    std::wstring edit_text, edit_warn, edit_cancel, edit_send;
    int hover_turn = -1;              // user block whose turn is under the pointer (55 P2: 复制 · 编辑)
    AiPanelHit pressed;
    AiPeekView peek;
    AiHistoryView history;            // 55 P3
};

class AiPanelView {
public:
    AiPanelView();
    AiPanelView(const AiPanelView&) = delete;
    AiPanelView& operator=(const AiPanelView&) = delete;
    ~AiPanelView();

    // Opens, closes or moves the panel; `animate` is ignored when the system
    // turns animations off. The docked width and the floating fade each run
    // to their own target, so switching modes crosses over.
    void SetState(bool open, AiPanelMode mode, bool animate);
    bool IsOpen() const noexcept { return open_; }
    // New chat: the empty state's spark breathes again.
    void Rewake(uint64_t now_ms);
    AiPanelMode Mode() const noexcept { return mode_; }
    // Something of the panel is on screen (open, or still closing).
    bool Visible() const noexcept { return dock_value_ > 0.0f || float_value_ > 0.0f; }
    bool FloatingVisible() const noexcept { return float_value_ > 0.0f; }

    // Once per frame before layout: samples the motion. True while it runs.
    bool Tick(uint64_t now_ms);
    bool Animating() const noexcept { return dock_anim_ || float_anim_ || conv_anim_ || peek_anim_; }
    // Decorative motion of the open panel (the breathing spark, spinners).
    bool WantsDecorFrame() const noexcept { return (open_ && (decor_ || conv_decor_)) || chip_decor_; }

    // Width of the docked column this frame, in px (0 when floating or closed).
    float DockWidth(float window_width, float rail_width, float scale) const;
    // The full docked width the column grows to, in px.
    static float DockTargetWidth(float window_width, float rail_width, float scale);

    // `column`: docked column this frame (may be narrower than the content
    // while it opens). `floating_area`: the area a floating card sits in
    // (main area between title bar and status bar).
    // `column_content_width`: the full docked width the content is laid out
    // at (the column clips it while it grows).
    void Draw(Compositor* compositor, const AiPanelViewModel& vm, const Theme& theme,
              const D2D1_RECT_F& column, float column_content_width, const D2D1_RECT_F& floating_area,
              float scale, uint64_t now_ms);

    // Conversation scrolling (px). Follows the newest block until the user
    // scrolls up; scrolling back to the end follows again.
    void ScrollBy(float dy, bool animate = true);
    void ScrollToEnd();
    bool JumpShown() const noexcept { return jump_shown_; }   // 55 P4
    bool Following() const noexcept { return follow_; }
    // The overlay scrollbar (conversation or welcome) works like Pulse's other scrollbars: drag the thumb, or press the track
    // to jump there and keep dragging. Hover widens the bar.
    bool BeginScrollDrag(float x, float y);   // true when a bar took the press
    bool DragScroll(float y);                 // true when the view moved
    void EndScrollDrag() noexcept { scroll_drag_ = 0; }
    int ScrollDragging() const noexcept { return scroll_drag_; }   // 1 conversation
    bool SetScrollHot(float x, float y);      // true when the hover changed
    bool ScrollHot() const noexcept { return scroll_hot_ != 0; }

    // The 思考强度 slider: a press on it starts a drag, the knob follows the
    // pointer and the level under it shows; the release snaps to a stop.
    bool BeginSliderDrag(float x, float y);   // true when the press is on the slider
    int DragSlider(float x);                  // the stop under the knob (-1: no drag)
    int EndSliderDrag();                      // the stop it snapped to (-1: no drag)
    bool SliderDragging() const noexcept { return slider_drag_; }
    // The model list's anchor in the popover (「model ›」; the other is
    // ModelButtonRect).
    D2D1_RECT_F ModelLinkRect() const noexcept { return menu_link_; }

    // Last drawn geometry.
    AiPanelHit HitTest(float x, float y) const;
    bool Contains(float x, float y) const;
    D2D1_RECT_F InputRect() const noexcept { return input_rect_; }
    // The composer's input is visible and settled (place the EDIT there).
    bool InputPlaced() const noexcept { return input_placed_; }
    D2D1_COLOR_F CardColor() const noexcept { return card_color_; }
    // Set by the renderer before Draw: the pane card's opacity over a
    // wallpaper or Mica (`on`) and its frame color. The docked column then
    // draws as the same translucent card; the composer rests translucent and
    // turns solid while its EDIT has focus.
    void SetGlass(bool on, float card_alpha, D2D1_COLOR_F frame) noexcept {
        glass_ = on;
        glass_alpha_ = card_alpha;
        glass_frame_ = frame;
    }
    // The docked column was last drawn as glass.
    bool Glass() const noexcept { return glass_drawn_; }
    D2D1_COLOR_F BgColor() const noexcept { return bg_color_; }
    D2D1_COLOR_F TextColor() const noexcept { return text_color_; }
    D2D1_COLOR_F PlaceholderColor() const noexcept { return text3_color_; }
    // The composer's text face (13.5), for the LumaText-presented EDIT.
    IDWriteTextFormat* ComposerFormat() const noexcept { return formats_.body; }
    D2D1_RECT_F ModelButtonRect() const noexcept { return model_button_; }
    // Draws a service's logo (MainRenderer::DrawProviderMark), set before Draw.
    void SetMarkPainter(std::function<void(std::string_view, const D2D1_RECT_F&)> painter) {
        mark_painter_ = std::move(painter);
    }
    // Where the microphone dropdown opens: the menu's box (VoiceMicPick),
    // last frame.
    bool VoiceAnchor(AiPanelPart part, D2D1_RECT_F& rect) const;

    // The status bar chip; its motion (spinning spark, amber ring) asks for
    // decorative frames like the open panel does.
    void DrawStatusChip(Compositor* compositor, const Theme& theme, const AiStatusChipView& chip,
                        const D2D1_RECT_F& rect, float scale, uint64_t now_ms);
    // The peek bubble over the chip (`chip`: its rect this frame); it fades
    // and rises in and out on its own (ai_panel_peek.cpp).
    void DrawPeek(Compositor* compositor, const AiPanelViewModel& vm, const Theme& theme, const D2D1_RECT_F& chip,
                  float scale, uint64_t now_ms);
    AiPanelHit PeekHitTest(float x, float y) const;
    bool PeekVisible() const noexcept { return peek_value_ > 0.0f; }

    void ReleaseDeviceResources();

    // Text selection over answers and messages (ai_panel_rich.cpp). A press
    // starts it (clicks: 1 characters, 2 a word, 3 a paragraph; extend: Shift
    // moves its end), the drag follows the nearest text and the release shows
    // the mini toolbar. Buttons, icons and line numbers are never selected.
    bool OverText(float x, float y) const;     // selectable text under the point
    int UserTurnAt(float x, float y) const;    // the user block whose turn is there (-1: none)
    D2D1_RECT_F ConvHitRect(AiPanelHit hit) const;   // where a conversation part was drawn (empty: not shown)
    // The in-place edit's text area and whether all of it is in view (the
    // EDIT is shown over it only then).
    D2D1_RECT_F TurnEditRect() const noexcept { return turn_edit_rect_; }
    bool TurnEditPlaced() const noexcept { return turn_edit_placed_; }
    void RevealTurnEdit();   // scrolls the whole edit box into view
    // 55 P3 历史对话 (ai_panel_history.cpp): how far the layer slid in (0..1),
    // the search box's and the renamed title's text areas (the app puts its
    // EDITs there once the layer stands still), and the list's scrolling.
    float HistoryAmount() const noexcept { return hist_k_; }
    // The layer is over this point (what is under it takes no pointer).
    bool HistoryCovers(float x, float y) const noexcept {
        return hist_k_ > 0.001f && x >= hist_rect_.left && x < hist_rect_.right && y >= hist_rect_.top &&
               y < hist_rect_.bottom;
    }
    bool HistorySettled() const noexcept { return hist_k_ >= 0.999f && hist_open_drawn_; }
    D2D1_RECT_F HistorySearchRect() const noexcept { return hist_search_rect_; }
    D2D1_RECT_F HistoryRenameRect() const noexcept { return hist_rename_rect_; }
    bool HistoryScrollBy(float dy);   // false: the layer is not there
    void HistoryRevealRow(int row);   // scrolls a row into view
    void HistoryScrollTop() noexcept { hist_scroll_ = 0.0f; }
    bool BeginSelect(float x, float y, int clicks, bool extend);
    void DragSelect(float x, float y);
    void EndSelect();
    bool Selecting() const noexcept;
    bool HasSelection() const;
    void ClearSelection();
    void HideSelectionBar();
    // The answer (its Text blocks back to the message) or the message holding
    // `block`; -1: the last answer.
    bool SelectAnswer(int block);
    int CaretBlock() const;   // the block last clicked in (a selection or just a caret), else -1
    int RichBlockAt(float x, float y) const;   // block index of the text nearest the point, -1
    int SelectionBlock() const;                // block index of the selection's end, -1
    std::wstring SelectionText(bool markdown) const;
    // While a drag is past the conversation's top or bottom: px to scroll a tick.
    float SelectScrollSpeed(float y) const;
    // A link, path, code block or table under a RichLink / RichPath /
    // CodeCopy / TableCopy hit: its URL, path, code or tab separated cells.
    struct RichTargetInfo {
        AiPanelPart part = AiPanelPart::None;
        std::wstring text;
    };
    RichTargetInfo TargetOf(const AiPanelHit& hit) const;
    // A copy button shows 已复制 for 2 s.
    void MarkCopied(const AiPanelHit& hit, uint64_t now_ms);
    // A code block or table under the point scrolls sideways; false elsewhere.
    bool ScrollBoxAt(float x, float y, float dx);

    struct Palette;   // colors of one frame (ai_panel_view.cpp)

private:
    struct Formats {
        float scale = 0.0f;
        float font_scale = 0.0f;
        IDWriteTextFormat* title = nullptr;     // 14.5 semibold
        IDWriteTextFormat* greeting = nullptr;  // 21 semibold
        IDWriteTextFormat* body = nullptr;      // 13.5
        IDWriteTextFormat* body13 = nullptr;    // 13
        IDWriteTextFormat* body13b = nullptr;   // 13 semibold
        IDWriteTextFormat* caption = nullptr;    // 12
        IDWriteTextFormat* small125 = nullptr;  // 12.5
        IDWriteTextFormat* small125b = nullptr; // 12.5 semibold
        IDWriteTextFormat* model = nullptr;     // 12.5 medium
        IDWriteTextFormat* heading = nullptr;   // 11.5 semibold
        IDWriteTextFormat* body_b = nullptr;    // 13.5 semibold
        IDWriteTextFormat* caption_b = nullptr; // 12 semibold
        IDWriteTextFormat* badge = nullptr;     // 10.5 semibold
        IDWriteTextFormat* meta = nullptr;      // 11.5
        IDWriteTextFormat* mono = nullptr;      // 11.5 Cascadia Mono / Consolas
        IDWriteTextFormat* mono125 = nullptr;   // 12.5 (the rename rule's literal text)
        IDWriteTextFormat* num22 = nullptr;     // 22 semibold (the Query card's count)
        IDWriteTextFormat* num20 = nullptr;     // 20 semibold (the disk card's used figure)
        IDWriteTextFormat* num16 = nullptr;     // 16 semibold (a result card's free figure)
        IDWriteTextFormat* num15 = nullptr;     // 15 semibold (the cleanup total)
        IDWriteTextFormat* num14 = nullptr;     // 14 semibold (the disk card's free figure)
        IDWriteTextFormat* small11 = nullptr;   // 11 (sub lines, the small legend)
        IDWriteTextFormat* small11b = nullptr;  // 11 semibold (key caps)
        void Release();
    };

    void EnsureFormats(Compositor* compositor, float scale);
    void DrawContent(Compositor* compositor, const AiPanelViewModel& vm, const Palette& pal,
                     const D2D1_RECT_F& panel, float scale, uint64_t now_ms);
    void DrawHeader(Compositor* compositor, const AiPanelViewModel& vm, const Palette& pal,
                    const D2D1_RECT_F& panel, float scale);
    void DrawEmptyState(Compositor* compositor, const AiPanelViewModel& vm, const Palette& pal,
                        const D2D1_RECT_F& area, float scale, uint64_t now_ms);
    void DrawConversation(Compositor* compositor, const AiPanelViewModel& vm, const Palette& pal,
                          const D2D1_RECT_F& area, float scale, uint64_t now_ms);
    float ComposerHeight(const AiPanelViewModel& vm, float scale) const;
    void DrawComposer(Compositor* compositor, const AiPanelViewModel& vm, const Palette& pal,
                      const D2D1_RECT_F& box, float scale, uint64_t now_ms);
    void DrawEffortMenu(Compositor* compositor, const AiPanelViewModel& vm, const Palette& pal,
                        float scale, uint64_t now_ms);
    // ai_panel_voice.cpp: the mic button (in the composer row), the composer
    // while it listens and the 语音设置 menu.
    void DrawMic(Compositor* compositor, const AiPanelViewModel& vm, const Palette& pal, float scale, uint64_t now_ms);
    void DrawVoiceInput(Compositor* compositor, const AiPanelViewModel& vm, const Palette& pal, float line,
                        float scale, uint64_t now_ms);
    void DrawVoiceMenu(Compositor* compositor, const AiPanelViewModel& vm, const Palette& pal, float scale,
                       uint64_t now_ms);
    // The input's lines: the EDIT's, or while listening the recording lines.
    int InputLines(const AiPanelViewModel& vm) const;
    AiPanelHit VoiceHitTest(float x, float y) const;

    bool open_ = false;
    AiPanelMode mode_ = AiPanelMode::Docked;
    bool animate_ = true;
    // Docked width and floating fade, 0..1, each with its own spring.
    float dock_value_ = 0.0f, dock_from_ = 0.0f;
    float float_value_ = 0.0f, float_from_ = 0.0f;
    bool dock_anim_ = false, float_anim_ = false;
    uint64_t dock_start_ = 0, float_start_ = 0;
    uint64_t opened_at_ = 0;
    // 思考强度 popover pop.
    bool menu_shown_ = false;
    uint64_t menu_at_ = 0;
    bool decor_ = false;
    bool chip_decor_ = false;
    // Peek bubble.
    float peek_value_ = 0.0f;         // 0..1 shown
    bool peek_target_ = false;
    uint64_t peek_t0_ = 0;            // its last turn started
    float peek_from_ = 0.0f;
    bool peek_anim_ = false;
    D2D1_RECT_F peek_rect_{}, peek_close_{}, peek_undo_{}, peek_open_{};
    bool chip_waiting_ = false;
    uint64_t chip_since_ = 0;
    bool EnsureBrush(ID2D1DeviceContext* dc);

    // Geometry of the last frame (px).
    bool drawn_ = false;
    D2D1_RECT_F panel_rect_{};     // clipped to what is visible
    D2D1_RECT_F head_rect_{};
    D2D1_RECT_F new_button_{}, mode_button_{}, close_button_{};
    std::vector<D2D1_RECT_F> suggestion_rects_;
    D2D1_RECT_F composer_rect_{};
    D2D1_RECT_F context_chip_{}, context_remove_{};
    D2D1_RECT_F input_rect_{};
    bool input_placed_ = false;
    bool glass_ = false;
    bool glass_drawn_ = false;
    float glass_alpha_ = 1.0f;
    D2D1_COLOR_F glass_frame_{};
    D2D1_RECT_F add_files_{}, model_button_{}, send_button_{};
    D2D1_RECT_F mic_button_{}, mic_menu_{};
    // 语音输入: menu motion, hit rects and formats (ai_panel_voice.h).
    struct VoiceGeom;
    std::shared_ptr<VoiceGeom> voice_;
    D2D1_RECT_F menu_rect_{};
    D2D1_RECT_F menu_link_{}, menu_slider_{}, menu_reset_{};
    // The slider's knob, in stops: it follows a drag, then springs onto the
    // stop with a slight overshoot.
    float slider_pos_ = 0.0f, slider_from_ = 0.0f;
    int slider_target_ = -1;          // the stop it settles on (-1: not placed yet)
    int slider_stops_ = 0;            // 0 while the popover shows a switch or 始终思考
    int toggle_target_ = -1;          // the switch row: the stop a click picks
    uint64_t slider_at_ = 0;
    bool slider_anim_ = false, slider_drag_ = false;
    // The panel's scrollbars are the shared fluent control (the same look,
    // fade and drag as settings and the sidebar): the conversation or the
    // welcome. `spec` is as last drawn; `drawn` is reset each frame.
    struct Bar {
        fluent::ScrollbarSpec spec;
        fluent::ScrollbarFade fade;
        bool drawn = false;
    };
    std::array<Bar, 1> bars_{};
    int scroll_drag_ = 0;            // 1 conversation
    int scroll_hot_ = 0;
    float drag_grab_ = 0.0f;         // pointer offset along the thumb
    float bar_scale_ = 1.0f;
    uint64_t bar_now_ = 0;
    fluent::Painter bar_painter_;
    int BarAt(float x, float y) const;
    // Draws bar `which` over `viewport` (its hit zone and track) and records it.
    void DrawBar(int which, const D2D1_RECT_F& viewport, float offset, float view, float content);
    D2D1_RECT_F go_settings_{};
    // Rich text and the selection (ai_panel_rich.h).
    struct RichCache;
    std::unique_ptr<RichCache> rich_;
    D2D1_RECT_F quote_remove_{};
    void EnsureRichBrushes(ID2D1DeviceContext* dc, const Palette& pal, bool dark);
    void ReleaseRich();
    const AiRichDoc* RichFor(Compositor* compositor, const AiBlockView& block, float width, float scale);
    void BuildRich(Compositor* compositor, const AiBlockView& block, float width, float scale, AiRichDoc& doc,
                   std::unordered_map<uint64_t, Microsoft::WRL::ComPtr<IDWriteTextLayout>>& reuse);
    // Draws a laid out block at (ox, oy) and records its pieces for hit tests
    // (only the latter when `draw` is false: off screen).
    void DrawRich(Compositor* compositor, const AiRichDoc& doc, const AiBlockView& block, int index, float ox,
                  float oy, const Palette& pal, const D2D1_RECT_F& area, const AiPanelViewModel& vm, float scale,
                  uint64_t now_ms, bool draw);
    bool RichCaret(const AiRichDoc& doc, uint32_t id, float ox, float oy, float& cx, float& cy, float& lh) const;
    void BeginRichFrame(const AiPanelViewModel& vm);
    void EndRichFrame();
    float BoxScroll(uint32_t id, int box) const;
    bool RichPosAt(float x, float y, bool strict, AiRichSelPos& out) const;
    bool SelectionIn(uint32_t id, int piece, uint32_t length, uint32_t& a, uint32_t& b) const;
    int ComparePos(const AiRichSelPos& x, const AiRichSelPos& y) const;
    const AiRichPiece* PieceOf(const AiRichSelPos& p) const;
    void UnitAround(const AiRichSelPos& p, int unit, AiRichSelPos& from, AiRichSelPos& to) const;
    void DrawSelectionBar(Compositor* compositor, const AiPanelViewModel& vm, const Palette& pal, float scale);
    void DrawHistory(Compositor* compositor, const AiPanelViewModel& vm, const Palette& pal, const D2D1_RECT_F& panel,
                     float scale, uint64_t now_ms);
    AiPanelHit HistoryHitTest(float x, float y) const;
    float HistorySlide(bool open, uint64_t now_ms);   // eased 0..1 (its own: DrawConversation sweeps tweens_)
    // The composer's quote chip: its height (0 without a quote, measured
    // before the composer is placed) and drawing.
    float MeasureQuote(Compositor* compositor, const AiPanelViewModel& vm, float inner_width, float scale);
    void DrawQuoteChip(Compositor* compositor, const AiPanelViewModel& vm, const Palette& pal, float left, float right,
                       float& y, float scale);
    std::function<void(std::string_view, const D2D1_RECT_F&)> mark_painter_;
    D2D1_RECT_F conversation_rect_{};
    D2D1_COLOR_F card_color_{1, 1, 1, 1};
    D2D1_COLOR_F bg_color_{1, 1, 1, 1};
    D2D1_COLOR_F text_color_{0, 0, 0, 1};
    D2D1_COLOR_F text3_color_{0.5f, 0.5f, 0.5f, 1};

    // Conversation motion and scrolling.
    struct Tween {
        float from = 0.0f, to = 0.0f;
        uint64_t start = 0;
        uint32_t ms = 0;
        bool used = false;
    };
    // Eases the value under `key` toward `target` over `ms`; keys are block
    // id * 8 + slot. Unused keys are dropped after each frame.
    float TweenTo(uint64_t key, float target, uint32_t ms, uint64_t now_ms);
    std::vector<std::pair<uint64_t, Tween>> tweens_;
    bool conv_anim_ = false;    // tweens, entrances, scrolling: full rate
    bool conv_decor_ = false;   // spinners, caret, shimmer: decorative rate
    float scroll_ = 0.0f;       // px from the top of the content
    float scroll_from_ = 0.0f, scroll_to_ = 0.0f;
    uint64_t scroll_start_ = 0;
    bool scroll_anim_ = false;
    bool follow_ = true;
    bool was_empty_ = false;    // the welcome was drawn last frame (it starts at the top)
    float content_h_ = 0.0f, view_h_ = 0.0f;
    struct ConvHit {
        D2D1_RECT_F rect;
        AiPanelHit hit;
    };
    std::vector<ConvHit> conv_hits_;
    std::vector<std::pair<D2D1_RECT_F, int>> turn_rects_;   // user turns (bubble .. next turn), visible part
    D2D1_RECT_F turn_edit_rect_{};
    bool turn_edit_placed_ = false;
    // 55 P3 历史对话.
    D2D1_RECT_F hist_button_{};
    float hist_k_ = 0.0f;               // slid in (eased)
    float hist_from_ = 0.0f, hist_to_ = 0.0f;
    uint64_t hist_start_ = 0;
    uint32_t hist_ms_ = 0;
    bool hist_open_drawn_ = false;      // vm.history.open last frame
    D2D1_RECT_F hist_rect_{}, hist_list_rect_{}, hist_search_rect_{}, hist_rename_rect_{};
    float hist_scroll_ = 0.0f, hist_content_h_ = 0.0f;
    D2D1_RECT_F jump_rect_{};   // 55 P4: the 回到最新 pill (drawn only while scrolled up)
    bool jump_shown_ = false;
    std::vector<float> hist_row_tops_;  // content y of each row (for reveal)
    std::vector<ConvHit> hist_hits_;

    Formats formats_;
    ID2D1SolidColorBrush* brush_ = nullptr;
    ID2D1DeviceContext* brush_dc_ = nullptr;
};

} // namespace pulse::ui

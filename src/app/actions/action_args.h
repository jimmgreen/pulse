// action_args.h - Actions run with arguments (全面打通 P4).
//
// The palette runs an action as its menu row does; the assistant (and MCP in
// P5) names what to act on: which tab, which tag, what name, which keys. A
// runner here does that one action with those arguments and says what
// changed, in the words of the result card, and how to undo it. Runners only
// call what the menus and the settings page call, and never ask: the gate
// (action_policy.h) decides before a runner is reached.
#pragma once

#include "../../common/json_value.h"

#include <cstdint>
#include <functional>
#include <vector>
#include <string>
#include <string_view>

namespace pulse {
struct AppState;
}

namespace pulse::app::actions {

struct ArgRun {
    bool ok = false;
    bool skip = false;           // nothing to do: it was so already
    std::wstring title;          // the step's title when not the row's (Git 状态)
    std::string error;           // for the model (English)
    std::wstring why;            // !ok: the step card's reason
    std::wstring arg;            // the step's chip (报销, 3 个)
    std::wstring sub;            // the step's second line (蓝色 · 在「工作」分组)
    // The result card's line: before + bold + after (empty: the title).
    std::wstring line_before, line_bold, line_after;
    // Key caps for the line (keymap.set): from -> to.
    std::vector<std::wstring> caps_from, caps_to;
    bool read_only = false;      // read rows: no line, no undo
    // Restores what the action changed, if it is still as the action left it.
    std::function<void(AppState&)> undo;
    json::Value data;            // extra result fields for the model
};

// True when `id` has a runner here (the assistant then uses it, not the
// plain menu row).
bool HasArgRunner(std::string_view id) noexcept;
ArgRun RunWithArgs(AppState& s, std::string_view id, const json::Value& args);

// The confirm card of an action that asks each time (P4b): 外部效果 for Git,
// shell verbs and other programs, 影响系统 for cloud files and drives. The
// agent draws it; the runner does the action once the user accepted.
enum class AskIcon : uint8_t { System, Push, Pull, Verb, OpenWith, Cloud, Eject, Disconnect };
struct ArgAsk {
    // Skip / Fail: no card. Nothing to do, or it cannot run; the runner then
    // says so in its step (没有要推送的提交, 这里不是 Git 仓库).
    enum class State : uint8_t { Ask, Skip, Fail } state = State::Ask;
    bool external = false;       // 外部效果; else 影响系统
    AskIcon icon = AskIcon::System;
    std::wstring title, sub;     // 推送到远端？ / 会把 feat/report 上的 3 个提交推到 origin。…
    std::wstring file, file_note;  // pulse-docs · feat/report → origin / 3 个提交
    std::wstring deny, accept;   // 不推送 / 推送
};
// True when `id` has its own card words (the rest use the generic cards).
bool HasAskText(std::string_view id) noexcept;
ArgAsk AskFor(AppState& s, std::string_view id, const json::Value& args);

// An action only the user does (UserOnly): why the assistant does not, and
// where the user does it (此电脑 → 双击 E 盘). The agent's lock card.
struct ArgLock {
    std::wstring title, sub, where;
};
bool HasLockText(std::string_view id) noexcept;
ArgLock LockFor(AppState& s, std::string_view id, const json::Value& args);

// The P4b runners (action_args_ext.cpp): Git, shell verbs, toolbar apps,
// cloud files and drives. HasArgRunner / RunWithArgs include them.
bool HasExtRunner(std::string_view id) noexcept;
ArgRun RunExt(AppState& s, std::string_view id, const json::Value& args);

// Color words the runners take: red orange yellow green teal blue purple
// pink none; index into TabController::Palette(), -1 none, -2 unknown.
int ColorIndex(std::string_view word) noexcept;
std::wstring ColorName(int index);

} // namespace pulse::app::actions

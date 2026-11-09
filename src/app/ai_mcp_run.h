// ai_mcp_run.h - One MCP run_actions call:
// the broker's call thread (ai_tools_actions.cpp) hands it to the UI thread,
// which runs the steps with the assistant's runners (ai_assistant_agent.cpp)
// and asks the user with Pulse's confirm dialog instead of chat cards.
//
//  - Before any step runs, every step's group (actions/action_policy.h
//    McpGroup) must be switched on in ai.json (mcp_allow); otherwise nothing
//    runs and the call answers not_allowed.
//  - Create steps ask unless an always-allow rule covers the folder; file
//    steps and effects outside Pulse ask every time; a permanent delete takes
//    two presses. A question open for 2 minutes is closed by the call thread
//    and counts as a denial (user_timeout).
//  - What the call did is undone as one group by undo_last (`undo`).
#pragma once
#include "ai_broker.h"
#include "actions/action_policy.h"
#include "../common/json_value.h"
#include <windows.h>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pulse {
struct AppState;
}

namespace pulse::app::ai {

inline constexpr DWORD kMcpQuestionMs = 120000;

struct McpActionsRun {
    std::wstring client;
    std::vector<json::Value> steps;   // {action, args?, wait?}, at most 12
    // The switched-on groups, as ai.json said when the call came in.
    std::array<bool, actions::kMcpGroupCount> allowed{};
    // Create steps: whether a rule lets the client create in `folder` without
    // asking (tool "pulse.<action id>"), and storing one (始终允许这个文件夹).
    std::function<bool(const std::string& tool, const std::wstring& folder)> always_allowed;
    std::function<void(const std::string& tool, const std::wstring& folder)> allow_always;

    HWND window = nullptr;                // the window the steps ran in (UI thread sets it)
    std::atomic<bool> cancelled{false};   // the call is over (cancel, timeout, stop)

    std::mutex mu;   // guards the members below
    std::condition_variable cv;
    HWND dialog = nullptr;        // the question open now (UI thread sets it)
    ULONGLONG dialog_since = 0;   // GetTickCount64 when it opened
    bool timed_out = false;       // the call thread closed it after kMcpQuestionMs
    bool done = false;
    ToolOutcome outcome;
    // UI thread: what undo_last runs, in the order done (it runs them backwards).
    std::vector<std::function<void(AppState&)>> undo;
    uint64_t file_group = 0;      // the file step's undo group, 0 without one
};

// UI thread (ai_assistant_agent.cpp): runs the call; FinishMcpRun ends it.
void StartMcpRunActions(AppState& s, std::shared_ptr<McpActionsRun> run);

// Any thread: stores the outcome and wakes the call thread (once).
inline void FinishMcpRun(McpActionsRun& run, ToolOutcome outcome) {
    std::lock_guard lock(run.mu);
    if (run.done) return;
    run.outcome = std::move(outcome);
    run.done = true;
    run.cv.notify_all();
}

// The words a not_allowed error and find_actions use for a group's switch:
// its name as Pulse's settings show it (UTF-8), and where it is.
std::string McpGroupSwitchName(actions::McpGroup group);
std::string McpSwitchWhere();
// not_allowed for a step of `group` ("step 2 (tag.toggle)" in `what`).
ToolOutcome McpNotAllowed(actions::McpGroup group, const std::string& what);

} // namespace pulse::app::ai

// ai_tools_actions.cpp - pulse.find_actions and pulse.run_actions (P5).
#include "ai_tools_actions.h"
#include "../common/ai_action_request.h"
#include "ai_tools_ui.h"
#include "app_state.h"
#include "actions/action_files.h"
#include "actions/action_params.h"
#include "actions/action_policy.h"
#include "actions/action_registry.h"
#include "../common/localization.h"
#include <chrono>

namespace pulse::app::ai {
namespace {

namespace A = actions;
using I = l10n::StringId;

std::string Utf8Text(I id) { return json::Utf8(std::wstring(l10n::Get(id).c_str())); }
std::wstring Text(I id) { return l10n::Get(id).c_str(); }

std::wstring Fill(std::wstring text, std::wstring_view key, const std::wstring& value) {
    if (const size_t at = text.find(key); at != std::wstring::npos) text.replace(at, key.size(), value);
    return text;
}

ToolOutcome Cancelled() { return ToolOutcome::Fail("cancelled", "The call was cancelled."); }

ToolOutcome NoWindow() {
    return ToolOutcome::Fail("unsupported", "Pulse has no window open right now; ask the user to open Pulse.", true);
}

// The row of one action for an AI app: the assistant's row (action_params.h)
// with its group and whether the user lets AI apps run it.
json::Value ActionRow(const A::ActionView& v, const std::array<bool, A::kMcpGroupCount>& allowed) {
    const json::Value full = A::ActionJson(v);
    json::Value row = json::Value::Object();
    for (size_t i = 0; i < full.keys().size(); ++i)
        if (full.keys()[i] != "use_instead") row.set(full.keys()[i], full.items()[i]);   // the assistant's own tools
    const A::McpGroup group = A::McpGroupOf(v.id);
    row.set("group", A::McpGroupKey(group));
    const A::Policy policy = A::AiPolicy(v.id);
    if (group == A::McpGroup::Locked || policy.gate == A::Gate::Locked) {
        row.set("allowed", false);
        row.set("why_not_allowed", "only the user can do this, in Pulse itself");
    } else if (policy.gate == A::Gate::Plan && !A::IsFileOp(v.id)) {
        row.set("allowed", false);
        row.set("why_not_allowed", "not available to AI apps; the user does it in Pulse");
    } else if (!allowed[static_cast<size_t>(group)]) {
        row.set("allowed", false);
        row.set("why_not_allowed", "\"" + McpGroupSwitchName(group) + "\" is off for AI apps; the user can turn it on in " +
                                       McpSwitchWhere());
    } else {
        row.set("allowed", true);
    }
    return row;
}

} // namespace

std::string McpGroupSwitchName(A::McpGroup group) {
    switch (group) {
    case A::McpGroup::View: return Utf8Text(I::AiMcpGroupView);
    case A::McpGroup::Organize: return Utf8Text(I::AiMcpGroupOrganize);
    case A::McpGroup::Create: return Utf8Text(I::AiMcpGroupCreate);
    case A::McpGroup::Files: return Utf8Text(I::AiMcpGroupFiles);
    case A::McpGroup::External: return Utf8Text(I::AiMcpGroupExternal);
    case A::McpGroup::Locked: break;
    }
    return Utf8Text(I::AiMcpGroupLocked);
}

std::string McpSwitchWhere() { return "Pulse: " + Utf8Text(I::AiMcpWhere); }

ToolOutcome McpNotAllowed(A::McpGroup group, const std::string& what) {
    if (group == A::McpGroup::Locked)
        return ToolOutcome::Fail("locked", what + " is only the user's to do, in Pulse itself; nothing of this call was done.");
    return ToolOutcome::Fail("not_allowed", what + ": \"" + McpGroupSwitchName(group) +
                                                "\" is off for AI apps, so nothing of this call was done. The user can turn it "
                                                "on in " + McpSwitchWhere() + ". Tell the user; do not retry.");
}

std::array<bool, A::kMcpGroupCount> McpAllowedGroups(const Settings& settings) {
    std::array<bool, A::kMcpGroupCount> allowed{};
    for (size_t i = 0; i < A::kMcpGroupCount; ++i) allowed[i] = McpAllows(settings, A::McpGroupKey(static_cast<A::McpGroup>(i)));
    return allowed;
}

ToolOutcome FindActionsTool(HWND host, const ToolCall& call, const Settings& settings) {
    const auto allowed = McpAllowedGroups(settings);
    const std::string domain = call.args.get_string("domain");
    const std::wstring query = json::Wide(call.args.get_string("query"));
    if (domain.empty() && query.empty())
        return ToolOutcome::Fail("invalid_args", "pass query (a few words) or domain \"settings\"");
    json::Value rows = json::Value::Array();
    std::wstring found;   // 本次运行: the first three ids
    size_t count = 0;
    auto note = [&](const std::string& id) {
        if (count < 3) found += (count ? Text(I::AiTkListSep) : std::wstring()) + json::Wide(id);
        ++count;
    };
    bool window = false;
    const bool ran = RunOnAiUiThread(host, [&](AppState* s) {
        if (!s) return;
        window = true;
        const auto views = A::ListActions(*s);
        if (domain == "settings") {
            for (const auto& v : views)
                if (!v.key.empty()) {
                    rows.push(ActionRow(v, allowed));
                    ++count;
                }
        } else {
            for (const A::ActionView* v : A::FindActions(views, query, 8)) {
                rows.push(ActionRow(*v, allowed));
                note(std::string(v->id));
            }
        }
    }, call.cancelled);
    if (call.cancelled) return Cancelled();
    if (!ran || !window) return NoWindow();
    ToolOutcome out;
    out.data = json::Value::Object();
    const bool none = rows.size() == 0;
    out.data.set("actions", std::move(rows));
    if (none) out.data.set("note", "nothing matched; try other words (Chinese or English) or domain settings");
    // 「“移动” → file.move_to、file.copy_to、tray.send_to」 under 查找动作.
    if (domain == "settings") {
        out.summary = L"\n" + Fill(Text(I::AiMcpSumSettings), L"{n}", std::to_wstring(count));
    } else {
        out.summary = L"\n\u201C" + query + L"\u201D \u2192 " +
                      (none ? Text(I::AiMcpSumNone) : found + (count > 3 ? L"\u2026" : L""));
    }
    return out;
}

ToolOutcome RunActionsTool(HWND host, const ToolCall& call, const Settings& settings,
                           std::function<void(AllowRule)> add_rule, std::shared_ptr<McpActionsRun>& finished) {
    if (call.cancelled) return Cancelled();
    if (const auto error = pulse::ai::ValidateActionRequest(call.args); !error.empty())
        return ToolOutcome::Fail("invalid_args", error);
    auto run = std::make_shared<McpActionsRun>();
    run->client = call.client;
    if (const json::Value* steps = call.args.find("steps"); steps && steps->is_array())
        for (size_t i = 0; i < steps->size(); ++i) run->steps.push_back(steps->at(i));
    if (run->steps.empty()) return ToolOutcome::Fail("invalid_args", "steps is empty");
    run->allowed = McpAllowedGroups(settings);
    const Settings rules = settings;   // a copy: the call outlives nothing it borrows
    const std::wstring client = call.client;
    run->always_allowed = [rules, client](const std::string& tool, const std::wstring& folder) {
        return IsAlwaysAllowed(rules, PermissionClass::Create, client, tool, folder);
    };
    run->allow_always = [add_rule, client](const std::string& tool, const std::wstring& folder) {
        if (add_rule) add_rule({client, tool, folder});
    };
    if (!PostAiUiTask(host, [run](AppState* s) {
            if (!s) {
                FinishMcpRun(*run, NoWindow());
                return;
            }
            StartMcpRunActions(*s, run);
        }))
        return NoWindow();
    std::unique_lock lock(run->mu);
    bool told = false;
    while (!run->done) {
        if (run->dialog && !told && call.waiting_user) {
            told = true;
            lock.unlock();
            call.waiting_user();
            lock.lock();
            continue;
        }
        if (call.cancelled) {
            run->cancelled = true;
            if (run->dialog) PostMessageW(run->dialog, WM_CLOSE, 0, 0);
            return Cancelled();
        }
        if (run->dialog && !run->timed_out && GetTickCount64() - run->dialog_since > kMcpQuestionMs) {
            // 2 minutes without an answer: a denial (the run reports user_timeout).
            run->timed_out = true;
            PostMessageW(run->dialog, WM_CLOSE, 0, 0);
        }
        run->cv.wait_for(lock, std::chrono::milliseconds(50));
    }
    finished = run;
    return run->outcome;
}

} // namespace pulse::app::ai

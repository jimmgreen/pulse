#include "../ai_host/mcp_tools.h"
#include "../app/actions/action_policy.h"
#include "../common/ai_action_request.h"
#include <cstdio>
using namespace pulse;
namespace {
int failures = 0;
void Check(bool value, const char *message) {
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", message);
    if (!value)
        ++failures;
}
json::Value Request(std::string_view action) {
    auto step = json::Value::Object();
    step.set("action", std::string(action));
    auto steps = json::Value::Array();
    steps.push(std::move(step));
    auto request = json::Value::Object();
    request.set("steps", std::move(steps));
    return request;
}
} // namespace
int main() {
    const auto *tool = ai_host::FindTool("run_actions");
    Check(tool != nullptr, "real MCP run_actions definition exists");
    if (!tool)
        return 1;
    auto valid = Request("nav.up");
    Check(ai::ValidateActionRequest(valid).empty() && ai_host::ValidateArguments(*tool, valid).empty(),
          "ordinary action request remains accepted at host and MCP boundaries");
    for (const char *claim : {"principal", "caller", "owner", "package", "ticket", "capability", "client"}) {
        auto forged = valid;
        forged.set(claim, "sample.transfer-workflow");
        Check(!ai::ValidateActionRequest(forged).empty() && !ai_host::ValidateArguments(*tool, forged).empty(),
              "claimed authority is rejected before host dispatch at both boundaries");
        auto step = json::Value::Object();
        step.set("action", "file.copy_to");
        auto args = json::Value::Object();
        args.set(claim, "forged");
        step.set("args", std::move(args));
        auto steps = json::Value::Array();
        steps.push(valid.find("steps")->at(0));
        steps.push(std::move(step));
        auto mixed = json::Value::Object();
        mixed.set("steps", std::move(steps));
        Check(!ai::ValidateActionRequest(mixed).empty(),
              "later forged step rejects whole batch before earlier valid step");
    }
    auto budget = json::Value::Object();
    auto steps = json::Value::Array();
    for (int i = 0; i < 13; ++i)
        steps.push(valid.find("steps")->at(0));
    budget.set("steps", std::move(steps));
    Check(!ai::ValidateActionRequest(budget).empty() && !ai_host::ValidateArguments(*tool, budget).empty(),
          "oversized action request is rejected rather than silently truncated");
    for (const char *action : {"workflow.run", "workflow.cancel", "resources.read", "extensions.invoke"})
        Check(!ai::ValidateActionRequest(Request(action)).empty() &&
                  app::actions::AiPolicy(action).gate == app::actions::Gate::Locked,
              "extension workflow and resource names grant no AI action authority");
    Check(app::actions::AiPolicy("file.copy_to").gate == app::actions::Gate::Plan &&
              app::actions::AiPolicy("file.recycle").gate == app::actions::Gate::Plan &&
              app::actions::AiPolicy("file.delete_permanent").gate == app::actions::Gate::Plan,
          "actual file-operation policy still requires a user plan");
    Check(app::actions::AiPolicy("nav.open", L"C:\\fixture\\program.exe").gate == app::actions::Gate::Confirm &&
              app::actions::AiPolicy("shell.run_verb").gate == app::actions::Gate::Confirm,
          "actual external-action policy still requires confirmation");
    Check(app::actions::AiPolicy("nonexistent.action").gate == app::actions::Gate::Locked,
          "unknown action cannot fall back to extension execution");
    auto malformed = Request("nav.up");
    malformed.set("steps", "not-an-array");
    Check(!ai::ValidateActionRequest(malformed).empty(), "malformed batch rejected before any UI scheduling");
    return failures ? 1 : 0;
}

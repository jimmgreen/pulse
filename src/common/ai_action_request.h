#pragma once
#include "json_value.h"
#include <string>
#include <string_view>
namespace pulse::ai {
// Shared host/MCP envelope boundary. Action arguments never carry extension authority.
inline bool IsAuthorityClaim(std::string_view key) {
    return key == "principal" || key == "caller" || key == "owner" || key == "package" || key == "ticket" ||
           key == "capability" || key == "client";
}
inline std::string ValidateActionSteps(const json::Value &steps) {
    if (!steps.is_array() || steps.size() == 0)
        return "steps must be a nonempty array";
    if (steps.size() > 12)
        return "steps has too many steps (at most 12)";
    for (size_t index = 0; index < steps.size(); ++index) {
        const auto &step = steps.at(index);
        const auto at = "steps[" + std::to_string(index) + "]";
        if (!step.is_object())
            return at + " must be an object {action, args}";
        for (const auto &key : step.keys())
            if (key != "action" && key != "args" && key != "wait")
                return at + ": unknown member " + key;
        const auto *action = step.find("action");
        if (!action || !action->is_string() || action->as_string().empty() || action->as_string().size() > 64 ||
            action->as_string().find('\0') != std::string::npos)
            return at + ".action must be an action identifier of 1..64 bytes";
        const auto &id = action->as_string();
        if (id.starts_with("workflow.") || id.starts_with("extensions.") || id.starts_with("resources."))
            return at + ": package-bound workflows and resources are not AI actions";
        if (const auto *args = step.find("args")) {
            if (!args->is_object())
                return at + ".args must be an object";
            for (const auto &key : args->keys())
                if (IsAuthorityClaim(key))
                    return at + ".args cannot supply caller identity or capability authority";
        }
        if (const auto *wait = step.find("wait")) {
            if (!wait->is_string() || (wait->as_string() != "listing_loaded" && wait->as_string() != "op_finished" &&
                                       wait->as_string() != "preview_open"))
                return at + ".wait is not a supported host completion condition";
        }
    }
    return {};
}
inline std::string ValidateActionRequest(const json::Value &args) {
    if (!args.is_object())
        return "arguments must be an object";
    for (const auto &key : args.keys())
        if (key != "steps")
            return "unknown argument " + key;
    const auto *steps = args.find("steps");
    return steps ? ValidateActionSteps(*steps) : "steps is required";
}
} // namespace pulse::ai

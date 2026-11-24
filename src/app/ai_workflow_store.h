// ai_workflow_store.h - Reusable local workflow rules (%LOCALAPPDATA%\Pulse\ai\workflows.json).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pulse::app::ai {

struct WorkflowRule {
    std::string id;
    std::string name;
    std::string description;
    std::string steps;       // reusable instructions / rule template
    int64_t created = 0;
};

std::wstring WorkflowsFilePath(const std::wstring& dir_override = {});
std::vector<WorkflowRule> LoadWorkflows(const std::wstring& dir_override = {});
bool SaveWorkflows(const std::vector<WorkflowRule>& rules, const std::wstring& dir_override = {});
bool UpsertWorkflow(const std::string& name, const std::string& description, const std::string& steps,
                    WorkflowRule& out_rule, const std::wstring& dir_override = {});
size_t RemoveWorkflow(const std::string& id_or_name, const std::wstring& dir_override = {});
std::string FormatWorkflowsForPrompt(const std::vector<WorkflowRule>& rules);

} // namespace pulse::app::ai

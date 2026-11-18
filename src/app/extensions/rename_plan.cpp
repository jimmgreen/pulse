#include "rename_plan.h"
#include "../../common/rename_identity.h"
#include <filesystem>
#include <unordered_set>
namespace pulse::extensions {
namespace rn = app::rename;
std::string ValidateRenameDisk(RenameDiskPlan &plan, bool again, const std::function<bool()> &authorized) {
    if (!authorized || !authorized())
        return "cancelled";
    if (plan.sources.empty() || plan.sources.size() > 64 || plan.names.size() != plan.sources.size() ||
        (again && plan.identities.size() != plan.sources.size()))
        return "invalid_request";
    std::unordered_set<std::wstring> selected, existing;
    for (const auto &source : plan.sources)
        selected.insert(rn::Lower(source.name));
    for (const auto &name : plan.names) {
        if (!authorized())
            return "cancelled";
        if (!rn::IsValidComponent(name))
            return "invalid_name";
        const auto key = rn::Lower(name);
        if (selected.contains(key))
            continue;
        const DWORD attributes = GetFileAttributesW((std::filesystem::path(plan.directory) / name).c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES)
            existing.insert(key);
        else if (const DWORD error = GetLastError(); error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
            return "directory_unavailable";
    }
    std::vector<ops::RenameIdentity> identities;
    for (size_t i = 0; i < plan.sources.size(); ++i) {
        if (!authorized())
            return "cancelled";
        ops::RenameIdentity identity;
        if (ops::CaptureRenameIdentity(plan.sources[i].path, identity))
            return "stale_reference";
        if (again &&
            (!(identity.source == plan.identities[i].source) || !(identity.parent == plan.identities[i].parent)))
            return "stale_reference";
        identities.push_back(identity);
    }
    rn::Options options;
    options.policy = rn::Policy::Block;
    const auto preview =
        rn::RunNames(plan.sources, plan.names, options,
                     [&](const std::wstring &, const std::wstring &name) { return existing.contains(name); });
    if (preview.blocked)
        return "rename_conflict";
    for (size_t i = 0; i < preview.rows.size(); ++i) {
        const auto &row = preview.rows[i];
        // The host planner holds cycles at their original names without setting
        // plan.blocked. Preserve that conflict instead of reporting a bad leaf.
        if (row.status == rn::RowStatus::Conflict || row.why == rn::Why::ClashCycle)
            return "rename_conflict";
        if (row.new_name != plan.names[i] || row.status == rn::RowStatus::Invalid || row.status == rn::RowStatus::Empty)
            return "invalid_name";
    }
    if (!preview.renames)
        return "no_changes";
    if (!again) {
        plan.identities = std::move(identities);
        plan.preview = preview;
    } else if (preview.exec_order != plan.preview.exec_order)
        return "stale_plan";
    return {};
}
} // namespace pulse::extensions

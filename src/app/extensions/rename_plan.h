#pragma once
#include "../../common/rename_identity_types.h"
#include "../batch_rename_plan.h"
#include <functional>
namespace pulse::extensions {
struct RenameDiskPlan {
    std::wstring directory;
    std::vector<app::rename::Source> sources;
    std::vector<std::wstring> names;
    std::vector<ops::RenameIdentity> identities;
    app::rename::Plan preview;
};
// Worker only: validates identities and conflicts; does not mutate the filesystem.
std::string ValidateRenameDisk(RenameDiskPlan &plan, bool again, const std::function<bool()> &authorized);
} // namespace pulse::extensions

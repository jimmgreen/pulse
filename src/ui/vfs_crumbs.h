#pragma once

#include <string>

namespace pulse::ui {

struct VfsCrumbInfo {
    std::wstring display_name;
    bool known = false;
};

using VfsCrumbResolver = VfsCrumbInfo (*)(const std::string& package_id, const std::string& mount_id);
void SetVfsCrumbResolver(VfsCrumbResolver resolver);

} // namespace pulse::ui

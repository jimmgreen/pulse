#pragma once

#include "associations_adapter.h"
#include <windows.h>
#include <string>
#include <string_view>

namespace pulse::extensions::native {

AssociationsBackend MakeAssociationsBackend(HWND owner, std::string package_id, std::wstring package_dir);
bool RemovePackageAssociationsOnHost(std::string_view package_id);
void EnqueuePackageAssociationsCleanupOnHost(std::string_view package_id);
void ShutdownAssociationsHost();

} // namespace pulse::extensions::native

#pragma once
#include <string>
namespace pulse::app_runtime_paths {
// The first versioned call verifies and leases its immutable bundle. Prewarm
// before creating UI; subsequent calls return cached process-local paths.
std::wstring RuntimeDirectory();
std::wstring StableEntryPath();
std::wstring InstallRoot();
}

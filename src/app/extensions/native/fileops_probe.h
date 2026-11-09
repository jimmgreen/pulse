#pragma once
#include <string>
namespace pulse { struct AppState; }
namespace pulse::extensions::native {
bool RunFileOpsProbe(AppState& owner, const std::wstring& directory);
}

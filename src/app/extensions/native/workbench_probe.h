#pragma once
#include <string>
namespace pulse { struct AppState; }
namespace pulse::extensions::native {
bool RunWorkbenchProbe(AppState& owner, const std::wstring& directory);
}

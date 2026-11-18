#pragma once
#include <string>
namespace pulse { struct AppState; }
namespace pulse::extensions::native {
bool RunTargetProbe(AppState& owner, const std::wstring& directory);
}

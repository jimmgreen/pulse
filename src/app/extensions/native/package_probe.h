#pragma once
#include <string>
namespace pulse { struct AppState; }
namespace pulse::extensions::native {
bool RunPackageProbe(AppState& owner, const std::wstring& output);
}

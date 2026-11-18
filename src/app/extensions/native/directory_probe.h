#pragma once
#include <string>
namespace pulse { struct AppState; }
namespace pulse::extensions::native {
bool RunDirectoryProbe(AppState &owner, const std::wstring &directory);
}

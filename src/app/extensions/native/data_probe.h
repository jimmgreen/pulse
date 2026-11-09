#pragma once
#include <string>
namespace pulse { struct AppState; }
namespace pulse::extensions::native { bool RunDataProbe(AppState &, const std::wstring &); }

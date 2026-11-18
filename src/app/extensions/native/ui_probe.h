#pragma once
#include <string>
namespace pulse { struct AppState; }
namespace pulse::extensions::native { bool RunUiProbe(AppState&, const std::wstring&); }

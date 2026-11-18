#pragma once
#include <string>
namespace pulse { struct AppState; }
namespace pulse::extensions::native { bool RunReactiveProbe(AppState&, const std::wstring&); }

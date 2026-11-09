#pragma once
#include <string>
namespace pulse { struct AppState; }
namespace pulse::extensions::native { bool RunInspectorProbe(AppState&, const std::wstring&); }

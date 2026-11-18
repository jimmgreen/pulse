#pragma once
#include <string>
namespace pulse { struct AppState; }
namespace pulse::extensions::native { bool RunAuthorProbe(AppState&, const std::wstring&); }

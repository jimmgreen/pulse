#pragma once
#include <iosfwd>
#include <string>
namespace pulse { struct AppState; }
namespace pulse::extensions::native {
bool RunArchiveTaskProbe(AppState&, const std::wstring& output, std::ostream& report);
}

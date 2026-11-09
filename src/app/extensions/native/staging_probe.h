#pragma once
#include <string>
#include <iosfwd>
namespace pulse { struct AppState; }
namespace pulse::extensions::native {
bool RunArchiveStagingProbe(AppState&, const std::wstring&, std::ostream&);
}

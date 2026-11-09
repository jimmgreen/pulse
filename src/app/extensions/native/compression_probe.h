#pragma once
#include <string>
#include <iosfwd>
namespace pulse { struct AppState; }
namespace pulse::extensions::native {
bool RunArchiveCompressionProbe(AppState&, const std::wstring&, std::ostream&);
}


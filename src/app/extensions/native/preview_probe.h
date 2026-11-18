#pragma once
#include <string>
#include <iosfwd>
namespace pulse { struct AppState; }
namespace pulse::extensions::native {
bool RunArchivePreviewProbe(AppState& owner, const std::wstring& output, std::ostream& report);
}

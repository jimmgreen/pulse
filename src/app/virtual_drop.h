#pragma once

#include <shlobj.h>
#include <string>
#include <vector>

namespace pulse {

struct AppState;

DWORD ResolveVirtualFileDrop(AppState& s, const std::vector<std::wstring>& names, POINT pt, DWORD keys, DWORD allowed);
DWORD ExecuteVirtualFileDrop(AppState& s, const std::vector<std::wstring>& names, std::vector<FILEDESCRIPTORW> files,
                             IDataObject* data, POINT pt, DWORD keys, DWORD allowed);

} // namespace pulse

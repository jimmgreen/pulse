// selection_target.cpp - directory selected file operations should target.
#include "selection_target.h"

namespace pulse::app {

std::wstring SelectionTargetDirectory(const std::wstring& current_path,
                                      const std::wstring& selected_path,
                                      bool selected_is_directory) {
    if (selected_path.empty() || selected_is_directory)
        return selected_path.empty() ? current_path : selected_path;

    std::wstring path = selected_path;
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos || slash == 0) return current_path;
    std::wstring parent = path.substr(0, slash);
    if (parent.size() == 2 && parent[1] == L':') parent += L'\\';
    return parent.empty() ? current_path : parent;
}

} // namespace pulse::app

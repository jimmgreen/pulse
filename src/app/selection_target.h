// selection_target.h - directory selected file operations should target.
#pragma once

#include <string>

namespace pulse::app {

// A selected directory is the destination itself; a selected file targets its
// parent. An empty selected_path means that the current directory is used.
std::wstring SelectionTargetDirectory(const std::wstring& current_path,
                                      const std::wstring& selected_path,
                                      bool selected_is_directory);

} // namespace pulse::app

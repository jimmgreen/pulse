#pragma once

#include <string>
#include <vector>

namespace pulse::app {

// The taskbar button belongs to this identity. It has to match the one the
// installer puts on the shortcuts, otherwise the shell treats a pinned button
// and a running window as two different applications.
inline constexpr wchar_t kAppUserModelId[] = L"Pulse.FileManager";

// Rebuilds the taskbar jump list: the folders pinned to quick access, each one
// opening its own window, plus the "new window" task. Called while the pinned
// set changes, so a right click on the taskbar button shows what is pinned now.
void RefreshJumpList(const std::vector<std::wstring>& pinned_folders);

} // namespace pulse::app

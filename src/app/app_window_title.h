#pragma once
#include <string>

namespace pulse {
struct AppState;
namespace app { struct Tab; }

std::wstring TaskbarWindowTitle(const app::Tab* tab);
// Changes the native caption only when the active folder's display name changes.
bool SyncTaskbarWindowTitle(AppState& s);
} // namespace pulse

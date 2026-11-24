#pragma once
#include <windows.h>

namespace pulse {
struct AppState;
struct SidebarRefreshLoad;
struct SidebarKnownFolderOpen;
enum class SidebarFolderOpenMode { Current, NewTab, BackgroundTab };
bool OpenSidebarKnownFolder(AppState& state, int builtin, SidebarFolderOpenMode mode);
void RequestSidebarRefresh(AppState& state, bool rebuild = false);
bool TickSidebarRefresh(AppState& state, ULONGLONG now);
void CancelSidebarRefresh(AppState& state);
// Phone rows (DeviceRegistry) into state.sidebar.devices; true when changed.
bool SyncDeviceSidebar(AppState& state);
} // namespace pulse

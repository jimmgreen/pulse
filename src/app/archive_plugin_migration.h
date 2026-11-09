#pragma once
#include <windows.h>

namespace pulse {
// Performs filesystem I/O: call once during startup on a worker, then show the
// notice on the UI thread only when true. Old package contents remain untouched.
bool TakeArchivePluginMigrationNotice();
// At most one claim per installation; a crash before display can consume it.
void StartArchivePluginMigrationNotice(HWND window);
UINT ArchivePluginMigrationNoticeMessage();
bool IsArchivePluginMigrationNoticeTarget(HWND window, WPARAM token);
}

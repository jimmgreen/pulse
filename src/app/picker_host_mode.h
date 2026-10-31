#pragma once

namespace pulse::app {
// Called after OLE/localization initialization, before main application state.
int RunPickerHostMode(const wchar_t* pipe_name, const wchar_t* parent_pid);
}

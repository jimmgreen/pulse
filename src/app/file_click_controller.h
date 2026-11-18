#pragma once
#include "file_click_gesture.h"
#include <windows.h>
namespace pulse {
struct AppState;
app::FileClickIdentity FileClickIdentityFor(AppState& s, int index);
void CancelFileClick(AppState& s);
void TickFileClick(AppState& s, ULONGLONG now);
void ExecuteFileClickOpen(AppState& s, const app::FileClickIdentity& item, bool shift, bool single);
}

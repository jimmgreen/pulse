#pragma once
#include <windows.h>
namespace pulse {
struct AppState;
namespace app { struct Tab; }
namespace ui { struct PaneViewModel; }
inline constexpr UINT_PTR kPickerSaveWatchTimer = 0x50535754;
bool HandlePickerSaveWatch(AppState&, const COPYDATASTRUCT*);
void TickPickerSaveWatch(AppState&);
bool HandlePickerSavedAction(AppState&, UINT message, WPARAM);
void StopPickerSaveWatch(AppState&);
void FillPickerSavedBadges(AppState&, app::Tab&, ui::PaneViewModel&);
}
#ifdef PULSE_WITH_TEST_HOOKS
namespace pulse { int RunPickerSavedFollowupProbe(AppState&, const wchar_t* report_path); }
#endif

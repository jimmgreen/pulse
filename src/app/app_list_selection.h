#pragma once
#include <windows.h>
#include <d2d1.h>

namespace pulse {
struct AppState;
namespace app { struct Tab; }

void CancelListSelection(AppState& s);
void ResetMarquee(AppState& s);
bool CancelMarquee(AppState& s);
bool FilePressOwnerMatches(AppState& s, const app::Tab* tab);
bool MarqueeOwnerMatches(const AppState& s, const app::Tab* tab);
void BeginListBand(AppState& s, app::Tab* tab, POINT point, int row, bool ctrl, bool shift);
void BeginListFilePress(AppState& s, int row, bool ctrl, bool shift, POINT point);
int FinishListBand(AppState& s, POINT point);
void ApplyMarqueeSelection(AppState& s);
POINT MarqueeStartOnScreen(const AppState& s);
D2D1_RECT_F MarqueeDisplayRect(const AppState& s);
void UpdateMarqueeAutoScroll(AppState& s);
void TickMarqueeAutoScroll(AppState& s);
void UpdateMarqueeFrame(AppState& s);
void HandleListRowClick(AppState& s, int index, bool ctrl, bool shift);
void FinishListRowClick(AppState& s);
void CommitListRowClick(AppState& s, int index, bool ctrl, bool shift);
} // namespace pulse

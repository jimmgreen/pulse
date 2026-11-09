#pragma once
// Photo wall suggestion (P2 maybeSuggest):
// the first time a photo folder (70% or more pictures) shows as an icon grid,
// a card under the pane header offers the wall, once per folder per session.
// It hides after 9 s, on Switch, on "don't show again", or when the folder or
// view changes. UI thread only.
#include <windows.h>

namespace pulse::ui { struct PaneViewModel; }
namespace pulse::app { struct Tab; }

namespace pulse {
struct AppState;

constexpr UINT_PTR kTimerWallHint = 0x5748;  // 'WH': the suggestion's 9 s are up

// Each frame before the models are built: offer or withdraw the card.
void UpdateWallHint(AppState& s);
// Copies the card's timing into the model of the pane showing `tab`.
void FillWallHint(const app::Tab& tab, ui::PaneViewModel& out);
void WallHintTimer(AppState& s);
// The card's buttons: the wall now, or never again (app.json photo_wall_hint).
void WallHintSwitch(AppState& s);
void WallHintNever(AppState& s);

}  // namespace pulse

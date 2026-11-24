// app_hover_preview.h - The hover preview card (Settings > 快速预览 >
// 预览方式 = 悬停预览): resting the pointer on a file shows a small card with
// its preview and key facts; Space opens that file in quick look.
#pragma once
#include <windows.h>
#include <string>

namespace pulse {
struct AppState;
namespace ui { struct WindowViewModel; struct HoverPreviewView; }

constexpr UINT_PTR kTimerHoverPreview = 0x4850;       // 'HP': the dwell before the card
constexpr UINT_PTR kTimerHoverPreviewFrame = 0x4851;  // repaints while the card animates
constexpr UINT_PTR kTimerHoverPrefetch = 0x4852;      // the pointer settled: start decoding

// The hovered row changed (app_hover.cpp): hides the card and starts the
// dwell for the new row, sooner when moving on from a card that was shown.
void NoteHoverPreviewRow(AppState& s);
// Hides the card and cancels a pending dwell.
void HideHoverPreview(AppState& s);
// The pointer (client px) is on a folder's card, or in the gap between it
// and its row: the card keeps it, the rows under it get no hover.
bool HoverPreviewHoldsPointer(AppState& s, POINT point);
// Dwell timer, and dismissal on clicks, wheel, keys and focus loss. Returns
// true only for messages it consumes (its own timer).
bool HoverPreviewMessage(AppState& s, HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
// Space while the card shows: its file is selected and opens in quick look.
// False when no card is shown (Space then previews the selection as before).
bool OpenHoverPreviewInQuickLook(AppState& s);
// This frame's card for the renderer; a card whose row is gone or scrolled
// away is dropped.
// GUI verification (--shot-hover-preview N): shows the card over the Nth file
// (0-based, folders skipped) of the focused pane, pointer at the name; with
// PULSE_TEST_HOVER_FOLDER=1 over the Nth folder instead.
bool StageHoverPreviewShot(AppState& s, int nth_file);
// --shot-hover-dwell N: the dwell line on that row, 60% through.
bool StageHoverDwellShot(AppState& s, int nth_file);
// Next to a --shot-hover-preview image: <shot>.video.txt with the card
// video's state (its child window is not in the composed snapshot).
void WriteHoverVideoProbe(AppState& s, const std::wstring& shot_output);
// --bench-hover (env PULSE_BENCH_HOVER_LIST / _OUT): times each listed file's
// card content and video start; false when not asked for.
bool RunHoverBench(AppState& s);
void FillHoverPreviewView(AppState& s, const ui::WindowViewModel& vm, ui::HoverPreviewView& view);
// Keep parent-drawn overlays in front of the embedded Details preview HWND.
void SyncDetailsPreviewOcclusion(AppState& s);
// After the card was drawn: keeps or stops the repaint timer it asked for.
void AfterHoverPreviewDraw(AppState& s);
} // namespace pulse

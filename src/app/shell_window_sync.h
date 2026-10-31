// shell_window_sync.h — Keeps ShellWindowRegistry in step with Pulse's panes
// and carries out the shell's "select this item" requests (B站 #1 phase 2a).
#pragma once
#include "explorer_window_takeover.h"
#include "shell_window_plan.h"
#include "open_routing.h"
#include "explorer_open_context.h"
#include <vector>
#include <cstdint>

namespace pulse {
struct AppState;
namespace app { struct Tab; }
namespace ui { struct WindowViewModel; }
struct ExplorerFrameTab { app::Tab* tab = nullptr; uint64_t generation = 0, selection_revision = 0; };
using ExplorerFrame = std::vector<ExplorerFrameTab>;

// After each frame: publishes the panes' folders when they changed (while
// Pulse opens folders) and starts/stops the experimental Explorer window
// takeover with its setting. Never in shot or test runs.
void SyncShellWindows(AppState& s);
// WM_DESTROY: revokes every registration, stops the takeover.
void StopShellWindows(AppState& s);
// WM_SHELL_SELECT: brings the pane forward and selects the item.
void HandleShellSelect(AppState& s, const app::ShellSelectRequest& request);
// Owned queue execution; Deferred has no navigation side effects. Presented alone authorizes Ready.
ExternalOpenResult HandleExplorerTakeover(AppState& s, const app::ExplorerOpenContext& request);
void CompleteExplorerNavigation(app::Tab& tab, uint64_t generation, bool success);
ExplorerFrame CaptureExplorerFrame(AppState& s, const ui::WindowViewModel& vm);
void CompleteExplorerFrame(AppState& s, const ExplorerFrame& frame, bool presented);
void TickExplorerPresentation(AppState& s);
// Adds `name` to the tab's selection now when listed, else once its listing
// lands (a program selecting several files; AI reveal, ai_tools_ui.h).
void AddNameToSelection(AppState& s, app::Tab& tab, const std::wstring& name);

} // namespace pulse
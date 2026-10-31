#pragma once
namespace pulse {
struct AppState;
struct DialogManagementUiState;
namespace ui { struct WindowViewModel; }
void DialogManagementReady(AppState& state);
void DialogManagementAction(AppState& state, int action);
void TickDialogManagement(AppState& state);
void FillDialogManagementVm(AppState& state, ui::WindowViewModel& vm);
}

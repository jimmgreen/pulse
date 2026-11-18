#pragma once
#include "explorer_source_scope.h"
namespace pulse {
struct AppState;
namespace app { struct Tab; }
void ExplorerTakeoverProbeDelivery(AppState& state, const std::shared_ptr<app::ExplorerHandoff>& handoff, const char* stage);
void ExplorerTakeoverProbeNavigation(AppState& state, const app::Tab* tab, const char* reason, unsigned detail = 0);
// Call only with the actual ExplorerNavigationLease::Presented return value,
// after background preparation and confirmed source close, never for Prepared.
void ExplorerTakeoverProbePresented(AppState& state, const app::Tab* tab, bool acknowledged);
bool ExplorerTakeoverProbeRequested(const AppState& state);
std::shared_ptr<const app::ExplorerSourceScope> ExplorerTakeoverProbeScope(const AppState& state);
void InitializeExplorerTakeoverProbe(AppState& state);
void TickExplorerTakeoverProbe(AppState& state);
}

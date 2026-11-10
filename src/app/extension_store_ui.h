#pragma once
namespace pulse { struct AppState; struct AppCore; }
namespace pulse::ui { struct WindowViewModel; }
namespace pulse {
void FillExtensionStoreView(AppState& state, ui::WindowViewModel& vm);
bool ExtensionStoreAction(AppState& state, int action);
void TickExtensionStoreUi(AppState& state);
void ReleaseExtensionStoreOwner(AppState& state);
void StopExtensionStore(AppCore& core);
}

#pragma once
#include <string>
#include <string_view>

namespace pulse { struct AppState; struct AppCore; }
namespace pulse::ui { struct WindowViewModel; }
namespace pulse {
void FillExtensionStoreView(AppState& state, ui::WindowViewModel& vm);
bool ExtensionStoreAction(AppState& state, int action);
bool IsExtensionPackageFilePath(std::wstring_view path);
bool OpenExtensionPackageInStore(AppState& state, const std::wstring& path);
void TickExtensionStoreUi(AppState& state);
void ReleaseExtensionStoreOwner(AppState& state);
void StopExtensionStore(AppCore& core);
}

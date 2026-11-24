#pragma once
#include <filesystem>
#include <string>
#include <vector>
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
// The store root the install/management services use (empty when unavailable).
std::filesystem::path ExtensionStoreRoot(AppState& state);
// Ids a developer folder may not use: store registry entries and loaded store packages.
std::vector<std::string> DeveloperTakenIds(AppState& state);
}
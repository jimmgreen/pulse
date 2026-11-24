// extension_developer_mode.h — 开发者模式 in 设置 → 扩展 → 已安装 (devmode_plan.md M1/M2).
// The setting and the developer folder registry live in extensions-dev next to the
// store (extensions/developer_mode.h); the store, its registry and its signature
// policy are never written here. Every change takes effect after a quick restart.
#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace pulse {
struct AppState;
namespace ui { struct ExtensionStoreView; }
namespace app {
// Cached saved setting; first call reads extensions-dev/developer-mode.json.
bool DeveloperModeSetting(const std::filesystem::path& store_root);
bool SaveDeveloperModeSetting(const std::filesystem::path& store_root, bool enabled, std::string& error);

// Copies folder (manifest.json + the native files it declares, plus the DLL's .pdb) into
// extensions-dev/<id>/<revision>/, re-hashing the copied files into the shadow manifest so a
// rebuilt DLL needs no new native-manifest run. Refuses ids in taken_ids. Returns the id.
struct DeveloperFolderResult { std::string id; std::string error; std::wstring detail; };
DeveloperFolderResult AddDeveloperFolder(const std::filesystem::path& store_root, const std::filesystem::path& folder,
    const std::vector<std::string>& taken_ids);
bool RemoveDeveloperFolder(const std::filesystem::path& store_root, const std::string& id, std::string& error);
std::wstring LocalizedDeveloperError(const std::string& error, const std::wstring& detail = {});

// UI glue: rows appended to view.installed (after the store registry entries), the
// 开发者 button menu, and the 更多 menu of a developer row.
void FillDeveloperRows(AppState& s, const std::filesystem::path& store_root, ui::ExtensionStoreView& view);
bool DeveloperButtonAction(AppState& s, const std::filesystem::path& store_root, const std::vector<std::string>& taken_ids);
bool DeveloperRowMenu(AppState& s, const std::filesystem::path& store_root, int development_index,
    const std::vector<std::string>& taken_ids);
}
}

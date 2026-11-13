// extension_store_seven_zip.h - the official 7-Zip extension inside the store: its
// feature card and detail page read the extension's own settings page states and
// edit them the way the page would, so the extension validates, saves and renames
// its commands itself. Nothing here knows the extension's storage format.
#pragma once
#include <string>

namespace pulse { struct AppState; }
namespace pulse::ui { struct ExtensionStoreView; }

namespace pulse {
inline constexpr char kSevenZipExtensionId[] = "pulse.7zip";
// Fills view.seven_zip (items and installed must already be filled).
void FillSevenZipView(AppState& s, ui::ExtensionStoreView& view);
// One setting, as a user edit on the extension's page: "mode", "location",
// "format", "level" (digits), "remember" (true/false), "folder" (a path),
// "placement0"/"placement1" (radio), "cmd0".."cmd5" (true/false).
bool SetSevenZipState(AppState& s, const std::string& key, const std::string& value);
// The extension's own folder chooser for 固定文件夹.
bool BrowseSevenZipFolder(AppState& s);
// A button of the extension's page: "ask0".."ask11" (Windows' chooser for one format),
// "asknext" (逐个确认), "winsettings" (Windows' Default apps).
bool ClickSevenZip(AppState& s, const std::string& command);
}

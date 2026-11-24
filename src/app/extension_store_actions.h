#pragma once
#include <string_view>
namespace pulse::ui { struct ExtensionStoreView; }
namespace pulse {
struct AppState;
int ExtensionDetailInstalledIndex(const ui::ExtensionStoreView& view, std::wstring_view focused_id);
bool ExtensionStoreDetailAction(AppState& state, int action);
}

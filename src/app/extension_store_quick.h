#pragma once

namespace pulse { struct AppState; }
namespace pulse::ui { struct ExtensionStoreView; }

namespace pulse {

void FillGenericQuickCards(AppState& s, ui::ExtensionStoreView& view);
bool HandleGenericQuickControlAction(AppState& s, const ui::ExtensionStoreView& view, int action);

} // namespace pulse

#pragma once
#include <cstddef>
#include <string>
namespace pulse { struct AppState; }
namespace pulse::extensions {
struct PanelContext {
    std::wstring directory;
    size_t selection_count = 0;
    std::wstring selection_name;
    std::wstring view_mode;
    std::wstring transfer_status;
};
// UI-thread snapshot of already loaded state. Never opens a file or enumerates selection.
PanelContext CapturePanelContext(AppState& state);
}

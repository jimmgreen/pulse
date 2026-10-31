#pragma once
#include "pane_drop.h"
namespace pulse {
struct AppState;
struct ClosedPane {
    app::LayoutTab* owner = nullptr;
    std::unique_ptr<app::LayoutTab> contents;
    app::PaneOrigin origin;
    std::vector<app::Pane*> remaining;
};
bool CloseWorkspacePane(AppState& s, app::Pane* pane);
bool ReopenWorkspacePane(AppState& s);
}

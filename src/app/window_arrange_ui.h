#pragma once
#include "window_arrange.h"
namespace pulse {
struct AppState;
std::vector<HWND> WindowArrangementCandidates(AppState &s, bool all_monitors = false,
    const std::vector<HWND> &order = {}, bool force_include = false);
bool ArrangeWindows(AppState &s, app::WindowArrangeLayout layout, bool all_monitors = false,
                    const std::vector<HWND> &order = {}, bool force_include = false);
WPARAM WindowArrangeGeneration();
void UndoWindowArrange(AppState &s, WPARAM generation = 0);
void SetWindowArrangeAnimations(bool enabled);
} // namespace pulse

namespace pulse {
void CleanupWindowArrange();
}
